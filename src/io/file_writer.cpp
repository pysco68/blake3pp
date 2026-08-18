// file_writer implementation: the write-side mirror of file_reader.cpp,
// on the same raw-syscall io_uring plumbing (uring_impl.hpp). The flow
// inverts: instead of the kernel filling buffers ahead of the consumer,
// the producer fills buffers ahead of the device. acquire() blocking on a
// slot whose write is still in flight is the entire backpressure story.
// Degradation ladder as on the read side: O_DIRECT refused -> buffered
// io_uring; io_uring refused -> synchronous pwrite. Windows mirrors it
// with IOCP + FILE_FLAG_NO_BUFFERING through iocp_impl.hpp, macOS with
// GCD + F_NOCACHE through darwin_impl.hpp; anything else falls to stdio.

#include <blake3pp/detail/file_writer.hpp>

#include <cerrno>
#include <cstring>
#include <new>
#include <system_error>
#include <vector>

#include "io/darwin_impl.hpp"
#include "io/iocp_impl.hpp"
#include "io/uring_impl.hpp"

#if !defined(BLAKE3PP_IO_POSIX) && !defined(BLAKE3PP_IO_WIN32)
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
#if defined(BLAKE3PP_IO_GCD)
    int error = 0;  // errno captured by the GCD worker; thrown at acquire
#endif
  };
  std::vector<slot_state> slots;

#if defined(BLAKE3PP_IO_POSIX)
  int fd = -1;        // main data fd (O_DIRECT when engaged)
  int fd_plain = -1;  // always-buffered fd for the unaligned tail
  bool direct = false;
#elif defined(BLAKE3PP_IO_WIN32)
  HANDLE h = INVALID_HANDLE_VALUE;        // main (NO_BUFFERING when direct)
  HANDLE h_plain = INVALID_HANDLE_VALUE;  // buffered+sync: tail, fallback
  HANDLE port = nullptr;
  bool direct = false;
  bool use_iocp = false;
  unsigned outstanding = 0;
  std::vector<OVERLAPPED> ovs;  // one per slot
#else
  std::FILE* stream = nullptr;
#endif
#if defined(BLAKE3PP_IO_URING)
  uring ring;
  bool use_uring = false;
#endif
#if defined(BLAKE3PP_IO_GCD)
  struct gcd_task {
    impl* self;
    unsigned slot;
  };
  std::vector<gcd_task> tasks;  // one per slot, fixed at construction
  io_impl::gcd_pump pump;
  bool use_gcd = false;
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

#if defined(BLAKE3PP_IO_WIN32)
  // Positional synchronous write on a non-OVERLAPPED handle.
  void write_all(HANDLE use_h, const std::byte* data, std::size_t len,
                 std::uint64_t off) {
    std::size_t put = 0;
    while (put < len) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + put;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::WriteFile(use_h, data + put, static_cast<DWORD>(len - put), &n,
                      &ov) == 0) {
        io_impl::throw_winerr("WriteFile");
      }
      put += n;
    }
  }

  void submit_async(unsigned s, std::size_t from) {
    slot_state& st = slots[s];
    OVERLAPPED& ov = ovs[s];
    std::memset(&ov, 0, sizeof(ov));
    const std::uint64_t o = st.off + from;
    ov.Offset = static_cast<DWORD>(o);
    ov.OffsetHigh = static_cast<DWORD>(o >> 32);
    if (::WriteFile(h, buf(s) + from, static_cast<DWORD>(st.len - from),
                    nullptr, &ov) == 0 &&
        ::GetLastError() != ERROR_IO_PENDING) {
      io_impl::throw_winerr("WriteFile(async)");
    }
    ++outstanding;
  }

  void reap_one() {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok =
        ::GetQueuedCompletionStatus(port, &bytes, &key, &pov, INFINITE);
    if (pov == nullptr) {
      io_impl::throw_winerr("GetQueuedCompletionStatus");
    }
    --outstanding;
    const unsigned s = static_cast<unsigned>(pov - ovs.data());
    slot_state& st = slots[s];
    if (ok == 0 || bytes == 0) {
      io_impl::throw_winerr("iocp write");
    }
    st.done += bytes;
    if (st.done < st.len) {
      submit_async(s, st.done);
    } else {
      st.busy = false;
    }
  }
#endif

#if defined(BLAKE3PP_IO_GCD)
  // Runs on a GCD worker: drains the slot's buffer with one positional
  // write loop, then publishes completion under the pump lock. noexcept:
  // errors travel through slot_state::error to the acquiring thread.
  static void run_write(void* ctx) noexcept {
    const gcd_task t = *static_cast<gcd_task*>(ctx);
    impl& im = *t.self;
    slot_state& st = im.slots[t.slot];
    int err = 0;
    std::size_t put = 0;
    while (put < st.len) {
      const ssize_t n = ::pwrite(im.fd, im.buf(t.slot) + put, st.len - put,
                                 static_cast<off_t>(st.off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      put += static_cast<std::size_t>(n);
    }
    {
      const std::lock_guard<std::mutex> lk(im.pump.m);
      st.done = put;
      st.error = err;
      st.busy = false;
    }
    im.pump.cv.notify_all();
  }

  // Blocks until slot s is idle; surfaces its deferred write error once
  // (cleared after the throw so the slot stays reusable, matching the
  // reap_one contract).
  void wait_slot(unsigned s) {
    slot_state& st = slots[s];
    std::unique_lock<std::mutex> lk(pump.m);
    pump.cv.wait(lk, [&] { return !st.busy; });
    if (st.error != 0) {
      const int e = st.error;
      st.error = 0;
      throw std::system_error(e, std::generic_category(), "gcd pwrite");
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
#elif defined(BLAKE3PP_IO_GCD)
  // Same story via Darwin's spelling: F_PREALLOCATE + ftruncate.
  if (opts.preallocate_bytes > 0 &&
      io_impl::preallocate(im.fd_plain, opts.preallocate_bytes)) {
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
#elif defined(BLAKE3PP_IO_GCD)
  // Per-fd cache bypass, no alignment contract: one fd serves aligned
  // submits and the unaligned tail alike.
  if (opts.direct_io && io_impl::set_nocache(im.fd_plain)) {
    im.direct = true;
  }
#endif
#if defined(BLAKE3PP_IO_GCD)
  im.backend_name = im.direct ? "pwrite+nocache" : "pwrite";
  if (opts.async && im.pump.init()) {
    im.use_gcd = true;
    im.tasks.resize(im.qd);
    for (unsigned s = 0; s < im.qd; ++s) {
      im.tasks[s] = {&im, s};
    }
    im.backend_name = im.direct ? "gcd+nocache" : "gcd";
  }
#else
  im.backend_name = im.direct ? "pwrite+direct" : "pwrite";
#endif
#if defined(BLAKE3PP_IO_URING)
  if (opts.async && im.ring.init(2 * im.qd)) {
    im.use_uring = true;
    im.backend_name = im.direct ? "io_uring+direct" : "io_uring";
  }
#endif
#elif defined(BLAKE3PP_IO_WIN32)
  // Two opens of one file need explicit sharing on Windows. The buffered
  // synchronous handle always exists (tail, trim, fallback); the fast
  // handle layers NO_BUFFERING/OVERLAPPED on top.
  constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
  im.h_plain = ::CreateFileW(fspath.c_str(), GENERIC_WRITE, share, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (im.h_plain == INVALID_HANDLE_VALUE) {
    delete impl_;
    io_impl::throw_winerr("CreateFileW");
  }
  bool vdl = false;
  if (opts.preallocate_bytes > 0) {
    // SetEndOfFile is the fallocate twin: writes become overwrites of an
    // existing region. NTFS adds a second lock beyond ext4's, the valid
    // data length: any write landing past VDL zero-fills the gap
    // synchronously. SetFileValidData waives that, privilege permitting.
    LARGE_INTEGER target;
    target.QuadPart = static_cast<std::int64_t>(opts.preallocate_bytes);
    if (::SetFilePointerEx(im.h_plain, target, nullptr, FILE_BEGIN) != 0 &&
        ::SetEndOfFile(im.h_plain) != 0) {
      im.prealloc = opts.preallocate_bytes;
      vdl = io_impl::try_set_valid_data(im.h_plain, target.QuadPart);
    }
    LARGE_INTEGER zero{};
    ::SetFilePointerEx(im.h_plain, zero, nullptr, FILE_BEGIN);
  }
  im.h = im.h_plain;
  if (opts.direct_io || opts.async) {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (opts.direct_io) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (opts.async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    const HANDLE fast = ::CreateFileW(fspath.c_str(), GENERIC_WRITE, share,
                                      nullptr, OPEN_EXISTING, flags, nullptr);
    if (fast != INVALID_HANDLE_VALUE) {
      bool engaged = false;
      if (opts.async) {
        im.port =
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (im.port != nullptr &&
            ::CreateIoCompletionPort(fast, im.port, 0, 0) != nullptr) {
          im.use_iocp = true;
          engaged = true;
        } else if (im.port != nullptr) {
          ::CloseHandle(im.port);
          im.port = nullptr;
        }
      } else {
        engaged = true;
      }
      if (engaged) {
        im.h = fast;
        im.direct = opts.direct_io;
      } else {
        ::CloseHandle(fast);
      }
    }
  }
  im.backend_name = im.use_iocp
                        ? (im.direct ? (vdl ? "iocp+direct+vdl" : "iocp+direct")
                                     : "iocp")
                        : im.direct
                            ? (vdl ? "writefile+direct+vdl"
                                   : "writefile+direct")
                            : "writefile";
  im.ovs.resize(im.qd);
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
#if defined(BLAKE3PP_IO_URING) || defined(BLAKE3PP_IO_WIN32) || \
    defined(BLAKE3PP_IO_GCD)
  // Best-effort drain: buffers must outlive in-flight writes. Errors here
  // are unreportable; that is why finish() exists.
  try {
    finish();
  } catch (...) {
  }
#endif
#if defined(BLAKE3PP_IO_GCD)
  // If finish() threw, workers may still be writing from the pool: wait
  // them all out before it is freed.
  impl_->pump.destroy();
#endif
#if defined(BLAKE3PP_IO_URING)
  impl_->ring.destroy();
#endif
#if defined(BLAKE3PP_IO_WIN32)
  // If finish() threw, writes may still be in flight against the pool.
  if (impl_->outstanding > 0) {
    ::CancelIoEx(impl_->h, nullptr);
    while (impl_->outstanding > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(impl_->port, &bytes, &key, &pov, 5000);
      if (pov == nullptr) {
        break;
      }
      --impl_->outstanding;
    }
  }
#endif
  impl& im = *impl_;
  if (im.pool != nullptr) {
    ::operator delete(im.pool, std::align_val_t{direct_align});
  }
#if defined(BLAKE3PP_IO_POSIX)
  // fd is a second, separately-opened fd only when the O_DIRECT reopen
  // engaged; on Darwin (F_NOCACHE on the one fd) they are the same.
  if (im.fd != im.fd_plain && im.fd >= 0) {
    ::close(im.fd);
  }
  if (im.fd_plain >= 0) {
    ::close(im.fd_plain);
  }
#elif defined(BLAKE3PP_IO_WIN32)
  if (im.port != nullptr) {
    ::CloseHandle(im.port);
  }
  if (im.h != im.h_plain && im.h != INVALID_HANDLE_VALUE) {
    ::CloseHandle(im.h);
  }
  if (im.h_plain != INVALID_HANDLE_VALUE) {
    ::CloseHandle(im.h_plain);
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
#if defined(BLAKE3PP_IO_GCD)
  if (im.use_gcd) {
    im.wait_slot(s);
  }
#elif defined(BLAKE3PP_IO_URING) || defined(BLAKE3PP_IO_WIN32)
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
#if defined(BLAKE3PP_IO_GCD)
  if (im.use_gcd && aligned) {
    impl::slot_state& st = im.slots[b.slot];
    st.off = im.offset;
    st.len = bytes;
    st.done = 0;
    st.busy = true;
    im.pump.submit(&impl::run_write, &im.tasks[b.slot]);
    im.offset += bytes;
    im.written += bytes;
    im.next_slot = (b.slot + 1) % im.qd;
    return;
  }
#elif defined(BLAKE3PP_IO_URING)
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
#elif defined(BLAKE3PP_IO_WIN32)
  if (im.use_iocp && aligned) {
    impl::slot_state& st = im.slots[b.slot];
    st.off = im.offset;
    st.len = bytes;
    st.done = 0;
    st.busy = true;
    im.submit_async(b.slot, 0);
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
#elif defined(BLAKE3PP_IO_WIN32)
  // Same, spelled NO_BUFFERING: the tail always takes the buffered
  // handle (and in iocp mode aligned writes never reach this path).
  const HANDLE use_h =
      aligned && im.direct && !im.use_iocp ? im.h : im.h_plain;
  im.write_all(use_h, b.data, bytes, im.offset);
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
#if defined(BLAKE3PP_IO_GCD)
  if (im.use_gcd) {
    for (unsigned s = 0; s < im.qd; ++s) {
      im.wait_slot(s);
    }
  }
#elif defined(BLAKE3PP_IO_URING) || defined(BLAKE3PP_IO_WIN32)
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
#elif defined(BLAKE3PP_IO_WIN32)
  // SetEndOfFile set the size up front; trim back if less was written.
  if (im.prealloc > im.written) {
    LARGE_INTEGER n;
    n.QuadPart = static_cast<std::int64_t>(im.written);
    if (::SetFilePointerEx(im.h_plain, n, nullptr, FILE_BEGIN) == 0 ||
        ::SetEndOfFile(im.h_plain) == 0) {
      io_impl::throw_winerr("SetEndOfFile(trim)");
    }
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
