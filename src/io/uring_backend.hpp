#pragma once

// Registered buffers (IORING_REGISTER_BUFFERS + IORING_OP_READ_FIXED)
// measured 28-38% SLOWER here in every round of a four-round alternating
// run, on Linux 6.8 over virtio-scsi; see the exp/registered-buffers
// branch for the implementation, the reports and the hypothesis.
// --inline-submit and offload_submit are unchanged by that result.
//
// The Linux backend: io_uring driven through raw syscalls (three of them:
// setup, enter, and mmap for the rings), with no liburing dependency, so
// every moving part is visible. Degrades per-feature at RUNTIME inside
// this class: O_DIRECT refused by the filesystem -> buffered io_uring;
// io_uring refused (seccomp, old kernel) -> synchronous pread. Internal
// to src/io/, never installed.

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
// tools print. Without it, "blocked by policy" (Android's seccomp filter,
// EPERM) and "kernel too old" (ENOSYS) are indistinguishable in bench
// output.
//
// That would be a fourth kind of quiet degradation the backend line does
// not otherwise confess to, in the same spirit as --version's "cpu also
// supports X (not compiled in)".
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
// Two spellings say so, and both are defined behaviour. start_lifetime_as
// is the one that means it and copies nothing, and is C++23.
// Placement-new creates the object the standard requires, and seeding it
// from the bytes already in the storage is what keeps the value the kernel
// put there; the byte read is legal, and the write puts back what it read.
// Nothing else in the ring is named as an object at all: the descriptors
// and completions are copied in and out, because memcpy creates whatever
// it writes and a local is a real object to read into.
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

// The mapped rings, reduced to what this pipeline needs. Kernel-shared
// integers are accessed through atomic_ref with acquire/release, per the
// io_uring memory-ordering contract.
struct uring {
  int fd = -1;
  // errno from a failed io_uring_setup. EPERM (a seccomp policy forbids
  // the syscall; Android does) and ENOSYS (kernel predates io_uring) are
  // the same observable with entirely different causes, so the fallback
  // name below reports which one fired.
  int setup_errno = 0;
  unsigned sq_entries = 0;
  unsigned cq_entries = 0;
  void* sq_ring = nullptr;
  std::size_t sq_ring_sz = 0;
  void* cq_ring = nullptr;
  std::size_t cq_ring_sz = 0;
  unsigned char* sqes = nullptr;
  std::size_t sqes_sz = 0;
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
    fd = sys_io_uring_setup(entries, &p);
    if (fd < 0) {
      setup_errno = errno;
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
    sqes = static_cast<unsigned char*>(
        ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
    if (sqes == MAP_FAILED) {
      sqes = nullptr;
      return fail();
    }
    auto* sqb = static_cast<unsigned char*>(sq_ring);
    auto* cqb = static_cast<unsigned char*>(cq_ring);
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

  // Reaps every completion still owed. A failure of the wait itself,
  // other than EINTR, ends the loop. Nothing more can be learned from
  // that ring, and the caller's buffers are the only thing left to
  // protect.
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
      if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
          errno != EINTR) {
        break;
      }
    }
  }

  void destroy() noexcept {
    drain();
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

  // Queues one READ or WRITE without entering the kernel; the sole
  // submitter, so sq_tail needs no CAS. flush() is what hands it over.
  void fill_rw(std::uint8_t opcode, int file_fd, const void* buf,
               unsigned len, std::uint64_t off, std::uint64_t user_data,
               bool offload = false) noexcept {
    const unsigned tail = *sq_tail;  // we are the only writer
    const unsigned idx = tail & *sq_mask;
    io_uring_sqe sqe{};
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = opcode;
    if (offload) {
      // Issue on io-wq rather than inline. Kernels before ~6.x bailed out
      // of the inline attempt on a large O_DIRECT read and punted anyway;
      // newer ones complete it inline, 1.5-1.9 ms of pinning, splitting
      // and queueing per 64 MiB on the submitting thread (four PCIe 5
      // drives, kernel 7.0). Serialized with the hash that thread also
      // waits for, that halved the pipeline; asking for the hand-off
      // restored it (22 -> 41 GiB/s).
      sqe.flags |= IOSQE_ASYNC;
    }
    sqe.fd = file_fd;
    sqe.addr = static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(buf));
    sqe.len = len;
    sqe.off = off;
    sqe.user_data = user_data;
    std::memcpy(sqes + idx * sizeof(io_uring_sqe), &sqe, sizeof(sqe));
    std::memcpy(sq_array + idx * sizeof(unsigned), &idx, sizeof(idx));
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
  }

  // Queues a one-shot poll on `poll_fd`, the wake path's arming step.
  void fill_poll_add(int poll_fd, std::uint64_t user_data) noexcept {
    const unsigned tail = *sq_tail;
    const unsigned idx = tail & *sq_mask;
    io_uring_sqe sqe{};
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_POLL_ADD;
    sqe.fd = poll_fd;
    sqe.poll_events = POLLIN;
    sqe.user_data = user_data;
    std::memcpy(sqes + idx * sizeof(io_uring_sqe), &sqe, sizeof(sqe));
    std::memcpy(sq_array + idx * sizeof(unsigned), &idx, sizeof(idx));
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
  }

  // Hands the kernel everything queued since it last looked, in one
  // enter. Submitting the whole head-to-tail span rather than a fixed
  // count is what keeps a failed enter recoverable: its entries stay
  // published, and the next flush picks them up in order instead of
  // shifting every later completion by a slot.
  void flush() {
    for (;;) {
      const unsigned tail = *sq_tail;
      const unsigned head =
          std::atomic_ref<unsigned>(*sq_head).load(std::memory_order_acquire);
      if (tail == head) {
        return;
      }
      const int n = sys_io_uring_enter(fd, tail - head, 0, 0);
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
  // one-at-a-time shape, unchanged.
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
    if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
        errno != EINTR) {
      throw_errno("io_uring_enter(wait)");
    }
  }

  // Blocks for one completion and returns (user_data, result).
  std::pair<std::uint64_t, int> wait_one() {
    for (;;) {
      const unsigned head = *cq_head;  // we are the only consumer
      const unsigned tail =
          std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
      if (head != tail) {
        io_uring_cqe cqe{};
        std::memcpy(&cqe, cqes + (head & *cq_mask) * sizeof(io_uring_cqe),
                    sizeof(cqe));
        const std::pair<std::uint64_t, int> out{cqe.user_data, cqe.res};
        std::atomic_ref<unsigned>(*cq_head).store(head + 1,
                                                  std::memory_order_release);
        outstanding -= std::min(1u, outstanding);
        return out;
      }
      if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
          errno != EINTR) {
        throw_errno("io_uring_enter(wait)");
      }
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
    std::span<std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t filled = 0;
    int fd = -1;                      // the fd this read rides
    posix_file* sync_file = nullptr;  // the deferred path needs the pair
    read_op* next_deferred = nullptr;
  };

  // An open file bound to a context. Every file must be destroyed before
  // the context it was opened on: the context's drain writes into buffers
  // these fds are reading into.
  class file {
   public:
    file(uring_context& ctx, const std::filesystem::path& path,
         bool direct_io) {
      f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
      size_ = f_.stat_size();
      if (direct_io) {
        f_.try_odirect(path.c_str(), O_RDONLY | O_CLOEXEC);
      }
      name_ = ctx.name_for(f_.direct);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

   private:
    friend class uring_context;
    posix_file f_;
    std::uint64_t size_ = 0;
    std::string name_;
  };

  uring_context(const reader_context_options& opts, unsigned max_inflight)
      : async_requested_(opts.async) {
    // Two entries beyond the reads: the armed wake poll, and one spare so
    // a short read's continuation always finds a free entry even with
    // every slot in flight.
    if (opts.async && ring_.init(2 * max_inflight + 2)) {
      use_uring_ = true;
      offload_ = opts.offload_submit;
      arm_wake();
      ring_.flush();
    }
  }

  uring_context(const uring_context&) = delete;
  uring_context& operator=(const uring_context&) = delete;

  ~uring_context() {
    // The armed poll completes only when the eventfd becomes readable,
    // and ~uring drains everything outstanding before it unmaps: without
    // this nudge that drain waits forever. Runs before any member is
    // destroyed, and the eventfd itself outlives the ring by declaration
    // order, so the nudge lands on an open descriptor and the drain
    // reaps the poll it completes.
    if (wake_armed_) {
      waiter_.wake();
    }
    // ~uring drains the rest. No callback runs from here, by contract.
  }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   read_op& op) {
    op.buf = buf;
    op.off = off;
    op.filled = 0;
    op.fd = f.f_.fd;
    op.sync_file = &f.f_;
    op.next_deferred = nullptr;
    in_flight_++;
    // O_DIRECT rejects an unaligned length, so only whole granules may
    // ride the ring; the tail is read synchronously inside poll(). This
    // is the whole runtime-degradation question, asked in one place.
    if (use_uring_ && buf.size() % direct_align == 0) {
      ring_.fill_rw(IORING_OP_READ, op.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, op_ud(&op),
                    offload_);
    } else {
      deferred_.push(op);
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
      read_op& op = *op_from(ud);
      op.next_deferred = nullptr;
      deferred_.push(op);
    });
  }

 private:
  // Which rung of the ladder this context reached, for a file that did or
  // did not get O_DIRECT. file_reader::backend() reports it verbatim, so
  // the spellings, the order they combine in and the suffixes are
  // observable API: tools print it, and a run is read differently
  // depending on which rung it names.
  [[nodiscard]] std::string name_for(bool direct) const {
    std::string n = use_uring_ ? (direct ? "io_uring+direct" : "io_uring")
                               : (direct ? "pread+direct" : "pread");
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
  // op's address is one, and costs nothing.
  [[nodiscard]] std::uint64_t wake_ud() const noexcept {
    return static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(&wake_op_));
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
      BLAKE3PP_MSAN_UNPOISON(op.buf.data() + op.filled,
                             static_cast<std::size_t>(res));
      op.filled += static_cast<std::size_t>(res);
      if (op.filled < op.buf.size()) {
        ring_.fill_rw(IORING_OP_READ, op.fd, op.buf.data() + op.filled,
                      static_cast<unsigned>(op.buf.size() - op.filled),
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
    try {
      op.sync_file->pread_all(op.sync_file->sync_fd(op.buf.size()),
                              op.buf.data(), op.buf.size(), op.off);
      op.filled = op.buf.size();
    } catch (const std::system_error& e) {
      ec = e.code();
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
        // The arm will go out with the next read's flush; a wake in the
        // meantime still breaks the sleep through the eventfd itself.
      }
    }
    return woken;
  }

  void sleep_once() {
    if (use_uring_) {
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
  read_op wake_op_{};  // address only: the wake completion's user_data
  uring ring_;
  deferred_ops<read_op> deferred_;
  std::size_t in_flight_ = 0;
  bool async_requested_ = true;
  bool use_uring_ = false;
  bool offload_ = false;
  bool wake_armed_ = false;
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
      offload_ = opts.offload_submit;
    }
    name_ = use_uring_ ? (f_.direct ? "io_uring+direct" : "io_uring")
                       : (f_.direct ? "pwrite+direct" : "pwrite");
    if (use_uring_ && !offload_) {
      name_ += " (inline submit)";
    }
    if (opts.async && !use_uring_ && ring_.setup_errno != 0) {
      name_ += no_uring_suffix(ring_.setup_errno);
    }
  }

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return use_uring_ && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    // The engine asks wants_async() first, which is false without a
    // ring; reaching here degraded would submit through null ring
    // pointers. The synchronous writers state the same invariant with
    // the same assert.
    assert(use_uring_ && "the uring writer has no async path without a ring");
    slots_[s] = {buf, off, 0, true};
    ring_.submit_rw(IORING_OP_WRITE, f_.fd, buf.data(),
                    static_cast<unsigned>(buf.size()), off, s, offload_);
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
                      st.off + st.done, s, offload_);
    } else {
      st.busy = false;
    }
  }

  posix_file f_;
  uring ring_;
  std::vector<slot> slots_;
  std::uint64_t prealloc_ = 0;
  bool use_uring_ = false;
  bool offload_ = false;
  std::string name_ = "pwrite";
};

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(reader_context<uring_context>);
static_assert(writer_backend<uring_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __linux__
