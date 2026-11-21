#pragma once

// The FreeBSD backend: POSIX AIO submitted in batches through
// lio_listio(LIO_NOWAIT), with every request's completion delivered to
// one kqueue as an EVFILT_AIO event. Pull-style like io_uring, and
// driven the same way: a pending list filled by submit and handed to the
// kernel by flush(), completions reaped and reported only inside poll().
// Degrades per feature at RUNTIME inside this class: O_DIRECT refused by
// the filesystem -> buffered AIO; no kqueue, or no AIO queue to put a
// request on -> pread on the deferred pump's lanes. Internal to src/io/,
// never installed.
//
// O_DIRECT on FreeBSD is a hint:
//  - UFS honours it for page-aligned buffers, offsets and lengths and
//    serves anything else through the cache.
//  - ZFS honours it from OpenZFS 2.3 under the dataset's `direct`
//    property. `direct=standard` (the default) serves requests whose
//    offset and length are 4 KiB aligned and answers EINVAL for the rest,
//    `direct=disabled` ignores the flag, and `direct=always` takes every
//    request.
//  - ZFS before 2.3 has no direct path and ignores the flag.
// The context keeps one rule for all of them: whole granules at a granule
// offset ride the direct descriptor, the rest rides the plain one. A
// reader also issues one page-sized pread through the direct descriptor
// at open, so a filesystem that refuses it says so in the file's name
// instead of in the middle of a run. The probe answers only whether the
// descriptor is served: a `+direct` rung on a filesystem that ignores the
// flag still reads through that filesystem's cache.

#if defined(__FreeBSD__)

#include <aio.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "io/fd.hpp"
#include "io/io_context.hpp"
#include "io/ladder.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"
#include "io/waiter.hpp"
#include "io/completion_context.hpp"
#include "io/posix/file.hpp"

namespace blake3pp::detail::io_impl {

// Names the reason AIO is absent, for the fallback backend name the tools
// print: a kernel without the facility (the sysctl tree is not there) and
// a per-process queue closed by the administrator are otherwise
// indistinguishable in bench output.
inline constexpr std::string_view aio_eagain_suffix = " [aio: EAGAIN]";
inline constexpr std::string_view aio_enosys_suffix =
    " [aio: ENOSYS, no kernel AIO]";
inline constexpr std::string_view aio_errno_prefix = " [aio: errno ";
inline constexpr std::string_view kqueue_errno_prefix = " [kqueue: errno ";
// A direct descriptor the filesystem would not serve (see direct_serves()).
inline constexpr std::string_view direct_einval_suffix = " [direct: EINVAL]";

inline std::string no_aio_suffix(int err) {
  switch (err) {
    case EAGAIN:
      // vfs.aio.max_aio_queue_per_proc is zero: the facility is there and
      // will take nothing, which is what a refused lio_listio reports.
      return std::string(aio_eagain_suffix);
    case ENOSYS:
      return std::string(aio_enosys_suffix);
    default:
      return std::string(aio_errno_prefix) + std::to_string(err) + "]";
  }
}

// The same for the completion queue itself: without a kqueue there is
// nowhere for a completion to arrive, whatever the AIO limits say.
inline std::string no_kqueue_suffix(int err) {
  return std::string(kqueue_errno_prefix) + std::to_string(err) + "]";
}

// The longest reason either gives, its errno at the widest.
inline constexpr std::size_t no_aio_suffix_max =
    std::max({aio_eagain_suffix.size(), aio_enosys_suffix.size(),
              aio_errno_prefix.size() + int_text_max + 1,
              kqueue_errno_prefix.size() + int_text_max + 1});

// What the kernel will let this process keep queued at once, or zero when
// the facility is absent. Read once per context: the batch below is cut
// to what is still free, so the per-process limit never turns a flush
// into a refusal the backend has to unpick. The sysctl is an int; the
// count comes back in the type the queue arithmetic uses.
[[nodiscard]] inline std::size_t aio_queue_limit(int& why) noexcept {
  int limit = 0;
  std::size_t len = sizeof limit;
  if (::sysctlbyname("vfs.aio.max_aio_queue_per_proc", &limit, &len, nullptr,
                     0) != 0) {
    why = ENOSYS;
    return 0;
  }
  if (limit <= 0) {
    why = EAGAIN;
    return 0;
  }
  return static_cast<std::size_t>(limit);
}

// How many aiocbs one lio_listio may carry. AIO_LISTIO_MAX is the
// standard's name for it and sysconf is the one that answers at runtime.
[[nodiscard]] inline std::size_t aio_listio_max() noexcept {
  const long n = ::sysconf(_SC_AIO_LISTIO_MAX);
  return n > 0 ? static_cast<std::size_t>(n) : 16;
}

// The FreeBSD engine under completion_context: one kqueue, POSIX AIO
// requests from a pool the engine owns, offered to the kernel in batches.
// Degrades per feature at runtime, as the whole layer does: O_DIRECT
// refused by the filesystem -> buffered, AIO refused -> every transfer
// served by the deferred pump's own threads.
class aio_engine {
 public:
  struct request;  // the engine-owned aiocb a transfer rides on

  struct transfer_part : posix_transfer {
    // The kernel's half of this transfer, borrowed from the engine's pool
    // while the transfer is queued or in flight and given back when it
    // finishes; null for one on the synchronous rung.
    request* req = nullptr;
    transfer_part* next_pending = nullptr;  // queued, not yet offered
    bool issued = false;  // the kernel has it; an event is owed

    template <class Part>
    void arm_read(Part& f, std::uint64_t at, std::span<std::byte> to) noexcept {
      posix_transfer::arm_read(f, at, to);
      reset();
    }
    template <class Part>
    void arm_write(Part& f, std::uint64_t at,
                   std::span<const std::byte> from) noexcept {
      posix_transfer::arm_write(f, at, from);
      reset();
    }

   private:
    void reset() noexcept {
      req = nullptr;
      next_pending = nullptr;
      issued = false;
    }
  };

  // One kernel request, owned by the engine. The kernel reads the aiocb
  // when the request is queued, writes the status into it when the
  // request finishes, and aio_return() writes into it again when the
  // result is taken. A drain reaps every request through the pool, and
  // `op` says whether a transfer is still owed a report.
  //
  // Keeping the aiocb out of the transfer also keeps the caller's
  // operation the size of the other backends'. A struct aiocb is 160 bytes
  // on amd64, a sigevent included, and io_op::storage would have to grow on
  // every platform to hold one.
  struct request {
    struct aiocb cb {};
    transfer_part* op = nullptr;
    request* next_free = nullptr;
  };

  class file_part : public posix_file {
   public:
    file_part(const aio_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      const int base = how.write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = how.write ? 0 : stat_size();
      // Preallocation belongs to the OPEN, not to the finish: a write
      // that extends the file serializes on the inode lock, and with
      // async direct I/O that collapses the whole queue to one stalled
      // write at a time. set_size() trims back afterwards.
      //
      // posix_fallocate returns the error rather than setting errno, and
      // a filesystem that cannot reserve space says so instead of
      // failing the open: UFS reserves, ZFS answers EINVAL and the writes
      // extend the file as they land, which is what every backend without
      // preallocation does anyway.
      if (how.write && how.preallocate_bytes > 0) {
        [[maybe_unused]] const int refused =
            ::posix_fallocate(fd_plain(), 0,
                              static_cast<off_t>(how.preallocate_bytes));
      }
      bool direct_refused = false;
      if (how.direct_io) {
        // Not TRUNC on the reopen: already truncated, and the tail keeps
        // a plain fd.
        try_odirect(path.c_str(), (how.write ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
        if (direct() && !how.write && !direct_serves()) {
          drop_direct();
          direct_refused = true;
        }
      }
      name_ = e.name_for(direct(), how.write, direct_refused);
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
    // Whether the direct descriptor will serve the file at all: one
    // page-sized read at offset zero, the smallest request the alignment
    // rules accept. EINVAL is a filesystem refusing the descriptor
    // (OpenZFS `direct=standard` on a dataset whose geometry this request
    // does not suit). Every other answer, a short one or an end-of-file
    // zero included, means it serves. Readers only: a writable open has
    // just truncated the file to nothing, and every window it sends down
    // the direct descriptor is a whole granule at a granule offset.
    [[nodiscard]] bool direct_serves() const noexcept {
      alignas(direct_align) std::byte page[direct_align];
      const ssize_t n = ::pread(fd(), page, sizeof page, 0);
      return n >= 0 || errno != EINVAL;
    }

    std::uint64_t size_ = 0;
    std::string name_;
  };

  aio_engine(const reader_context_options& opts, unsigned max_inflight)
      : queue_depth_(std::max(1u, max_inflight)),
        async_requested_(opts.async) {
    if (!opts.async) {
      return;
    }
    kq_.reset(::kqueue());
    if (!kq_) {
      refusal_ = no_kqueue_suffix(errno);
      return;
    }
    int why = 0;
    queue_limit_ = aio_queue_limit(why);
    if (queue_limit_ == 0) {
      refusal_ = no_aio_suffix(why);
      kq_.reset();
      return;
    }
    // The wake is a user event on the same queue, armed once and left
    // armed: EV_CLEAR hands each trigger over exactly once, so a wake
    // nobody is waiting for is still delivered to the next kevent().
    struct kevent ev;
    EV_SET(&ev, wake_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr) < 0) {
      refusal_ = no_kqueue_suffix(errno);
      kq_.reset();
      return;
    }
    use_aio_ = true;
    batch_.resize(std::min<std::size_t>(
        aio_listio_max(), queue_limit_));
    // What one wait can hand back: a completion for every slot, its
    // continuation, and the wake.
    events_.resize(std::size_t{2} * max_inflight + 2);
  }
  aio_engine(const aio_engine&) = delete;
  aio_engine& operator=(const aio_engine&) = delete;

  // Why the kqueue or the AIO queue was not had, for the tests that probe
  // the machine rather than assume which rung a host reaches.
  [[nodiscard]] std::string_view refusal() const noexcept { return refusal_; }

  static constexpr io_engine rung_engine = io_engine::aio;
  // What failed, as the shared context's messages name it.
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return "kevent";
  }
  using rung_below = posix_sync_rung;

  // A refused AIO is never settled here, unlike a refused io_uring on
  // Linux. Every reason the constructor can record is something an
  // administrator or another process can undo while this one runs: the
  // queue limit is the tunable vfs.aio.max_aio_queue_per_proc, the
  // facility itself is a kernel module that can be loaded (the sysctl's
  // absence is how that reads from here), and a kqueue that could not be
  // opened is a descriptor this process may get back. So the verdict is
  // re-probed each time it is asked for.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return use_aio_ ? rung_verdict{rung_engine, true}
                    : rung_verdict{rung_below::rung_engine, false};
  }
  [[nodiscard]] bool engaged() const noexcept { return use_aio_; }

  // The kernel queues as deep as the caller asked; without AIO the pump's
  // lanes are the whole of what the device sees.
  [[nodiscard]] unsigned device_depth(bool, unsigned lanes) const noexcept {
    return use_aio_ ? queue_depth_ : lanes;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return perform_rest(op);
  }

  // Fills the request for the rest of the transfer and queues it for the
  // next batch, or declines when the kernel cannot take its shape (a
  // context with no AIO, a zero-length transfer) or the pool could not
  // grow. A continuation keeps the request its first piece rode on, which
  // is free to refill once aio_return has been called.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    const std::size_t len = op.remaining();
    if (!use_aio_ || len == 0) {
      release(op);
      return false;
    }
    const fd_piece piece = direct_piece(*op.sync_file, op.position(), len);
    request* const req = op.req != nullptr ? op.req : acquire_request();
    if (req == nullptr) {
      return false;
    }
    op.req = req;
    req->op = &op;
    std::memset(&req->cb, 0, sizeof req->cb);
    req->cb.aio_fildes = piece.fd;
    req->cb.aio_offset = static_cast<off_t>(op.position());
    // aio_buf is `volatile void *`, so a write's source loses its const
    // on the way in; the kernel only reads it, which is what LIO_WRITE
    // means.
    req->cb.aio_buf = op.is_write
                          ? const_cast<std::byte*>(op.write_remaining().data())
                          : op.read_remaining().data();
    req->cb.aio_nbytes = piece.len;
    req->cb.aio_lio_opcode = op.is_write ? LIO_WRITE : LIO_READ;
    req->cb.aio_sigevent.sigev_notify = SIGEV_KEVENT;
    req->cb.aio_sigevent.sigev_notify_kqueue = kq_.get();
    // One event per request: the request is reaped by the event, and the
    // same aiocb is refilled for the continuation afterwards.
    req->cb.aio_sigevent.sigev_notify_kevent_flags = EV_ONESHOT;
    req->cb.aio_sigevent.sigev_value.sival_ptr = req;
    pending_.push(op);
    return true;
  }

  // Advisory: AIO_NOTCANCELED means the request is past cancelling and
  // will report its data, which the contract allows. A transfer still
  // queued here is flagged by the context, and the next flush hands it
  // to the pump, where it reports operation_canceled unissued.
  template <class Op>
  void cancel(Op& op) noexcept {
    if (op.issued && op.req != nullptr) {
      (void)::aio_cancel(op.req->cb.aio_fildes, &op.req->cb);
    }
  }

  // Hands the kernel everything queued since it last looked, in batches
  // of AIO_LISTIO_MAX cut to what the per-process queue still holds. A
  // transfer the kernel will not take for a lasting reason goes to
  // refused(); one refused for now stays queued for the next flush. With
  // nothing in the kernel, the oldest one goes to refused() too, or a
  // blocking poll() would wait for an event that cannot arrive.
  template <class Op, class Refused>
  void flush(Refused&& refused) noexcept {
    if (!use_aio_) {
      return;
    }
    pending_.remove_if([&](transfer_part& p) noexcept {
      if (!p.cancelled) {
        return false;
      }
      release(p);
      refused(static_cast<Op&>(p));
      return true;
    });
    while (!pending_.empty()) {
      const std::size_t n = fill_batch();
      if (n == 0 || !offer<Op>(n, refused)) {
        break;  // the queue is full, or refused the whole batch: next flush
      }
    }
    if (issued_ == 0 && !pending_.empty()) {
      transfer_part* const p = pending_.take();
      release(*p);
      refused(static_cast<Op&>(*p));
    }
  }

  // Every transfer the kernel has not been offered is given to refused(),
  // which is what a flush the kernel refuses outright leaves behind.
  template <class Op, class Refused>
  void take_back(Refused&& refused) noexcept {
    while (transfer_part* const p = pending_.take()) {
      release(*p);
      refused(static_cast<Op&>(*p));
    }
  }
  [[nodiscard]] std::error_code take_failure() noexcept { return {}; }

  // One completion, or a wake, or nothing. A kevent() batch is handed out
  // one event at a time; the wake is a user event on the same queue.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    if (!use_aio_) {
      // No queue to wait on: the only event that can end a sleep is a
      // wake, and that is what the waiter is for.
      for (;;) {
        if (waiter_.take()) {
          return {event::kind::wake};
        }
        if (!block) {
          return {};
        }
        waiter_.sleep();
      }
    }
    for (;;) {
      while (cursor_ < got_) {
        const struct kevent& ev = events_[cursor_++];
        if (ev.filter == EVFILT_USER) {
          return {event::kind::wake};
        }
        request* const req = static_cast<request*>(ev.udata);
        const reaped r = take_result(*req);
        if (req->op == nullptr) {
          release_request(req);  // a drain forgot it; nobody is owed
          return {event::kind::progress};
        }
        Op& op = static_cast<Op&>(*req->op);
        op.issued = false;
        if (r.err == ECANCELED) {
          return {event::kind::completion, &op,
                  std::make_error_code(std::errc::operation_canceled), 0};
        }
        if (r.err != 0 || r.res < 0) {
          return {event::kind::completion, &op,
                  std::error_code(r.err != 0 ? r.err : EIO,
                                  std::generic_category()),
                  0};
        }
        if (!op.is_write) {
          msan_unpoison(op.read_remaining().data(),
                        static_cast<std::size_t>(r.res));
        }
        return {event::kind::completion, &op, {},
                static_cast<std::size_t>(r.res)};
      }
      const int got = wait_events(block);
      if (got < 0) {
        return {event::kind::failed, nullptr,
                std::error_code(errno, std::generic_category())};
      }
      cursor_ = 0;
      got_ = static_cast<std::size_t>(got);
      if (got_ == 0) {
        return {};
      }
    }
  }

  // A user event on the kqueue is thread-safe, and a trigger nobody is
  // waiting for stays pending until a kevent() takes it, which is the
  // contract's "a wake with no blocked poll is remembered".
  void wake() noexcept {
    if (!use_aio_) {
      waiter_.wake();
      return;
    }
    struct kevent ev;
    EV_SET(&ev, wake_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr);
  }

  // Cancelling first shortens a closing file's wait; a request that cannot
  // be cancelled is still waited for, because it is writing into a buffer
  // the caller is about to release.
  void cancel_file(file_part& f, std::size_t owed) noexcept {
    if (use_aio_ && owed > 0) {
      (void)::aio_cancel(f.fd(), nullptr);
      (void)::aio_cancel(f.fd_plain(), nullptr);
    }
  }
  // A closing file's share of the queue the kernel was never offered,
  // with no wait and no callback: the request each entry holds goes
  // straight back to the pool.
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&& mine) noexcept {
    return pending_.remove_if([&](transfer_part& p) noexcept {
      if (!mine(static_cast<const Op&>(p))) {
        return false;
      }
      release(p);
      return true;
    });
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return issued_; }

  void begin_drain() noexcept {}
  // The queue is dropped without being walked and the request pool is
  // handed back wholesale, since nothing is in flight by then.
  void end_drain() noexcept {
    pending_.clear();
    free_ = nullptr;
    for (request& r : requests_) {
      r.op = nullptr;
      r.next_free = free_;
      free_ = &r;
    }
    issued_ = 0;
    cursor_ = got_ = 0;
  }

  // Gives back whatever request a transfer is holding: one that is alive
  // and not in the kernel's hands.
  void release(transfer_part& op) noexcept {
    op.device_end();  // whatever it was, the kernel is not serving it now
    if (op.req != nullptr) {
      release_request(op.req);
      op.req = nullptr;
    }
  }

  // Which rung of the ladder this engine reached, for a file that did or
  // did not get O_DIRECT. file_reader::backend() reports it verbatim and
  // tools print it, so the spellings, their order and the suffixes are
  // observable API.
  [[nodiscard]] std::string name_for(bool direct, bool write,
                                     bool direct_refused) const {
    std::string n(
        engine_token(use_aio_ ? io_engine::aio : io_engine::posix_sync, write));
    if (direct) {
      n += direct_tag;
    }
    if (async_requested_ && !use_aio_) {
      n += refusal_;
    }
    if (direct_refused) {
      n += direct_einval_suffix;
    }
    return n;
  }

 private:
  // The user event's identifier. The kqueue is this engine's alone, so
  // any value will do and no request can wear it: an EVFILT_AIO event
  // carries the request's own address instead.
  static constexpr std::uintptr_t wake_ident = 1;
  // What fill_batch() leaves in _aiocb_private.error: no errno, and not
  // EINPROGRESS or 0, so offer() reads an entry the kernel never reached
  // as not taken.
  static constexpr long not_looked_at = -1;

  // A request to put the next piece on, taken from the pool or made.
  // Null means the pool could not grow, and the transfer goes to the pump
  // instead of failing.
  [[nodiscard]] request* acquire_request() noexcept {
    if (free_ != nullptr) {
      request* const r = free_;
      free_ = r->next_free;
      r->next_free = nullptr;
      r->op = nullptr;
      return r;
    }
    try {
      // A deque, so the requests already in the kernel's hands keep their
      // addresses while the pool grows.
      requests_.emplace_back();
      return &requests_.back();
    } catch (...) {
      return nullptr;
    }
  }
  void release_request(request* r) noexcept {
    r->op = nullptr;
    r->next_free = free_;
    free_ = r;
  }

  // The next batch of requests, oldest first: as many as one lio_listio
  // may carry and as many as the per-process queue still has room for.
  // The transfers stay queued until the kernel's answer says which of
  // them it took.
  [[nodiscard]] std::size_t fill_batch() noexcept {
    const std::size_t room = queue_limit_ - std::min(issued_, queue_limit_);
    const std::size_t cap = std::min(batch_.size(), room);
    std::size_t n = 0;
    for (transfer_part* op = pending_.front(); op != nullptr && n < cap;
         op = op->next_pending) {
      // A value the kernel never writes there, so an entry it did not
      // reach reads as not taken (see offer()).
      op->req->cb._aiocb_private.error = not_looked_at;
      batch_[n++] = &op->req->cb;
    }
    return n;
  }

  // Offers the batch and books what the kernel took. Returns whether the
  // flush may carry on: a queue that refused everything is asked again at
  // the next flush, not in a loop here.
  //
  // lio_listio answers for the list as a whole:
  //  - 0 when every request was queued;
  //  - EINVAL or EFAULT when the list was rejected before any entry was
  //    looked at (a list longer than vfs.aio.max_aio_queue_per_proc);
  //  - EIO when some entry was refused for a reason other than room,
  //    else EAGAIN when some entry was refused for room, whatever
  //    happened to the others.
  // So after EIO or EAGAIN each entry has to be asked, and aio_error()
  // cannot tell: it answers from the kernel's job list, where a job the
  // kernel took and that already failed reads like a refusal. FreeBSD's
  // aio_aqueue writes the user copy's _aiocb_private.error instead:
  // EINPROGRESS once the job is committed, the errno when it refuses one,
  // and nothing a finished job writes until aio_return. That field is
  // the answer here.
  template <class Op, class Refused>
  [[nodiscard]] bool offer(std::size_t n, Refused& refused) noexcept {
    const bool all =
        ::lio_listio(LIO_NOWAIT, batch_.data(), static_cast<int>(n),
                     nullptr) == 0;
    const int why = all ? 0 : errno;
    if (!all && why != EIO && why != EAGAIN) {
      return false;  // nothing was queued; the whole batch stays pending
    }
    bool took_any = false;
    pending_.remove_first_if(n, [&](transfer_part& op) noexcept {
      const long err = all ? EINPROGRESS : op.req->cb._aiocb_private.error;
      if (err == EINPROGRESS) {
        // The kernel has it from here, and charged to the device from
        // here.
        op.device_begin();
        op.issued = true;
        issued_++;
        took_any = true;
        return true;
      }
      if (err == EAGAIN || err == not_looked_at || err == 0) {
        return false;  // refused for room, or not reached: still pending
      }
      // Refused for a reason retrying will not change (a filesystem whose
      // files this kernel will not serve asynchronously, with
      // vfs.aio.enable_unsafe off). The pump's lanes take it.
      release(op);
      refused(static_cast<Op&>(op));
      return true;
    });
    return took_any;
  }

  // The kqueue's answer, or -1 for a wait that failed. Retries an
  // interrupted wait.
  [[nodiscard]] int wait_events(bool block) noexcept {
    const struct timespec zero = {0, 0};
    for (;;) {
      const int n = ::kevent(kq_.get(), nullptr, 0, events_.data(),
                             static_cast<int>(events_.size()),
                             block ? nullptr : &zero);
      if (n >= 0 || errno != EINTR) {
        return n;
      }
    }
  }

  // What one completed request says, and the end of the kernel's interest
  // in it: aio_return is what releases the job, so it is called for every
  // delivered event whether or not the result is still wanted.
  struct reaped {
    int err;
    ssize_t res;
  };
  [[nodiscard]] reaped take_result(request& r) noexcept {
    // aio_error answers -1/EINVAL for a request the kernel knows nothing
    // of, which a delivered event rules out; it is normalised here so
    // every caller reads one error number.
    int err = ::aio_error(&r.cb);
    if (err < 0) {
      err = errno;
    }
    const ssize_t res = ::aio_return(&r.cb);
    issued_ -= std::min<std::size_t>(1, issued_);
    return {err, res};
  }

  unique_fd kq_;
  std::size_t queue_limit_ = 0;
  std::string refusal_;  // why the queue was not had, or empty
  std::vector<struct aiocb*> batch_;
  std::vector<struct kevent> events_;
  std::size_t cursor_ = 0;  // the next of events_ next() hands out
  std::size_t got_ = 0;     // how many of events_ the last wait filled
  std::deque<request> requests_;
  request* free_ = nullptr;
  deferred_ops<transfer_part, &transfer_part::next_pending> pending_;
  std::size_t issued_ = 0;  // requests the kernel holds
  // The wake for an engine that never got a kqueue: the user event is
  // the wake everywhere else.
  poll_waiter waiter_;
  unsigned queue_depth_ = 1;
  bool async_requested_ = true;
  bool use_aio_ = false;
};

using aio_context = completion_context<aio_engine>;

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(io_context<aio_context>);

// The longest read name aio_engine::name_for() gives, counted generously:
// the longest rung, why AIO was refused, and the refused direct
// descriptor, as though all three could meet.
inline constexpr std::size_t aio_read_name_max =
    longest_of({engine_token(io_engine::aio, false),
                engine_token(io_engine::posix_sync, false)}) +
    direct_tag.size() + no_aio_suffix_max + direct_einval_suffix.size();
static_assert(aio_read_name_max <= max_read_backend_length,
              "a read backend name would not fit driver_stats::read_backend");

}  // namespace blake3pp::detail::io_impl

#endif  // __FreeBSD__
