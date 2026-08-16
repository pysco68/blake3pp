#pragma once

// Shared io_uring plumbing for the direct-I/O reader and writer: three raw
// syscalls (setup, enter, mmap for the rings), SQE submit and CQE reap with
// atomic_ref acquire/release on the kernel-shared ring indices. Internal to
// src/io/, never installed.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#define BLAKE3PP_IO_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#define BLAKE3PP_IO_URING 1
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <algorithm>
#include <atomic>

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
#endif
#if !defined(BLAKE3PP_MSAN_UNPOISON)
#define BLAKE3PP_MSAN_UNPOISON(ptr, len) ((void)0)
#endif

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

constexpr std::size_t direct_align = 4096;

#if defined(BLAKE3PP_IO_URING)
inline int sys_io_uring_setup(unsigned entries, io_uring_params* p) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                       unsigned flags) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                                    min_complete, flags, nullptr, 0));
}

// The mapped rings, reduced to what reads need. Kernel-shared integers are
// accessed through atomic_ref with acquire/release, per the io_uring
// memory-ordering contract.
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
      return fail();
    }
    cq_ring = (p.features & IORING_FEAT_SINGLE_MMAP)
                  ? sq_ring
                  : ::mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) {
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
  void submit_rw(std::uint8_t opcode, int file_fd, void* buf, unsigned len,
                 std::uint64_t off, std::uint64_t user_data) {
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

#endif  // BLAKE3PP_IO_URING

}  // namespace blake3pp::detail::io_impl
