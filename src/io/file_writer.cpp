// file_writer implementation: the write-side mirror of file_reader.cpp,
// on the same raw-syscall io_uring plumbing (uring_impl.hpp). The flow
// inverts: instead of the kernel filling buffers ahead of the consumer,
// the producer fills buffers ahead of the device. acquire() blocking on a
// slot whose write is still in flight is the entire backpressure story.
// Degradation ladder as on the read side: O_DIRECT refused -> buffered
// io_uring; io_uring refused -> synchronous pwrite; non-POSIX -> stdio.

#include <blake3pp/detail/file_writer.hpp>

#include <cerrno>
#include <cstring>
#include <new>
#include <system_error>
#include <vector>

#include "io/uring_impl.hpp"

#if !defined(BLAKE3PP_IO_POSIX)
#include <cstdio>
#endif

namespace blake3pp::detail {

using io_impl::direct_align;
using io_impl::throw_errno;
#if defined(BLAKE3PP_IO_URING)
using io_impl::uring;
#endif

struct file_writer::impl {
  std::size_t buffer = 0;
  unsigned qd = 0;
  unsigned next_slot = 0;      // round-robin acquire order
  std::uint64_t offset = 0;    // next sequential file offset
  std::uint64_t written = 0;   // total bytes accepted via submit()
  std::uint64_t prealloc = 0;  // file size set by fallocate, if any
  bool tail_submitted = false; // a partial submit closes the stream
  std::byte* pool = nullptr;
  std::size_t pool_sz = 0;
  const char* backend_name = "unknown";

  struct slot_state {
    std::uint64_t off = 0;  // file offset of this write
    std::size_t len = 0;    // bytes to write
    std::size_t done = 0;   // bytes completed so far (async)
    bool busy = false;      // write in flight
  };
  std::vector<slot_state> slots;

#if defined(BLAKE3PP_IO_POSIX)
  int fd = -1;        // main data fd (O_DIRECT when engaged)
  int fd_plain = -1;  // always-buffered fd for the unaligned tail
  bool direct = false;
#else
  std::FILE* stream = nullptr;
#endif
#if defined(BLAKE3PP_IO_URING)
  uring ring;
  bool use_uring = false;
#endif

  std::byte* buf(unsigned slot) const noexcept {
    return pool + static_cast<std::size_t>(slot) * buffer;
  }

#if defined(BLAKE3PP_IO_POSIX)
  void pwrite_all(int use_fd, const std::byte* data, std::size_t len,
                  std::uint64_t off) {
    std::size_t put = 0;
    while (put < len) {
      const ssize_t n = ::pwrite(use_fd, data + put, len - put,
                                 static_cast<off_t>(off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pwrite");
      }
      put += static_cast<std::size_t>(n);
    }
  }
#endif

#if defined(BLAKE3PP_IO_URING)
  // Reaps one completion, issuing a continuation on a short write. The
  // device may complete slots in any order; each carries its slot index.
  void reap_one() {
    const auto [ud, res] = ring.wait_one();
    const unsigned s = static_cast<unsigned>(ud);
    slot_state& st = slots[s];
    if (res <= 0) {
      throw std::system_error(res < 0 ? -res : EIO, std::generic_category(),
                              "io_uring write");
    }
    st.done += static_cast<std::size_t>(res);
    if (st.done < st.len) {
      ring.submit_rw(IORING_OP_WRITE, fd, buf(s) + st.done,
                     static_cast<unsigned>(st.len - st.done),
                     st.off + st.done, s);
    } else {
      st.busy = false;
    }
  }
#endif
};

file_writer::file_writer(const std::filesystem::path& fspath,
                         const file_writer_options& opts)
    : impl_(new impl) {
  impl& im = *impl_;
  // Round up to the O_DIRECT length granule; >= 64 KiB so queued writes
  // are worth their submission cost.
  im.buffer = std::max<std::size_t>(opts.buffer_bytes, 64 * 1024);
  im.buffer = (im.buffer + direct_align - 1) / direct_align * direct_align;
  im.qd = std::min(32u, std::max(2u, opts.queue_depth));

#if defined(BLAKE3PP_IO_POSIX)
  const char* const path = fspath.c_str();  // native() is char-based here
  im.fd_plain =
      ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (im.fd_plain < 0) {
    delete impl_;
    throw_errno("open");
  }
  im.fd = im.fd_plain;
#if defined(__linux__)
  // Preallocating turns every write into an overwrite of existing extents.
  // Extending writes serialize on the inode lock; with async direct I/O
  // that collapses the whole queue to one stalled write at a time.
  if (opts.preallocate_bytes > 0 &&
      ::fallocate(im.fd_plain, 0, 0,
                  static_cast<off_t>(opts.preallocate_bytes)) == 0) {
    im.prealloc = opts.preallocate_bytes;
  }
#endif
#if defined(O_DIRECT)
  if (opts.direct_io) {
    // Reopen (not TRUNC, already truncated) so the tail keeps a plain fd.
    const int dfd = ::open(path, O_WRONLY | O_CLOEXEC | O_DIRECT);
    if (dfd >= 0) {
      im.fd = dfd;
      im.direct = true;
    }
  }
#endif
  im.backend_name = im.direct ? "pwrite+direct" : "pwrite";
#if defined(BLAKE3PP_IO_URING)
  if (opts.async && im.ring.init(2 * im.qd)) {
    im.use_uring = true;
    im.backend_name = im.direct ? "io_uring+direct" : "io_uring";
  }
#endif
#else
  im.stream = std::fopen(fspath.string().c_str(), "wb");
  if (im.stream == nullptr) {
    delete impl_;
    throw_errno("fopen");
  }
  im.backend_name = "stdio";
#endif

  im.slots.resize(im.qd);
  im.pool_sz = static_cast<std::size_t>(im.qd) * im.buffer;
  im.pool = static_cast<std::byte*>(
      ::operator new(im.pool_sz, std::align_val_t{direct_align}));
}

file_writer::~file_writer() {
#if defined(BLAKE3PP_IO_URING)
  // Best-effort drain: buffers must outlive in-flight writes. Errors here
  // are unreportable; that is why finish() exists.
  try {
    finish();
  } catch (...) {
  }
  impl_->ring.destroy();
#endif
  impl& im = *impl_;
  if (im.pool != nullptr) {
    ::operator delete(im.pool, std::align_val_t{direct_align});
  }
#if defined(BLAKE3PP_IO_POSIX)
  if (im.direct && im.fd >= 0) {
    ::close(im.fd);
  }
  if (im.fd_plain >= 0) {
    ::close(im.fd_plain);
  }
#else
  if (im.stream != nullptr) {
    std::fclose(im.stream);
  }
#endif
  delete impl_;
}

std::uint64_t file_writer::bytes_written() const noexcept {
  return impl_->written;
}

const char* file_writer::backend() const noexcept {
  return impl_->backend_name;
}

file_writer::buffer file_writer::acquire() {
  impl& im = *impl_;
  const unsigned s = im.next_slot;
#if defined(BLAKE3PP_IO_URING)
  while (im.slots[s].busy) {
    im.reap_one();
  }
#endif
  return buffer{im.buf(s), im.buffer, s};
}

void file_writer::submit(const buffer& b, std::size_t bytes) {
  impl& im = *impl_;
  if (bytes == 0) {
    return;
  }
  if (im.tail_submitted) {
    throw std::system_error(EINVAL, std::generic_category(),
                            "submit after partial write");
  }
  const bool aligned = bytes % direct_align == 0;
  if (!aligned) {
    im.tail_submitted = true;
  }
#if defined(BLAKE3PP_IO_URING)
  if (im.use_uring && aligned) {
    impl::slot_state& st = im.slots[b.slot];
    st.off = im.offset;
    st.len = bytes;
    st.done = 0;
    st.busy = true;
    im.ring.submit_rw(IORING_OP_WRITE, im.fd, b.data,
                      static_cast<unsigned>(bytes), st.off, b.slot);
    im.offset += bytes;
    im.written += bytes;
    im.next_slot = (b.slot + 1) % im.qd;
    return;
  }
#endif
#if defined(BLAKE3PP_IO_POSIX)
  // Synchronous path, and the unaligned tail in every mode: O_DIRECT
  // rejects unaligned lengths, so the tail goes through the plain fd.
  const int use_fd = aligned && im.direct ? im.fd : im.fd_plain;
  im.pwrite_all(use_fd, b.data, bytes, im.offset);
#else
  if (std::fwrite(b.data, 1, bytes, im.stream) != bytes) {
    throw std::system_error(EIO, std::generic_category(), "fwrite");
  }
#endif
  im.offset += bytes;
  im.written += bytes;
  im.next_slot = (b.slot + 1) % im.qd;
}

void file_writer::finish() {
  impl& im = *impl_;
#if defined(BLAKE3PP_IO_URING)
  for (impl::slot_state& st : im.slots) {
    while (st.busy) {
      im.reap_one();
    }
  }
#endif
#if defined(BLAKE3PP_IO_POSIX)
  // fallocate set the file size up front; trim if less was written.
  if (im.prealloc > im.written &&
      ::ftruncate(im.fd_plain, static_cast<off_t>(im.written)) != 0) {
    throw_errno("ftruncate");
  }
  im.prealloc = 0;
#else
  if (std::fflush(im.stream) != 0) {
    throw_errno("fflush");
  }
#endif
  (void)im;
}

}  // namespace blake3pp::detail
