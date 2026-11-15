#pragma once

// The Linux backend: io_uring driven through raw syscalls (three of them:
// setup, enter, and mmap for the rings), with no liburing dependency, so
// every moving part is visible. Degrades per-feature at RUNTIME inside
// this class: O_DIRECT refused by the filesystem -> buffered io_uring;
// io_uring refused (seccomp, old kernel) -> pread on the deferred pump's
// lanes. Internal to src/io/, never installed.
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
#include <array>
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
#include "io/completion_context.hpp"
#include "io/posix_file.hpp"

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
      // Either io_uring_setup is absent (pre-5.1) or the ring came up
      // without the read/write opcodes this backend needs (pre-5.6); both
      // are the same answer to a caller: this kernel cannot serve it.
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

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// The test executable compiles this backend with a seam, under a name of
// its own: the library's copy, which the same executable links, stays one
// definition, and nothing shipped carries the seam.
inline namespace test_seams {
// While set, every submission is refused as io_uring_enter would refuse
// one, which no working kernel can be asked to do.
inline std::atomic<bool> refuse_submit{false};
// While positive, each submission takes nothing, as an enter answering 0
// does, and the count drops; then refuse_submits, the same way, refuses.
inline std::atomic<unsigned> stall_submits{0};
inline std::atomic<unsigned> refuse_submits{0};

[[nodiscard]] inline bool take_seam(std::atomic<unsigned>& count) noexcept {
  unsigned n = count.load();
  while (n != 0 && !count.compare_exchange_weak(n, n - 1)) {
  }
  return n != 0;
}
#endif

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
    // A ring that sets up only proves the syscall exists, which is 5.1.
    // IORING_OP_READ and IORING_OP_WRITE -- the only two opcodes this
    // backend uses -- arrived in 5.6, and on 5.1 to 5.5 every transfer
    // would come back -EINVAL while backend() happily reported io_uring:
    // Ubuntu 20.04 ships 5.4. IORING_FEAT_RW_CUR_POS is from the same
    // release as those opcodes, so the feature word answers the question
    // that IORING_REGISTER_PROBE cannot (it is itself 5.6).
    if ((p.features & IORING_FEAT_RW_CUR_POS) == 0) {
      setup_errno = ENOSYS;
      destroy();
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
  //
  // Nothing here checks for a full queue: the context sizes the ring for
  // twice the transfers it was built for plus the wake poll, and every
  // caller keeps under that count -- a pipeline holds at most one read per
  // window, and builds its driver for its window count. The assert says
  // what that rests on.
  void push_sqe(const io_uring_sqe& sqe) noexcept {
    const unsigned tail = *sq_tail;
    assert(tail - std::atomic_ref<unsigned>(*sq_head).load(
                      std::memory_order_acquire) <
               sq_entries &&
           "the submission queue is sized for every entry in flight");
    const unsigned idx = tail & *sq_mask;
    std::memcpy(sqes + idx * sizeof(io_uring_sqe), &sqe, sizeof(sqe));
    std::memcpy(sq_array + idx * sizeof(unsigned), &idx, sizeof(idx));
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
  }

  // Hands the kernel everything queued since it last looked, in one
  // enter. It submits the whole head-to-tail span, so a failed enter is
  // recoverable: its entries stay published, and the next flush picks them
  // up in order. Returns 0, or the errno of an enter the kernel refused,
  // which is an expected answer rather than a failure of this call.
  [[nodiscard]] int flush() noexcept {
    for (;;) {
      const unsigned tail = *sq_tail;
      const unsigned head =
          std::atomic_ref<unsigned>(*sq_head).load(std::memory_order_acquire);
      if (tail == head) {
        return 0;
      }
#if defined(BLAKE3PP_TEST_IO_SEAMS)
      if (take_seam(stall_submits)) {
        return 0;  // n == 0: the entries stay published
      }
      if (refuse_submit.load() || take_seam(refuse_submits)) {
        return EAGAIN;
      }
#endif
      const int n = sys_io_uring_enter(fd.get(), tail - head, 0, 0);
      if (n >= 0) {
        outstanding += static_cast<unsigned>(n);
        // An enter may take fewer than it was offered, and the rest stay
        // published. In steady state the next flush would pick them up, but
        // forget() waits out a count that includes them, so they are
        // offered again here. n == 0 is no progress: leave them, and
        // uring_engine::flush() takes them back.
        if (n == 0 || static_cast<unsigned>(n) == tail - head) {
          return 0;
        }
        continue;
      }
      if (errno != EINTR) {
        return errno;
      }
    }
  }

  // Whether entries are still published that no enter has taken: what a
  // flush that returned 0 without taking everything leaves behind.
  [[nodiscard]] bool unsubmitted() const noexcept {
    return *sq_tail !=
           std::atomic_ref<unsigned>(*sq_head).load(std::memory_order_acquire);
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

// The io_uring engine under completion_context: one ring, its pieces,
// its completions, and the eventfd a sleeping poll is woken through.
// Degrades per feature at runtime, as the whole layer does: O_DIRECT
// refused by the filesystem -> buffered, io_uring refused -> every
// transfer performed by the deferred pump's lanes.
class uring_engine {
 public:
  using transfer_part = posix_transfer;

  // An open file: the plain descriptor, the O_DIRECT reopen when one
  // engaged, its size and the name tools print. Every file must be
  // destroyed before the context it was opened on, which keeps these
  // descriptors open while the context's drain waits out the transfers
  // on them.
  class file_part : public posix_file {
   public:
    file_part(const uring_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      const int base = how.write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = how.write ? 0 : stat_size();
      // Preallocation belongs to the OPEN, not to the finish: a write
      // that extends the file serializes on the inode lock, and with
      // async direct I/O that collapses the whole queue to one stalled
      // write at a time. set_size() trims back afterwards. A refusal
      // costs only that: the writes extend the file as they land.
      if (how.write && how.preallocate_bytes > 0) {
        [[maybe_unused]] const int refused =
            ::fallocate(fd_plain(), 0, 0,
                        static_cast<off_t>(how.preallocate_bytes));
      }
      if (how.direct_io) {
        // Not TRUNC on the reopen: already truncated, and the tail keeps
        // a plain fd.
        try_odirect(path.c_str(), (how.write ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
      }
      name_ = e.name_for(direct(), how.write);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (::ftruncate(fd_plain(), static_cast<off_t>(bytes)) != 0) {
        throw_errno("ftruncate");
      }
    }

   private:
    std::uint64_t size_ = 0;
    std::string name_;
  };

  uring_engine(const reader_context_options& opts, unsigned max_inflight)
      : ring_depth_(std::max(1u, max_inflight)), async_requested_(opts.async) {
    // What one batch between two flushes can hold: a transfer and its
    // continuation for every slot, plus the armed wake poll and one
    // spare. A cancel sweep fits inside the same bound: the scope stops
    // starting transfers before it cancels them, so the two never share
    // a batch.
    if (opts.async && ring_.init(2 * max_inflight + 2)) {
      use_uring_ = true;
      offload_ = opts.offload_submit;
      // A refused enter takes the wake poll back rather than failing the
      // open; the next flush arms it again.
      arm_wake();
      if (ring_.flush() != 0 || ring_.unsubmitted()) {
        take_back<posix_transfer>([](posix_transfer&) noexcept {});
      }
    }
  }
  uring_engine(const uring_engine&) = delete;
  uring_engine& operator=(const uring_engine&) = delete;

  // This engine's rung and the one it degrades to, declared for the
  // ladder walk in src/io/io_engines.cpp. A ring that could not be had
  // leaves every transfer to posix_file's pread on the pump's lanes.
  static constexpr io_engine rung_engine = io_engine::io_uring;
  // What failed, as the shared context's messages name it.
  static constexpr std::string_view mechanism = "io_uring_enter";
  using rung_below = posix_sync_rung;

  // A refusal is settled only when the kernel is the reason. ENOSYS says
  // io_uring is not in this kernel and EPERM says a policy forbids it;
  // EMFILE or ENOMEM say the ring could not be set up at this moment, and
  // a caller that asks again after closing something may get the ring,
  // which is why best_io_engine() may re-probe.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    if (use_uring_) {
      return {rung_engine, true};
    }
    return {rung_below::rung_engine,
            async_requested_ && !refusal_may_lift(ring_.setup_errno)};
  }
  [[nodiscard]] bool engaged() const noexcept { return use_uring_; }

  // The ring queues as deep as the caller asked; without it the pump's
  // lanes are the whole of what the device sees.
  [[nodiscard]] unsigned device_depth(bool, unsigned lanes) const noexcept {
    return use_uring_ ? ring_depth_ : lanes;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return perform_rest(op);
  }

  // The rest of a transfer, from op.filled, on the ring through the
  // descriptor direct_piece() picks. What does not ride is a context with
  // no ring and a zero-length transfer, which io_uring would report as
  // res == 0, the signature of a short transfer with nothing left to
  // give.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    const std::size_t len = op.remaining();
    if (!use_uring_ || len == 0) {
      return false;
    }
    const fd_piece piece = direct_piece(*op.sync_file, op.position(), len);
    const void* const data =
        op.is_write ? static_cast<const void*>(op.write_remaining().data())
                    : static_cast<const void*>(op.read_remaining().data());
    // The piece is the kernel's from here, and charged to the device from
    // here.
    op.device_begin();
    ring_.fill_rw(op.is_write ? IORING_OP_WRITE : IORING_OP_READ, piece.fd,
                  data, static_cast<unsigned>(piece.len), op.position(),
                  op_ud(&op), offload_);
    return true;
  }

  template <class Op>
  void cancel(Op& op) noexcept {
    ring_.fill_cancel(op_ud(&op), cancel_ud);
  }

  // A refused enter is not an error the caller can act on: the
  // transfers are already counted, and an exception here would leave
  // them counted and uncompleted. They go to the pump's lanes instead,
  // and so does what an enter that took nothing left published: a wait
  // does not submit, so nothing may stay behind for one. The wake poll
  // is armed again here when a reap took it.
  template <class Op, class Refused>
  void flush(Refused&& refused) noexcept {
    if (!use_uring_) {
      return;
    }
    if (!wake_armed_) {
      arm_wake();
    }
    if (ring_.flush() != 0 || ring_.unsubmitted()) {
      take_back<Op>(refused);
    }
  }

  // Every entry the kernel has not accepted names a transfer that is
  // counted and that the ring will never complete. Handing them back
  // keeps the promise that a counted transfer reaches its callback. The
  // wake's one-shot poll comes back with them; the next flush re-arms it.
  template <class Op, class Refused>
  void take_back(Refused&& refused) noexcept {
    if (!use_uring_) {
      return;
    }
    ring_.take_unsubmitted([&](std::uint64_t ud) noexcept {
      if (ud == wake_ud) {
        wake_armed_ = false;
        return;
      }
      if (ud == cancel_ud) {
        return;  // withdrawn with its target; nothing owes a completion
      }
      Op& op = *op_from<Op>(ud);
      op.device_end();  // the kernel never served it
      refused(op);
    });
  }

  [[nodiscard]] std::error_code take_failure() noexcept { return {}; }

  // One completion, or a wake, or nothing. Blocking, it sleeps until the
  // ring has a completion or the eventfd was signalled, deciding on what
  // the KERNEL holds: with the wake poll armed the completion queue
  // covers both; with nothing in the kernel only the eventfd can end the
  // sleep; with submissions in the kernel and the poll not armed, the
  // ring's descriptor and the eventfd together.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    try {
      for (;;) {
        std::uint64_t ud = 0;
        int res = 0;
        while (use_uring_ && ring_.reap(ud, res)) {
          if (ud == wake_ud) {
            wake_armed_ = false;  // one-shot; the next flush re-arms
            return {event::kind::progress};
          }
          if (ud == cancel_ud) {
            // The target's own completion is the one that matters.
            return {event::kind::progress};
          }
          Op& op = *op_from<Op>(ud);
          if (res < 0) {
            return {event::kind::completion, &op,
                    std::error_code(-res, std::generic_category()), 0};
          }
          if (!op.is_write) {
            msan_unpoison(op.read_remaining().data(),
                          static_cast<std::size_t>(res));
          }
          return {event::kind::completion, &op, {},
                  static_cast<std::size_t>(res)};
        }
        if (!block) {
          return {};
        }
        if (waiter_.take()) {
          return {event::kind::wake};
        }
        sleep_once();
      }
    } catch (const std::system_error& e) {
      return {event::kind::failed, nullptr, e.code()};
    }
  }

  [[nodiscard]] bool take_wake() noexcept { return waiter_.take(); }
  void wake() noexcept { waiter_.wake(); }

  template <class Part>
  void cancel_file(Part&, std::size_t) noexcept {}
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept {
    return use_uring_ ? ring_.outstanding : 0;
  }

  // Entries the kernel never took are withdrawn and the transfers it did
  // take are waited out. The armed wake poll completes only when the
  // eventfd becomes readable, so it is nudged first; the drain reaps it
  // with the rest, and the next flush re-arms it.
  void begin_drain() noexcept {
    if (!use_uring_) {
      return;
    }
    ring_.take_unsubmitted([this](std::uint64_t ud) noexcept {
      if (ud == wake_ud) {
        wake_armed_ = false;
      }
    });
    if (wake_armed_) {
      waiter_.wake();
    }
  }
  void end_drain() noexcept { wake_armed_ = false; }

  template <class Op>
  void release(Op&) noexcept {}

  // Which rung of the ladder this engine reached, for a file that did or
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

 private:
  template <class Op>
  [[nodiscard]] static std::uint64_t op_ud(Op* op) noexcept {
    return static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(op));
  }
  template <class Op>
  [[nodiscard]] static Op* op_from(std::uint64_t ud) noexcept {
    return std::bit_cast<Op*>(static_cast<std::uintptr_t>(ud));
  }
  // The wake completion needs a user_data no transfer can wear, and so
  // does a cancel's CQE: reaping either as a transfer would walk to one
  // that does not exist. A transfer's user_data is its aligned address,
  // which is never 1 or 2.
  static constexpr std::uint64_t wake_ud = 1;
  static constexpr std::uint64_t cancel_ud = 2;
  static_assert(alignof(posix_transfer) > cancel_ud);

  void arm_wake() noexcept {
    ring_.fill_poll_add(waiter_.fd(), wake_ud);
    wake_armed_ = true;
  }

  void sleep_once() {
    if (!use_uring_ || (!wake_armed_ && ring_.outstanding == 0)) {
      waiter_.sleep();
      return;
    }
    if (wake_armed_) {
      ring_.wait_cq();
      return;
    }
    std::array<::pollfd, 2> fds{{{ring_.fd.get(), POLLIN, 0},
                                 {waiter_.fd(), POLLIN, 0}}};
    while (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno != EINTR) {
        throw_errno("poll(ring, wake)");
      }
    }
  }

  // Declaration order is the teardown contract: the ring drains, and
  // with it the one-shot poll armed on the eventfd, before the eventfd
  // is closed. Do not move ring_ above waiter_.
  poll_waiter waiter_;
  uring ring_;
  unsigned ring_depth_ = 1;
  bool async_requested_ = true;
  bool use_uring_ = false;
  bool offload_ = false;
  bool wake_armed_ = false;
};

using uring_context = completion_context<uring_engine>;

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(io_context<uring_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // __linux__
