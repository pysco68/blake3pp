#pragma once

// The Linux backend: io_uring driven through raw syscalls (three of them:
// setup, enter, and mmap for the rings), with no liburing dependency, so
// every moving part is visible. Degrades per-feature at RUNTIME inside
// this class: O_DIRECT refused by the filesystem -> buffered io_uring;
// io_uring refused (seccomp, old kernel) -> synchronous pread. Internal
// to src/io/, never installed.

#if defined(__linux__)

#include <linux/io_uring.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "io/backend.hpp"
#include "io/posix_file.hpp"

// MemorySanitizer cannot see io_uring completions: the kernel fills read
// buffers without any libc call MSan intercepts, so the bytes stay
// "uninitialized" in its shadow. Read completions therefore unpoison the
// range they filled, stating a fact MSan has no other way to learn.
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define BLAKE3PP_MSAN_UNPOISON(ptr, len) __msan_unpoison(ptr, len)
#endif
#endif
#if !defined(BLAKE3PP_MSAN_UNPOISON)
#define BLAKE3PP_MSAN_UNPOISON(ptr, len) ((void)0)
#endif

namespace blake3pp::detail::io_impl {

inline int sys_io_uring_setup(unsigned entries, io_uring_params* p) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int ring_fd, unsigned to_submit,
                              unsigned min_complete, unsigned flags) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                                    min_complete, flags, nullptr, 0));
}

// The mapped rings, reduced to what this pipeline needs. Kernel-shared
// integers are accessed through atomic_ref with acquire/release, per the
// io_uring memory-ordering contract.
struct uring {
  int fd = -1;
  unsigned sq_entries = 0;
  unsigned cq_entries = 0;
  void* sq_ring = nullptr;
  std::size_t sq_ring_sz = 0;
  void* cq_ring = nullptr;
  std::size_t cq_ring_sz = 0;
  io_uring_sqe* sqes = nullptr;
  std::size_t sqes_sz = 0;
  unsigned* sq_tail = nullptr;
  unsigned* sq_mask = nullptr;
  unsigned* sq_array = nullptr;
  unsigned* cq_head = nullptr;
  unsigned* cq_tail = nullptr;
  unsigned* cq_mask = nullptr;
  io_uring_cqe* cqes = nullptr;

  uring() = default;
  uring(const uring&) = delete;
  uring& operator=(const uring&) = delete;
  ~uring() { destroy(); }

  bool init(unsigned entries) noexcept {
    io_uring_params p;
    std::memset(&p, 0, sizeof(p));
    fd = sys_io_uring_setup(entries, &p);
    if (fd < 0) {
      return false;
    }
    sq_entries = p.sq_entries;
    cq_entries = p.cq_entries;
    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
      sq_ring_sz = cq_ring_sz = std::max(sq_ring_sz, cq_ring_sz);
    }
    sq_ring = ::mmap(nullptr, sq_ring_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) {
      sq_ring = nullptr;  // destroy() tests for null, and MAP_FAILED is -1
      return fail();
    }
    cq_ring = (p.features & IORING_FEAT_SINGLE_MMAP)
                  ? sq_ring
                  : ::mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) {
      cq_ring = nullptr;
      return fail();
    }
    sqes_sz = p.sq_entries * sizeof(io_uring_sqe);
    sqes = static_cast<io_uring_sqe*>(
        ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
    if (sqes == MAP_FAILED) {
      sqes = nullptr;
      return fail();
    }
    auto* sqb = static_cast<unsigned char*>(sq_ring);
    auto* cqb = static_cast<unsigned char*>(cq_ring);
    sq_tail = reinterpret_cast<unsigned*>(sqb + p.sq_off.tail);
    sq_mask = reinterpret_cast<unsigned*>(sqb + p.sq_off.ring_mask);
    sq_array = reinterpret_cast<unsigned*>(sqb + p.sq_off.array);
    cq_head = reinterpret_cast<unsigned*>(cqb + p.cq_off.head);
    cq_tail = reinterpret_cast<unsigned*>(cqb + p.cq_off.tail);
    cq_mask = reinterpret_cast<unsigned*>(cqb + p.cq_off.ring_mask);
    cqes = reinterpret_cast<io_uring_cqe*>(cqb + p.cq_off.cqes);
    return true;
  }

  bool fail() noexcept {
    destroy();
    return false;
  }

  void destroy() noexcept {
    if (sqes != nullptr) {
      ::munmap(sqes, sqes_sz);
      sqes = nullptr;
    }
    if (cq_ring != nullptr && cq_ring != sq_ring) {
      ::munmap(cq_ring, cq_ring_sz);
    }
    cq_ring = nullptr;
    if (sq_ring != nullptr) {
      ::munmap(sq_ring, sq_ring_sz);
      sq_ring = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  // Queues one READ or WRITE; the sole submitter, so sq_tail needs no CAS.
  void submit_rw(std::uint8_t opcode, int file_fd, const void* buf,
                 unsigned len, std::uint64_t off, std::uint64_t user_data) {
    const unsigned tail = *sq_tail;  // we are the only writer
    const unsigned idx = tail & *sq_mask;
    io_uring_sqe& sqe = sqes[idx];
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = opcode;
    sqe.fd = file_fd;
    sqe.addr = reinterpret_cast<std::uint64_t>(buf);
    sqe.len = len;
    sqe.off = off;
    sqe.user_data = user_data;
    sq_array[idx] = idx;
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
    if (sys_io_uring_enter(fd, 1, 0, 0) < 0) {
      throw_errno("io_uring_enter(submit)");
    }
  }

  // Blocks for one completion and returns (user_data, result).
  std::pair<std::uint64_t, int> wait_one() {
    for (;;) {
      const unsigned head = *cq_head;  // we are the only consumer
      const unsigned tail =
          std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
      if (head != tail) {
        const io_uring_cqe& cqe = cqes[head & *cq_mask];
        const std::pair<std::uint64_t, int> out{cqe.user_data, cqe.res};
        std::atomic_ref<unsigned>(*cq_head).store(head + 1,
                                                  std::memory_order_release);
        return out;
      }
      if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
          errno != EINTR) {
        throw_errno("io_uring_enter(wait)");
      }
    }
  }
};

class uring_reader {
 public:
  uring_reader(const std::filesystem::path& path,
               const file_reader_options& opts, unsigned nslots)
      : slots_(nslots) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    if (opts.direct_io) {
      f_.try_odirect(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (opts.async && ring_.init(2 * nslots)) {
      use_uring_ = true;
    }
    name_ = use_uring_ ? (f_.direct ? "io_uring+direct" : "io_uring")
                       : (f_.direct ? "pread+direct" : "pread");
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Only fully-aligned windows may ride the io_uring path (O_DIRECT
  // rejects unaligned lengths); the tail goes through read_sync.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t len) const
      noexcept {
    return use_uring_ && len % direct_align == 0;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slots_[s] = {buf, off, 0, false};
    ring_.submit_rw(IORING_OP_READ, f_.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, s);
  }

  // Reaps completions (issuing continuations for short reads) until slot
  // `s` is fully read; completions for other slots are absorbed into
  // their state along the way.
  void wait(unsigned s) {
    while (!slots_[s].ready) {
      const auto [ud, res] = ring_.wait_one();
      const unsigned c = static_cast<unsigned>(ud);
      slot& st = slots_[c];
      if (res < 0) {
        throw std::system_error(-res, std::generic_category(),
                                "io_uring read");
      }
      if (res == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF (io_uring)");
      }
      BLAKE3PP_MSAN_UNPOISON(st.buf.data() + st.filled,
                             static_cast<std::size_t>(res));
      st.filled += static_cast<std::size_t>(res);
      if (st.filled < st.buf.size()) {
        ring_.submit_rw(IORING_OP_READ, f_.fd, st.buf.data() + st.filled,
                        static_cast<unsigned>(st.buf.size() - st.filled),
                        st.off + st.filled, c);
      } else {
        st.ready = true;
      }
    }
  }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

 private:
  struct slot {
    std::span<std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool ready = false;
  };

  posix_file f_;
  uring ring_;
  std::vector<slot> slots_;
  std::uint64_t size_ = 0;
  bool use_uring_ = false;
  std::string_view name_ = "pread";
};

class uring_writer {
 public:
  uring_writer(const std::filesystem::path& path,
               const file_writer_options& opts, unsigned nslots)
      : slots_(nslots) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // Preallocating turns every write into an overwrite of existing
    // extents. Extending writes serialize on the inode lock; with async
    // direct I/O that collapses the whole queue to one stalled write at a
    // time.
    if (opts.preallocate_bytes > 0 &&
        ::fallocate(f_.fd_plain, 0, 0,
                    static_cast<off_t>(opts.preallocate_bytes)) == 0) {
      prealloc_ = opts.preallocate_bytes;
    }
    if (opts.direct_io) {
      // Reopen (not TRUNC, already truncated) so the tail keeps a plain fd.
      f_.try_odirect(path.c_str(), O_WRONLY | O_CLOEXEC);
    }
    if (opts.async && ring_.init(2 * nslots)) {
      use_uring_ = true;
    }
    name_ = use_uring_ ? (f_.direct ? "io_uring+direct" : "io_uring")
                       : (f_.direct ? "pwrite+direct" : "pwrite");
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return use_uring_ && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slots_[s] = {buf, off, 0, true};
    ring_.submit_rw(IORING_OP_WRITE, f_.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, s);
  }

  void wait_slot(unsigned s) {
    while (slots_[s].busy) {
      reap_one();
    }
  }

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.sync_fd(buf.size()), buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t written) {
    for (unsigned s = 0; s < slots_.size(); ++s) {
      wait_slot(s);
    }
    // fallocate set the file size up front; trim if less was written.
    if (prealloc_ > written &&
        ::ftruncate(f_.fd_plain, static_cast<off_t>(written)) != 0) {
      throw_errno("ftruncate");
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    std::span<const std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t done = 0;
    bool busy = false;
  };

  // Reaps one completion, issuing a continuation on a short write. The
  // device may complete slots in any order; each carries its slot index.
  void reap_one() {
    const auto [ud, res] = ring_.wait_one();
    const unsigned s = static_cast<unsigned>(ud);
    slot& st = slots_[s];
    if (res <= 0) {
      throw std::system_error(res < 0 ? -res : EIO, std::generic_category(),
                              "io_uring write");
    }
    st.done += static_cast<std::size_t>(res);
    if (st.done < st.buf.size()) {
      ring_.submit_rw(IORING_OP_WRITE, f_.fd, st.buf.data() + st.done,
                      static_cast<unsigned>(st.buf.size() - st.done),
                      st.off + st.done, s);
    } else {
      st.busy = false;
    }
  }

  posix_file f_;
  uring ring_;
  std::vector<slot> slots_;
  std::uint64_t prealloc_ = 0;
  bool use_uring_ = false;
  std::string_view name_ = "pwrite";
};

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(reader_backend<uring_reader>);
static_assert(writer_backend<uring_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __linux__
