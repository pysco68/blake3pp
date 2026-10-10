#pragma once

// The Linux backend: io_uring driven through raw syscalls (three of them:
// setup, enter, and mmap for the rings), with no liburing dependency, so
// every moving part is visible. Degrades per-feature at RUNTIME inside
// this class: O_DIRECT refused by the filesystem -> buffered io_uring;
// io_uring refused (seccomp, old kernel) -> synchronous pread. Internal
// to src/io/, never installed.
//
// Registered buffers (IORING_REGISTER_BUFFERS + IORING_OP_READ_FIXED)
// are not used: they measured 28-38% slower than plain reads in every
// round of a four-round alternating run on Linux 6.8 over virtio-scsi.

#if defined(__linux__)

#include <linux/io_uring.h>
#include <poll.h>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <bit>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
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

// Names the reason the ring is absent, for the fallback backend name the
// tools print: "blocked by policy" (Android's seccomp filter, EPERM) and
// "kernel too old" (ENOSYS) are otherwise indistinguishable in bench
// output.
inline std::string no_uring_suffix(int err) {
  switch (err) {
    case EPERM:
      return " [io_uring: EPERM, blocked by policy]";
    case ENOSYS:
      return " [io_uring: ENOSYS, kernel too old]";
    default:
      return " [io_uring: errno " + std::to_string(err) + "]";
  }
}

// The ring is memory the kernel mapped and writes into, so no C++ object
// was ever created in it, while the accesses below want a T there: an
// unsigned for std::atomic_ref, which operates on an object in place.
//
// Two spellings say so, and both are defined behaviour.
//  - start_lifetime_as (C++23) starts the object's lifetime and copies
//    nothing.
//  - Placement-new creates the object, seeded from the bytes already in
//    the storage so it keeps the value the kernel put there. The byte
//    read is legal, and the write puts back what it read.
// Nothing else in the ring is named as an object. The descriptors and
// completions are copied in and out with memcpy, which creates whatever
// it writes, into and out of locals.
template <class T>
[[nodiscard]] inline T* ring_at(unsigned char* base, std::uint32_t off) noexcept {
#ifdef __cpp_lib_start_lifetime_as
  return std::start_lifetime_as<T>(base + off);
#else
  T seed{};
  std::memcpy(&seed, base + off, sizeof seed);
  return ::new (static_cast<void*>(base + off)) T(seed);
#endif
}

// atomic_ref needs the referenced object to be lock-free to be usable from
// another address space; nothing here would work otherwise.
static_assert(std::atomic_ref<unsigned>::is_always_lock_free,
              "the io_uring head/tail contract needs lock-free 32-bit atomics");

// One mmap, unmapped when it goes, with MAP_FAILED folded into the empty
// state. The ring's are the only mappings in the library.
class unique_mmap {
 public:
  unique_mmap() noexcept = default;
  unique_mmap(void* p, std::size_t n) noexcept
      : p_(p == MAP_FAILED ? nullptr : p), n_(p == MAP_FAILED ? 0 : n) {}
  unique_mmap(unique_mmap&& o) noexcept
      : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
  unique_mmap& operator=(unique_mmap&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = std::exchange(o.p_, nullptr);
      n_ = std::exchange(o.n_, 0);
    }
    return *this;
  }
  unique_mmap(const unique_mmap&) = delete;
  unique_mmap& operator=(const unique_mmap&) = delete;
  ~unique_mmap() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept {
    return p_ != nullptr;
  }
  [[nodiscard]] unsigned char* bytes() const noexcept {
    return static_cast<unsigned char*>(p_);
  }
  void reset() noexcept {
    if (p_ != nullptr) {
      ::munmap(p_, n_);
      p_ = nullptr;
      n_ = 0;
    }
  }

 private:
  void* p_ = nullptr;
  std::size_t n_ = 0;
};

// The mapped rings, reduced to what this pipeline needs. Kernel-shared
// integers are accessed through atomic_ref with acquire/release, per the
// io_uring memory-ordering contract.
struct uring {
  unique_fd fd;
  // errno from a failed io_uring_setup. EPERM (a seccomp policy forbids
  // the syscall; Android does) and ENOSYS (kernel predates io_uring) are
  // the same observable with entirely different causes, so the fallback
  // name below reports which one fired.
  int setup_errno = 0;
  unsigned sq_entries = 0;
  unsigned cq_entries = 0;
  // Two or three mappings, owned here; the pointers below are views into
  // them. With IORING_FEAT_SINGLE_MMAP the completion ring IS the
  // submission ring, and then cq_map stays empty: ownership says how many
  // mappings exist, so unmapping never has to compare two addresses.
  unique_mmap sq_map;
  unique_mmap cq_map;
  unique_mmap sqes_map;
  unsigned char* sqes = nullptr;
  unsigned* sq_head = nullptr;
  unsigned* sq_tail = nullptr;
  unsigned* sq_mask = nullptr;
  unsigned char* sq_array = nullptr;
  unsigned* cq_head = nullptr;
  unsigned* cq_tail = nullptr;
  unsigned* cq_mask = nullptr;
  unsigned char* cqes = nullptr;
  // Submissions the kernel has accepted and not yet completed. destroy()
  // reaps them before the ring goes. Closing the ring fd does not wait,
  // since ring exit runs on a kernel workqueue, and a read already issued
  // to the device completes into the pages it pinned. By then those pages
  // belong to whatever the caller allocated next.
  unsigned outstanding = 0;

  uring() = default;
  uring(const uring&) = delete;
  uring& operator=(const uring&) = delete;
  ~uring() { destroy(); }

  bool init(unsigned entries) noexcept {
    io_uring_params p;
    std::memset(&p, 0, sizeof(p));
    fd.reset(sys_io_uring_setup(entries, &p));
    if (!fd) {
      setup_errno = errno;
      return false;
    }
    sq_entries = p.sq_entries;
    cq_entries = p.cq_entries;
    const bool single = (p.features & IORING_FEAT_SINGLE_MMAP) != 0;
    std::size_t sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    const std::size_t cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    if (single) {
      sq_sz = std::max(sq_sz, cq_sz);
    }
    sq_map = unique_mmap(::mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_POPULATE, fd.get(),
                                IORING_OFF_SQ_RING),
                         sq_sz);
    if (!sq_map) {
      return fail();
    }
    if (!single) {
      cq_map = unique_mmap(::mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, fd.get(),
                                  IORING_OFF_CQ_RING),
                           cq_sz);
      if (!cq_map) {
        return fail();
      }
    }
    const std::size_t sqes_sz = p.sq_entries * sizeof(io_uring_sqe);
    sqes_map = unique_mmap(::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, fd.get(),
                                  IORING_OFF_SQES),
                           sqes_sz);
    if (!sqes_map) {
      return fail();
    }
    sqes = sqes_map.bytes();
    unsigned char* const sqb = sq_map.bytes();
    unsigned char* const cqb = single ? sqb : cq_map.bytes();
    sq_head = ring_at<unsigned>(sqb, p.sq_off.head);
    sq_tail = ring_at<unsigned>(sqb, p.sq_off.tail);
    sq_mask = ring_at<unsigned>(sqb, p.sq_off.ring_mask);
    sq_array = sqb + p.sq_off.array;
    cq_head = ring_at<unsigned>(cqb, p.cq_off.head);
    cq_tail = ring_at<unsigned>(cqb, p.cq_off.tail);
    cq_mask = ring_at<unsigned>(cqb, p.cq_off.ring_mask);
    cqes = cqb + p.cq_off.cqes;
    return true;
  }

  bool fail() noexcept {
    destroy();
    return false;
  }

  // Reaps every completion still owed. The caller frees the buffers
  // those transfers target as soon as this returns, so it returns only
  // when the kernel holds none of them.
  //  - A wait interrupted or refused for a transient reason is retried:
  //    EINTR, and EBUSY or EAGAIN, which a ring in overflow reports until
  //    its queue is reaped.
  //  - Any other failure is a ring this process has broken (a closed
  //    descriptor, a bad argument) with transfers still in the kernel.
  //    Nothing can make it release them, and continuing would let it
  //    write into memory about to be reused, so the process ends with the
  //    reason.
  void drain() noexcept {
    while (outstanding > 0 && cq_head != nullptr) {
      const unsigned head = *cq_head;
      const unsigned tail =
          std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
      if (head != tail) {
        std::atomic_ref<unsigned>(*cq_head).store(tail,
                                                  std::memory_order_release);
        outstanding -= std::min(tail - head, outstanding);
        continue;
      }
      if (sys_io_uring_enter(fd.get(), 0, 1, IORING_ENTER_GETEVENTS) < 0) {
        if (errno == EINTR || errno == EBUSY || errno == EAGAIN) {
          continue;
        }
        std::fprintf(stderr,
                     "blake3pp: io_uring_enter failed while %u read(s) were "
                     "still owed (%s); their buffers cannot be released\n",
                     outstanding, std::strerror(errno));
        std::abort();
      }
    }
  }

  void destroy() noexcept {
    drain();
    // The views first: nothing may name a mapping that is going away, and
    // a failed init() runs this with only some of them set.
    sqes = nullptr;
    sq_head = sq_tail = sq_mask = cq_head = cq_tail = cq_mask = nullptr;
    sq_array = cqes = nullptr;
    sqes_map.reset();
    cq_map.reset();
    sq_map.reset();
    fd.reset();
  }

  // Queues one READ or WRITE without entering the kernel; the sole
  // submitter, so sq_tail needs no CAS. flush() is what hands it over.
  void fill_rw(std::uint8_t opcode, int file_fd, const void* buf,
               unsigned len, std::uint64_t off, std::uint64_t user_data,
               bool offload = false) noexcept {
    io_uring_sqe sqe{};
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = opcode;
    if (offload) {
      // Issue on io-wq. Kernel 7.0 issues a large O_DIRECT read inline:
      // 1.5-1.9 ms of pinning, splitting and queueing per 64 MiB on the
      // submitting thread (four PCIe 5 drives), against 0.02 ms on 5.15,
      // which handed it to io-wq. That thread also waits for the hash, so
      // the two serialized; with the hand-off the pipeline went from 22.3
      // to 41.4 GiB/s at 64 MiB x 8.
      sqe.flags |= IOSQE_ASYNC;
    }
    sqe.fd = file_fd;
    sqe.addr = static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(buf));
    sqe.len = len;
    sqe.off = off;
    sqe.user_data = user_data;
    push_sqe(sqe);
  }

  // Queues a cancel for the request carrying `target`. With no matching
  // flags the kernel keys on user_data, which is the op's own address.
  //
  // Its CQE is separate from the target's and reports whether a request
  // was found (0), was already past cancelling (-EALREADY) or was never
  // there (-ENOENT). None of that changes what the caller has to do, so
  // it rides a user_data of its own and the reaper drops it; what
  // matters is the target's completion, which arrives either way.
  void fill_cancel(std::uint64_t target, std::uint64_t user_data) noexcept {
    io_uring_sqe sqe{};
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_ASYNC_CANCEL;
    sqe.fd = -1;
    sqe.addr = target;
    sqe.user_data = user_data;
    push_sqe(sqe);
  }

  // Queues a one-shot poll on `poll_fd`, the wake path's arming step.
  void fill_poll_add(int poll_fd, std::uint64_t user_data) noexcept {
    io_uring_sqe sqe{};
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_POLL_ADD;
    sqe.fd = poll_fd;
    sqe.poll_events = POLLIN;
    sqe.user_data = user_data;
    push_sqe(sqe);
  }

  // The one place the submission tail moves: the entry goes into the
  // next slot and the tail is published with release semantics, which
  // is what the kernel's acquire pairs with.
  void push_sqe(const io_uring_sqe& sqe) noexcept {
    const unsigned tail = *sq_tail;
    const unsigned idx = tail & *sq_mask;
    std::memcpy(sqes + idx * sizeof(io_uring_sqe), &sqe, sizeof(sqe));
    std::memcpy(sq_array + idx * sizeof(unsigned), &idx, sizeof(idx));
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
  }

  // Hands the kernel everything queued since it last looked, in one
  // enter. It submits the whole head-to-tail span, so a failed enter is
  // recoverable: its entries stay published, and the next flush picks them
  // up in order.
  void flush() {
    for (;;) {
      const unsigned tail = *sq_tail;
      const unsigned head =
          std::atomic_ref<unsigned>(*sq_head).load(std::memory_order_acquire);
      if (tail == head) {
        return;
      }
      const int n = sys_io_uring_enter(fd.get(), tail - head, 0, 0);
      if (n >= 0) {
        outstanding += static_cast<unsigned>(n);
        return;
      }
      if (errno != EINTR) {
        throw_errno("io_uring_enter(submit)");
      }
    }
  }

  // Takes back every entry the kernel has not accepted, oldest first,
  // and leaves the queue empty. A caller that cannot submit needs to
  // know which reads it is still holding, and a submit the kernel
  // refused will not do better on the next try.
  template <class Fn>
  void take_unsubmitted(Fn&& fn) noexcept {
    const unsigned tail = *sq_tail;  // we are the only writer
    const unsigned head =
        std::atomic_ref<unsigned>(*sq_head).load(std::memory_order_acquire);
    for (unsigned i = head; i != tail; ++i) {
      io_uring_sqe sqe{};
      std::memcpy(&sqe, sqes + (i & *sq_mask) * sizeof(io_uring_sqe),
                  sizeof(sqe));
      fn(sqe.user_data);
    }
    std::atomic_ref<unsigned>(*sq_tail).store(head, std::memory_order_release);
  }

  // Queues one READ or WRITE and submits immediately: the writer engine's
  // one-at-a-time shape.
  void submit_rw(std::uint8_t opcode, int file_fd, const void* buf,
                 unsigned len, std::uint64_t off, std::uint64_t user_data,
                 bool offload = false) {
    fill_rw(opcode, file_fd, buf, len, off, user_data, offload);
    flush();
  }

  // Takes one completion if the ring has one. False leaves the ring
  // untouched.
  [[nodiscard]] bool reap(std::uint64_t& user_data, int& res) noexcept {
    const unsigned head = *cq_head;  // we are the only consumer
    const unsigned tail =
        std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    io_uring_cqe cqe{};
    std::memcpy(&cqe, cqes + (head & *cq_mask) * sizeof(io_uring_cqe),
                sizeof(cqe));
    user_data = cqe.user_data;
    res = cqe.res;
    std::atomic_ref<unsigned>(*cq_head).store(head + 1,
                                              std::memory_order_release);
    outstanding -= std::min(1u, outstanding);
    return true;
  }

  // Sleeps until the ring has at least one completion.
  void wait_cq() {
    if (sys_io_uring_enter(fd.get(), 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
        errno != EINTR) {
      throw_errno("io_uring_enter(wait)");
    }
  }
};

// The io_uring reader context: one ring, any number of files, reads named
// by the caller's read_op, completions delivered as callbacks out of
// poll(). Degrades per feature at runtime, as the whole layer does:
// O_DIRECT refused by the filesystem -> buffered, io_uring refused ->
// every read served synchronously from the deferred list.
class uring_context {
 public:
  struct read_op : read_op_base {
    std::span<std::byte> buf{};         // reads fill this
    std::span<const std::byte> wbuf{};  // writes read from this
    std::uint64_t off = 0;
    std::size_t filled = 0;
    int fd = -1;                      // the fd this transfer rides
    posix_file* sync_file = nullptr;  // the deferred path needs the pair
    read_op* next_deferred = nullptr;
    bool on_ring = false;    // the kernel has it, so a cancel is an SQE
    bool cancelled = false;  // a cancel was asked for; one is enough
    bool is_write = false;

    [[nodiscard]] std::size_t length() const noexcept {
      return is_write ? wbuf.size() : buf.size();
    }
  };

  // An open file bound to a context. Every file must be destroyed before
  // the context it was opened on, which keeps these descriptors open
  // while the context's drain waits out the transfers on them.
  class file {
   public:
    file(uring_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : write_(how.write) {
      const int base = write_ ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      f_.open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = write_ ? 0 : f_.stat_size();
      // Preallocation belongs to the OPEN, not to the finish: a write
      // that extends the file serializes on the inode lock, and with
      // async direct I/O that collapses the whole queue to one stalled
      // write at a time. set_size() trims back afterwards.
      if (write_ && how.preallocate_bytes > 0 &&
          ::fallocate(f_.fd_plain(), 0, 0,
                      static_cast<off_t>(how.preallocate_bytes)) == 0) {
        prealloc_ = how.preallocate_bytes;
      }
      if (how.direct_io) {
        // Not TRUNC on the reopen: already truncated, and the tail keeps
        // a plain fd.
        f_.try_odirect(path.c_str(), (write_ ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
      }
      name_ = ctx.name_for(f_.direct(), write_);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (::ftruncate(f_.fd_plain(), static_cast<off_t>(bytes)) != 0) {
        throw_errno("ftruncate");
      }
      prealloc_ = 0;
    }

   private:
    friend class uring_context;
    posix_file f_;
    std::uint64_t size_ = 0;
    std::uint64_t prealloc_ = 0;
    std::string name_;
    bool write_ = false;
  };

  uring_context(const reader_context_options& opts, unsigned max_inflight)
      : async_requested_(opts.async) {
    // What one batch between two flushes can hold: a transfer and its
    // continuation for every slot, plus the armed wake poll and one
    // spare. A cancel sweep fits inside the same bound: the scope stops
    // starting transfers before it cancels them, so the two never share
    // a batch.
    if (opts.async && ring_.init(2 * max_inflight + 2)) {
      use_uring_ = true;
      offload_ = opts.offload_submit;
      arm_wake();
      ring_.flush();
    }
  }

  uring_context(const uring_context&) = delete;
  uring_context& operator=(const uring_context&) = delete;

  // Runs before any member is destroyed, and the eventfd outlives the
  // ring by declaration order, so the nudge the drain needs lands on an
  // open descriptor. ~uring drains again and finds nothing.
  ~uring_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    arm(op, f, off);
    op.buf = buf;
    op.wbuf = {};
    op.is_write = false;
    issue(op, IORING_OP_READ, buf.data(), buf.size());
  }

  // Same rules as submit_read, and the same ladder: a length O_DIRECT
  // cannot take rides the deferred list and is written synchronously
  // inside poll(), through the plain fd. Nothing above the context sees
  // the difference.
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, read_op& op) {
    arm(op, f, off);
    op.buf = {};
    op.wbuf = buf;
    op.is_write = true;
    issue(op, IORING_OP_WRITE, buf.data(), buf.size());
  }

 private:
  // What a transfer needs that does not depend on its direction.
  void arm(read_op& op, file& f, std::uint64_t off) noexcept {
    op.off = off;
    op.filled = 0;
    op.fd = f.f_.fd();
    op.sync_file = &f.f_;
    op.next_deferred = nullptr;
    op.on_ring = false;
    op.cancelled = false;
    in_flight_++;
  }

  // The ladder, asked once for both directions: only whole granules may
  // ride the ring, because O_DIRECT rejects an unaligned length, and the
  // tail goes through the synchronous path inside poll(). A zero-length
  // transfer takes that path too: io_uring would report it as res == 0,
  // which is the signature of a short transfer with nothing left to give,
  // so a no-op never reaches the kernel. This is the whole
  // runtime-degradation question.
  void issue(read_op& op, std::uint8_t opcode, const void* data,
             std::size_t len) noexcept {
    if (use_uring_ && len != 0 && len % direct_align == 0) {
      op.on_ring = true;
      ring_.fill_rw(opcode, op.fd, data, static_cast<unsigned>(len), op.off,
                    op_ud(&op), offload_);
    } else {
      deferred_.push(op);
    }
  }

 public:

  // The kernel is asked once; a second ask would only add a CQE to drop.
  // A read on the deferred list has not been issued to anything, so the
  // flag alone is the whole cancel and run_one_deferred() honours it.
  void cancel(read_op& op) noexcept {
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    if (op.on_ring) {
      ring_.fill_cancel(op_ud(&op), cancel_ud());
    }
  }

  // A refused submit is not an error the caller can do anything with:
  // the reads are already counted in in_flight(), and an exception here
  // would leave them counted and uncompleted. They take the ladder this
  // backend already has for a read it cannot put on the ring instead.
  void flush() noexcept {
    if (!use_uring_) {
      return;
    }
    try {
      ring_.flush();
    } catch (const std::system_error&) {
      defer_unsubmitted();
    }
  }

  std::size_t poll(bool block) {
    flush();
    std::size_t ran = reap_ready() + run_one_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (take_wake()) {
        return ran;
      }
      sleep_once();
      ran += reap_ready() + run_one_deferred();
      if (ran > 0 || take_wake()) {
        return ran;
      }
    }
  }

  // The one member another thread may call.
  void wake() noexcept { waiter_.wake(); }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Entries the kernel never took are withdrawn, the reads it did take
  // are reaped and discarded, and the deferred list is forgotten. The
  // armed wake poll completes only when the eventfd becomes readable,
  // so it is nudged first; the ring's drain reaps it with the rest, and
  // the next blocking poll re-arms it. A wake that arrives meanwhile is
  // kept in the eventfd's counter, so nothing is lost: at worst the next
  // poll returns once with nothing to report.
  void drain() noexcept {
    if (use_uring_) {
      ring_.take_unsubmitted([this](std::uint64_t ud) noexcept {
        if (ud == wake_ud()) {
          wake_armed_ = false;
        }
      });
      if (wake_armed_) {
        waiter_.wake();
      }
      ring_.drain();
      wake_armed_ = false;
    }
    deferred_.clear();
    in_flight_ = 0;
  }

  // Every entry the kernel refused to take names a read that in_flight()
  // counts and the ring will never complete. Moving them to the deferred
  // list is what keeps the contract's promise that a counted read
  // reaches its callback: poll() reads each one synchronously and
  // reports whatever happens then. The wake's one-shot poll comes back
  // with them; take_wake() re-arms it.
  //
  // Called by flush() on a refused submit, and directly by the test that
  // covers this path, since a working kernel cannot be asked to refuse.
  void defer_unsubmitted() noexcept {
    // A context that never got a ring has nothing published to take
    // back, and its ring pointers are null: every read it was handed is
    // already on the deferred list. Callers reach this from flush(),
    // which knows, and from a test, which does not.
    if (!use_uring_) {
      return;
    }
    ring_.take_unsubmitted([this](std::uint64_t ud) noexcept {
      if (ud == wake_ud()) {
        wake_armed_ = false;
        return;
      }
      if (ud == cancel_ud()) {
        return;  // withdrawn with its target; nothing owes a completion
      }
      read_op& op = *op_from(ud);
      op.next_deferred = nullptr;
      op.on_ring = false;  // the kernel never took it; no SQE to cancel
      deferred_.push(op);
    });
  }

 private:
  // Which rung of the ladder this context reached, for a file that did or
  // did not get O_DIRECT. file_reader::backend() reports it verbatim and
  // tools print it, so the spellings, their order and the suffixes are
  // observable API.
  [[nodiscard]] std::string name_for(bool direct, bool write) const {
    const char* const sync_name = write ? "pwrite" : "pread";
    std::string n = use_uring_
                        ? (direct ? "io_uring+direct" : "io_uring")
                        : (direct ? std::string(sync_name) + "+direct"
                                  : std::string(sync_name));
    if (use_uring_ && !offload_) {
      n += " (inline submit)";
    }
    if (async_requested_ && !use_uring_ && ring_.setup_errno != 0) {
      n += no_uring_suffix(ring_.setup_errno);
    }
    return n;
  }

  [[nodiscard]] std::uint64_t op_ud(read_op* op) const noexcept {
    return static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(op));
  }
  [[nodiscard]] read_op* op_from(std::uint64_t ud) const noexcept {
    return std::bit_cast<read_op*>(static_cast<std::uintptr_t>(ud));
  }
  // The wake completion needs a user_data no read_op can wear; its own
  // op's address is one, and costs nothing. A cancel's CQE needs the
  // same, and for the same reason: reaping it as a read would walk to a
  // read_op that does not exist.
  [[nodiscard]] std::uint64_t wake_ud() const noexcept {
    return static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(&wake_op_));
  }
  [[nodiscard]] std::uint64_t cancel_ud() const noexcept {
    return static_cast<std::uint64_t>(
        std::bit_cast<std::uintptr_t>(&cancel_op_));
  }

  void arm_wake() noexcept {
    ring_.fill_poll_add(waiter_.fd(), wake_ud());
    wake_armed_ = true;
  }

  void complete(read_op& op, std::error_code ec) noexcept {
    in_flight_--;
    op.done(&op, ec);
  }

  // Consumes every completion the ring has without blocking, reissuing
  // short reads. Returns the number of callbacks run.
  std::size_t reap_ready() {
    if (!use_uring_) {
      return 0;
    }
    std::size_t ran = 0;
    bool requeued = false;
    std::uint64_t ud = 0;
    int res = 0;
    while (ring_.reap(ud, res)) {
      if (ud == wake_ud()) {
        wake_armed_ = false;  // one-shot; take_wake() re-arms
        continue;
      }
      if (ud == cancel_ud()) {
        continue;  // the target's own completion is the one that matters
      }
      read_op& op = *op_from(ud);
      if (res < 0) {
        complete(op, std::error_code(-res, std::generic_category()));
        ran++;
        continue;
      }
      if (res == 0) {
        // Short of the length with nothing left to give: the file ended
        // where the engine was told it would not.
        complete(op, std::error_code(EIO, std::generic_category()));
        ran++;
        continue;
      }
      if (!op.is_write) {
        BLAKE3PP_MSAN_UNPOISON(op.buf.data() + op.filled,
                               static_cast<std::size_t>(res));
      }
      op.filled += static_cast<std::size_t>(res);
      if (op.filled < op.length()) {
        // A short transfer continues from where it stopped, reads and
        // writes alike; only the opcode and which span it walks differ.
        const std::byte* const from =
            op.is_write ? op.wbuf.data() + op.filled : op.buf.data() + op.filled;
        ring_.fill_rw(op.is_write ? IORING_OP_WRITE : IORING_OP_READ, op.fd,
                      from, static_cast<unsigned>(op.length() - op.filled),
                      op.off + op.filled, ud, offload_);
        requeued = true;
        continue;
      }
      complete(op, {});
      ran++;
    }
    if (requeued) {
      // The continuation of a short read is the same kind of entry as
      // the read itself, and the same refusal leaves it owed.
      try {
        ring_.flush();
      } catch (const std::system_error&) {
        defer_unsubmitted();
      }
    }
    return ran;
  }

  // At most one per poll(): a synchronous read holds the calling thread
  // for the whole window, and the caller asked to be given control back.
  std::size_t run_one_deferred() noexcept {
    read_op* const next = deferred_.take();
    if (next == nullptr) {
      return 0;
    }
    read_op& op = *next;
    std::error_code ec;
    if (op.cancelled) {
      ec = std::make_error_code(std::errc::operation_canceled);
    } else {
      try {
        if (op.is_write) {
          op.sync_file->pwrite_all(op.sync_file->sync_fd(op.wbuf.size()),
                                   op.wbuf.data(), op.wbuf.size(), op.off);
        } else {
          op.sync_file->pread_all(op.sync_file->sync_fd(op.buf.size()),
                                  op.buf.data(), op.buf.size(), op.off);
        }
        op.filled = op.length();
      } catch (const std::system_error& e) {
        ec = e.code();
      }
    }
    complete(op, ec);
    return 1;
  }

  // Drains the eventfd counter, re-arming the ring's one-shot poll. True
  // means a wake() had been issued.
  bool take_wake() noexcept {
    const bool woken = waiter_.take();
    if (use_uring_ && !wake_armed_) {
      arm_wake();
      try {
        ring_.flush();
      } catch (const std::system_error&) {
        // The arm did not reach the kernel. It is withdrawn rather than
        // left queued, so wake_armed_ says what is true: nothing watches
        // the eventfd, and sleep_once() must not block on the completion
        // queue for a wake that cannot arrive. The next blocking poll
        // arms again.
        ring_.take_unsubmitted([this](std::uint64_t ud) noexcept {
          if (ud == wake_ud()) {
            wake_armed_ = false;
          }
        });
      }
    }
    return woken;
  }

  // Blocks until something can complete. With the wake poll armed, or
  // reads in the kernel, that is the completion queue; with neither,
  // the only event that can end the sleep is a wake() on the eventfd,
  // so that is what is waited on.
  void sleep_once() {
    if (use_uring_ && (wake_armed_ || in_flight_ > 0)) {
      ring_.wait_cq();
      return;
    }
    waiter_.sleep();
  }

  // Declaration order is the teardown contract: the ring drains, and
  // with it the one-shot poll armed on the eventfd, before the eventfd
  // is closed and before the op whose address that poll wears goes.
  // Do not move ring_ above these two.
  poll_waiter waiter_;
  read_op wake_op_{};    // address only: the wake completion's user_data
  read_op cancel_op_{};  // address only: every cancel CQE's user_data
  uring ring_;
  deferred_ops<read_op> deferred_;
  std::size_t in_flight_ = 0;
  bool async_requested_ = true;
  bool use_uring_ = false;
  bool offload_ = false;
  bool wake_armed_ = false;
};

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(io_context<uring_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // __linux__
