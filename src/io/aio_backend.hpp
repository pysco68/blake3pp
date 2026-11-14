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

#include "io/backend.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

// Names the reason AIO is absent, for the fallback backend name the tools
// print: a kernel without the facility (the sysctl tree is not there) and
// a per-process queue closed by the administrator are otherwise
// indistinguishable in bench output.
inline std::string no_aio_suffix(int err) {
  switch (err) {
    case EAGAIN:
      // vfs.aio.max_aio_queue_per_proc is zero: the facility is there and
      // will take nothing, which is what a refused lio_listio reports.
      return " [aio: EAGAIN]";
    case ENOSYS:
      return " [aio: ENOSYS, no kernel AIO]";
    default:
      return " [aio: errno " + std::to_string(err) + "]";
  }
}

// The same for the completion queue itself: without a kqueue there is
// nowhere for a completion to arrive, whatever the AIO limits say.
inline std::string no_kqueue_suffix(int err) {
  return " [kqueue: errno " + std::to_string(err) + "]";
}

// What the kernel will let this process keep queued at once, or zero when
// the facility is absent. Read once per context: the batch below is cut
// to what is still free, so the per-process limit never turns a flush
// into a refusal the backend has to unpick.
[[nodiscard]] inline int aio_queue_limit(int& why) noexcept {
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
  return limit;
}

// How many aiocbs one lio_listio may carry. AIO_LISTIO_MAX is the
// standard's name for it and sysconf is the one that answers at runtime.
[[nodiscard]] inline std::size_t aio_listio_max() noexcept {
  const long n = ::sysconf(_SC_AIO_LISTIO_MAX);
  return n > 0 ? static_cast<std::size_t>(n) : 16;
}

// The FreeBSD reader/writer context: one kqueue, any number of files,
// transfers named by the caller's operation, completions delivered as
// callbacks out of poll(). Degrades per feature at runtime, as the whole
// layer does: O_DIRECT refused by the filesystem -> buffered, AIO refused
// -> every transfer served by the deferred pump's own threads.
class aio_context {
 public:
  class file;  // defined below; a transfer names the file it belongs to
  struct request;  // the context-owned aiocb a transfer rides on

  struct transfer : posix_transfer {
    bool issued = false;          // the kernel has it; an event is owed
    file* owner_file = nullptr;   // whose transfer this is, for forget()
    // The kernel's half of this transfer, borrowed from the context's
    // pool while the transfer is queued or in flight and given back when
    // it finishes; null for one on the synchronous rung.
    request* req = nullptr;
    // One link for several queues: a transfer waits to be issued, or
    // waits for a lane, or has been served by one, and is never on two
    // at once. The pending queue is a deferred_ops over the same field,
    // so every operation carries one pointer for all of them.
    transfer* next_deferred = nullptr;
  };

  // One kernel request, owned by the context. The kernel reads the aiocb
  // when the request is queued, writes the status into it when the
  // request finishes, and aio_return() writes into it again when the
  // result is taken. The drain reaps every request through the pool
  // without reading a transfer, and `op` says whether a transfer is still
  // owed a report; a drain sets it to null.
  //
  // Keeping the aiocb out of the transfer also keeps the caller's
  // operation the size of the other backends'. A struct aiocb is 160 bytes
  // on amd64, a sigevent included, and io_op::storage would have to grow on
  // every platform to hold one.
  struct request {
    struct aiocb cb {};
    transfer* op = nullptr;      // null once a drain has forgotten it
    request* next_free = nullptr;
  };

  // An open file bound to a context. Every file must be destroyed before
  // the context it was opened on, which keeps these descriptors open
  // while the context's drain waits out the transfers on them.
  class file {
   public:
    file(aio_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : write_(how.write) {
      const int base = write_ ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
      f_.open(path.c_str(), base | O_CLOEXEC, 0644);
      size_ = write_ ? 0 : f_.stat_size();
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
      if (write_ && how.preallocate_bytes > 0) {
        [[maybe_unused]] const int refused =
            ::posix_fallocate(f_.fd_plain(), 0,
                              static_cast<off_t>(how.preallocate_bytes));
      }
      bool direct_refused = false;
      if (how.direct_io) {
        // Not TRUNC on the reopen: already truncated, and the tail keeps
        // a plain fd.
        f_.try_odirect(path.c_str(),
                       (write_ ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
        if (f_.direct() && !write_ && !direct_serves(f_)) {
          f_.drop_direct();
          direct_refused = true;
        }
      }
      name_ = ctx.name_for(f_.direct(), write_, direct_refused);
      ctx_ = &ctx;
      next_open_ = ctx.files_;
      ctx.files_ = this;
    }

    // Takes this file's transfers with it, and only those: forget() waits
    // out what the kernel or a lane holds of them, because it may be
    // writing into the buffers they name, and a sibling's completion that
    // surfaces meanwhile is reported, not dropped. Its operations must
    // still be alive (the contract's operation lifetime rule). The file
    // leaves the context's list only afterwards: forget() waits until this
    // file's count is zero, and the list is what lets a context-wide drain
    // zero it.
    ~file() {
      ctx_->forget(*this);
      ctx_->unlink(*this);
    }

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (::ftruncate(f_.fd_plain(), static_cast<off_t>(bytes)) != 0) {
        throw_errno("ftruncate");
      }
    }

   private:
    friend class aio_context;

    // Whether the direct descriptor will serve the file at all: one
    // page-sized read at offset zero, the smallest request the alignment
    // rules below accept. EINVAL is a filesystem refusing the descriptor
    // (OpenZFS `direct=standard` on a dataset whose geometry this request
    // does not suit). Every other answer, a short one or an end-of-file
    // zero included, means it serves. Readers only: a writable open has
    // just truncated the file to nothing, and every window it sends down
    // the direct descriptor is a whole granule at a granule offset.
    [[nodiscard]] static bool direct_serves(const posix_file& f) noexcept {
      alignas(direct_align) std::byte page[direct_align];
      const ssize_t n = ::pread(f.fd(), page, sizeof page, 0);
      return n >= 0 || errno != EINVAL;
    }

    posix_file f_;
    std::uint64_t size_ = 0;
    std::string name_;
    // This file's share of the context's count, which is what forget()
    // waits out. The context-wide count stays the sum of these.
    std::size_t inflight_ = 0;
    aio_context* ctx_ = nullptr;
    file* next_open_ = nullptr;  // the context's list, for its own drain
    bool write_ = false;
  };

  aio_context(const reader_context_options& opts, unsigned max_inflight)
      : queue_depth_(std::max(1u, max_inflight)),
        async_requested_(opts.async),
        // Declared last, and initialized last: see the member list.
        pump_(io_lanes::waker<aio_context>(), this,
              &perform_deferred, io_lanes_for(max_inflight)) {
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
        aio_listio_max(), static_cast<std::size_t>(queue_limit_)));
    // What one wait can hand back: a completion for every slot, its
    // continuation, and the wake.
    events_.resize(std::size_t{2} * max_inflight + 2);
  }

  aio_context(const aio_context&) = delete;
  aio_context& operator=(const aio_context&) = delete;

  // Runs before any member is destroyed, so the kqueue the drain waits on
  // and the requests it reaps are both still there while it waits.
  ~aio_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    arm(op, f, off);
    op.buf = buf;
    op.wbuf = {};
    op.is_write = false;
    issue(op, buf.data(), buf.size());
  }

  // Same rules as submit_read, and the same choice of descriptor: the
  // granules O_DIRECT can take go through the direct fd, the rest
  // through the plain one, both as AIO. Nothing above the context sees
  // the difference.
  void submit_write(file& f, std::uint64_t off,
                    std::span<const std::byte> buf, transfer& op) noexcept {
    arm(op, f, off);
    op.buf = {};
    op.wbuf = buf;
    op.is_write = true;
    issue(op, buf.data(), buf.size());
  }

  // Hands the kernel everything queued since it last looked, in batches
  // of AIO_LISTIO_MAX cut to what the per-process queue still holds.
  //
  // A refused submit is not an error the caller can act on: the
  // transfers are already counted in in_flight(), and an exception here
  // would leave them counted and uncompleted. A transfer the kernel will
  // not take for a lasting reason goes to the pump's lanes; one refused
  // for now stays pending for the next flush.
  void flush() noexcept {
    if (!use_aio_) {
      return;
    }
    // A cancel asked for before the request went out is the whole cancel:
    // the kernel never sees it, and the pump reports it.
    sweep_cancelled();
    while (!pending_.empty()) {
      const std::size_t n = fill_batch();
      if (n == 0) {
        break;  // the per-process queue is full; the next flush retries
      }
      if (!offer(n)) {
        break;
      }
    }
    // Something has to be able to finish, or a blocking poll() would wait
    // for an event that cannot arrive. With nothing in the kernel and
    // nothing on the pump, the oldest transfer the kernel refused goes to
    // the pump's lanes so the run makes progress.
    if (issued_ == 0 && pump_.queue_empty() && !pending_.empty()) {
      transfer* const op = pending_.take();
      release(*op);
      pump_.push(*op);
    }
  }

  // The kqueue first, then the pump's finished list: a completion the
  // kqueue hands back can move its remainder to the pump.
  std::size_t poll(bool block) {
    flush();
    std::size_t ran = reap(false);
    ran += serve_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (take_wake()) {
        return ran + serve_deferred();
      }
      ran += reap(true);
      ran += serve_deferred();
      if (ran > 0 || take_wake()) {
        return ran;
      }
    }
  }

  // The one member another thread may call, and how a lane reports a
  // finished transfer. A user event on the kqueue is thread-safe, and a
  // trigger nobody is waiting for stays pending until a kevent() takes
  // it, which is the contract's "a wake with no blocked poll is
  // remembered". A context without a kqueue wakes through its waiter.
  void wake() noexcept {
    if (!use_aio_) {
      waiter_.wake();
      return;
    }
    struct kevent ev;
    EV_SET(&ev, wake_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    (void)::kevent(kq_.get(), &ev, 1, nullptr, 0, nullptr);
  }

  // The kernel is asked once; a second ask would only repeat the
  // question. A transfer the pump already has is cancelled through the
  // pump, under its lock, because the flag is what a lane reads when it
  // takes one; one a lane already has runs to completion. A transfer
  // still on the pending queue is flagged here and sweep_cancelled()
  // hands it to the pump.
  void cancel(transfer& op) noexcept {
    if (!op.issued && op.req == nullptr) {
      pump_.cancel(op);
      return;
    }
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    if (op.issued && op.req != nullptr) {
      // Advisory: AIO_NOTCANCELED means the request is past cancelling
      // and will report its data, which the contract allows.
      (void)::aio_cancel(op.req->cb.aio_fildes, &op.req->cb);
    }
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Whether the kqueue and the AIO queue were both had, and why they
  // were not: the ladder the files of this context will wear, asked
  // before any of them is opened. The tests probe the machine with this
  // rather than assuming which rung a given host reaches.
  [[nodiscard]] bool using_aio() const noexcept { return use_aio_; }
  [[nodiscard]] std::string_view refusal() const noexcept {
    return refusal_;
  }

  // This context's rung and the one it degrades to; see
  // uring_context::engaged_rung().
  static constexpr io_engine rung_engine = io_engine::aio;
  using rung_below = posix_sync_rung;

  // A refused AIO is never settled here, unlike a refused io_uring on
  // Linux. Every reason the constructor can record is something an
  // administrator or another process can undo while this one runs: the
  // queue limit is the tunable
  // vfs.aio.max_aio_queue_per_proc, the facility itself is a kernel
  // module that can be loaded (the sysctl's absence is how that reads
  // from here), and a kqueue that could not be opened is a descriptor
  // this process may get back. So the verdict is re-probed each time it
  // is asked for, and a machine that turns AIO on stops being reported
  // as a pread machine.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return use_aio_ ? rung_verdict{rung_engine, true}
                    : rung_verdict{rung_below::rung_engine, false};
  }

  // Every transfer the kernel has not been offered is taken back and
  // given to the pump, which is what a flush the kernel refuses outright
  // leaves behind. A counted transfer must reach its callback, so this is
  // the recovery: a lane performs each one and poll() reports the result.
  //
  // Called by the test that covers that path, since a working kernel
  // cannot be asked to refuse.
  void defer_pending() noexcept {
    while (transfer* const op = pending_.take()) {
      release(*op);
      pump_.push(*op);
    }
  }

  // Transfers, or remainders of transfers, performed by the lanes (or by
  // poll() where no lane runs) since construction: zero for a context
  // whose queue took everything.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }

  // The kernel queues as deep as the caller asked; without AIO the pump's
  // lanes are the whole of what the device sees.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return use_aio_ ? queue_depth_ : pump_.depth();
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

  // Requests the kernel never took are withdrawn, the ones it did take
  // are waited out and discarded, and the pump is cleared and its lanes
  // waited out. Cancelling first shortens the wait; a request that cannot
  // be cancelled is still waited for, because it is writing into a buffer
  // the caller is about to release.
  //
  // The queues are dropped without being walked and the request pool is
  // handed back wholesale, since nothing is in flight by then. The
  // operations must still be alive while the lanes are waited out.
  void drain() noexcept {
    if (use_aio_ && issued_ > 0) {
      for (file* f = files_; f != nullptr; f = f->next_open_) {
        (void)::aio_cancel(f->f_.fd(), nullptr);
        (void)::aio_cancel(f->f_.fd_plain(), nullptr);
      }
      while (issued_ > 0) {
        reap_quietly();
      }
    }
    pending_.clear();
    pump_.clear();
    free_all_requests();
    in_flight_ = 0;
    issued_ = 0;
    for (file* f = files_; f != nullptr; f = f->next_open_) {
      f->inflight_ = 0;
    }
  }

  // Gives up every transfer this file owns, running no callback, so the
  // file and the buffers its transfers name may be destroyed while the
  // context stays usable: afterwards in_flight() counts none of them.
  //
  // Legal only while the operations those transfers live in are alive;
  // ~file() calls it. What never reached the kernel is
  // dropped; what the kernel or a lane holds is waited out, because it is
  // writing into those buffers.
  //
  // The kqueue is shared, so a completion belonging to another file can
  // surface while this one is waited out. It is reported, since dropping
  // it would strand an operation that can never complete, so reclaiming
  // one file while a sibling has transfers in flight can run that
  // sibling's callback from here.
  void forget(file& f) noexcept {
    // The file stays on files_: it is still open and may be read again, and
    // a later context-wide drain has to be able to zero its count.
    drop_queued(f);
    drop_pumped(f);
    if (!use_aio_ || f.inflight_ == 0) {
      return;
    }
    (void)::aio_cancel(f.f_.fd(), nullptr);
    (void)::aio_cancel(f.f_.fd_plain(), nullptr);
    while (f.inflight_ != 0) {
      const int got = wait_events(true);
      if (got < 0) {
        abandoned(f.inflight_);
      }
      for (std::size_t i = 0; i < static_cast<std::size_t>(got); ++i) {
        request* const req = event_request(events_[i]);
        if (req == nullptr) {
          continue;
        }
        const reaped r = take_result(*req);
        transfer* const op = req->op;
        if (op == nullptr) {
          release_request(req);
          continue;
        }
        op->issued = false;
        if (op->owner_file == &f) {
          release(*op);  // reaped, not reported
          retire(*op);
          continue;
        }
        if (settle(*op, r)) {
          flush();
        }
      }
      // A continuation of this file's own transfer is dropped the same
      // way its first piece was: it never left for the kernel.
      drop_queued(f);
      drop_pumped(f);
    }
  }

  void unlink(file& f) noexcept {
    for (file** at = &files_; *at != nullptr; at = &(*at)->next_open_) {
      if (*at == &f) {
        *at = f.next_open_;
        f.next_open_ = nullptr;
        return;
      }
    }
  }

 private:
  // The user event's identifier. The kqueue is this context's alone, so
  // any value will do and no request can wear it: an EVFILT_AIO event
  // carries the request's own address instead.
  static constexpr std::uintptr_t wake_ident = 1;

  // Which rung of the ladder this context reached, for a file that did or
  // did not get O_DIRECT. file_reader::backend() reports it verbatim and
  // tools print it, so the spellings, their order and the suffixes are
  // observable API.
  [[nodiscard]] std::string name_for(bool direct, bool write,
                                     bool direct_refused) const {
    const char* const sync_name = write ? "pwrite" : "pread";
    std::string n = use_aio_ ? (direct ? "aio+direct" : "aio")
                             : (direct ? std::string(sync_name) + "+direct"
                                       : std::string(sync_name));
    if (async_requested_ && !use_aio_) {
      n += refusal_;
    }
    if (direct_refused) {
      n += " [direct: EINVAL]";
    }
    return n;
  }

  // What a transfer needs that does not depend on its direction.
  void arm(transfer& op, file& f, std::uint64_t off) noexcept {
    op.arm(f.f_, off);
    op.owner_file = &f;
    op.req = nullptr;
    op.next_deferred = nullptr;
    op.issued = false;
    in_flight_++;
    f.inflight_++;
  }

  // How much of a piece the kernel takes, and through which descriptor:
  // direct_piece()'s shapes. What goes to the pump's lanes is a context
  // with no AIO, a submit the kernel refused, and a zero-length transfer,
  // which has nothing for the kernel to do.
  [[nodiscard]] fd_piece piece_for(const transfer& op, std::uint64_t off,
                                   std::size_t len) const noexcept {
    if (!use_aio_ || len == 0) {
      return {-1, 0};
    }
    return direct_piece(*op.sync_file, off, len);
  }

  // A request to put the next piece on, taken from the pool or made.
  // Null means the pool could not grow, and the transfer goes to the pump
  // instead of failing: issue() is noexcept because a short completion's
  // continuation runs inside poll().
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

  // Gives back whatever request a transfer is holding. Only for a
  // transfer that is alive and not in the kernel's hands.
  void release(transfer& op) noexcept {
    op.device_end();  // whatever it was, the kernel is not serving it now
    if (op.req != nullptr) {
      release_request(op.req);
      op.req = nullptr;
    }
  }

  // Every request back on the free list at once, for the drain: nothing
  // is in flight by then, and no transfer is read to find out which ones
  // were held.
  void free_all_requests() noexcept {
    free_ = nullptr;
    for (request& r : requests_) {
      r.op = nullptr;
      r.next_free = free_;
      free_ = &r;
    }
  }

  // Fills the request for the next piece and queues it, or hands the
  // piece to the pump when the kernel cannot take its shape.
  // `data` is where the piece starts, which for a continuation is past
  // what already moved; a continuation keeps the request its first piece
  // rode on, which is free to refill once aio_return has been called.
  void issue(transfer& op, const void* data, std::size_t len) noexcept {
    const fd_piece piece = piece_for(op, op.off + op.filled, len);
    if (piece.fd < 0) {
      release(op);
      pump_.push(op);
      return;
    }
    request* const req = op.req != nullptr ? op.req : acquire_request();
    if (req == nullptr) {
      pump_.push(op);
      return;
    }
    op.req = req;
    req->op = &op;
    std::memset(&req->cb, 0, sizeof req->cb);
    req->cb.aio_fildes = piece.fd;
    req->cb.aio_offset = static_cast<off_t>(op.off + op.filled);
    // aio_buf is `volatile void *`, so a write's source loses its const
    // on the way in; the kernel only reads it, which is what LIO_WRITE
    // means.
    req->cb.aio_buf = const_cast<void*>(data);
    req->cb.aio_nbytes = piece.len;
    req->cb.aio_lio_opcode = op.is_write ? LIO_WRITE : LIO_READ;
    req->cb.aio_sigevent.sigev_notify = SIGEV_KEVENT;
    req->cb.aio_sigevent.sigev_notify_kqueue = kq_.get();
    // One event per request: the request is reaped by the event, and the
    // same aiocb is refilled for the continuation afterwards.
    req->cb.aio_sigevent.sigev_notify_kevent_flags = EV_ONESHOT;
    req->cb.aio_sigevent.sigev_value.sival_ptr = req;
    pending_.push(op);
  }

  // Moves every pending transfer a cancel reached to the pump, where it
  // reports operation_canceled without ever having been issued.
  void sweep_cancelled() noexcept {
    pending_.remove_if([this](transfer& op) noexcept {
      if (!op.cancelled) {
        return false;
      }
      release(op);
      pump_.push(op);
      return true;
    });
  }

  // The next batch of requests, oldest first: as many as one lio_listio
  // may carry and as many as the per-process queue still has room for.
  // The transfers stay on the pending list until the kernel's answer says
  // which of them it took.
  [[nodiscard]] std::size_t fill_batch() noexcept {
    const std::size_t room =
        static_cast<std::size_t>(queue_limit_) -
        std::min<std::size_t>(issued_, static_cast<std::size_t>(queue_limit_));
    const std::size_t cap = std::min(batch_.size(), room);
    std::size_t n = 0;
    for (transfer* op = pending_.front(); op != nullptr && n < cap;
         op = op->next_deferred) {
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
  [[nodiscard]] bool offer(std::size_t n) noexcept {
    const bool all =
        ::lio_listio(LIO_NOWAIT, batch_.data(), static_cast<int>(n),
                     nullptr) == 0;
    const int why = all ? 0 : errno;
    if (!all && why != EIO && why != EAGAIN) {
      return false;  // nothing was queued; the whole batch stays pending
    }
    bool took_any = false;
    std::size_t seen = 0;
    pending_.remove_if([&](transfer& op) noexcept {
      if (seen++ >= n) {
        return false;  // past the batch that was offered
      }
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
      // vfs.aio.enable_unsafe off). The pump's lanes take it, and
      // served_synchronously() counts it.
      release(op);
      pump_.push(op);
      return true;
    });
    return took_any;
  }

  // The kqueue's answer, or -1 for a wait that failed. Retries an
  // interrupted wait; any other failure is a queue this process has
  // broken, and each caller decides what that means, because the three
  // of them owe their callers different things.
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

  // The last resort shared by the drain and by forget(): transfers in
  // the kernel, a queue that cannot be waited on, and buffers about to be
  // reused. Nothing can make the kernel release them, and continuing
  // would let a read land in memory the caller has given back.
  [[noreturn]] void abandoned(std::size_t owed) const noexcept {
    std::fprintf(stderr,
                 "blake3pp: kevent failed while %zu transfer(s) were still "
                 "owed (%s); their buffers cannot be released\n",
                 owed, std::strerror(errno));
    std::abort();
  }

  // The request an event names, or null for the wake (latched here, so a
  // wake delivered to a non-blocking poll is not lost: the next blocking
  // one returns on it).
  [[nodiscard]] request* event_request(const struct kevent& ev) noexcept {
    if (ev.filter == EVFILT_USER) {
      woken_ = true;
      return nullptr;
    }
    return static_cast<request*>(ev.udata);
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
    // every caller below reads one error number.
    int err = ::aio_error(&r.cb);
    if (err < 0) {
      err = errno;
    }
    const ssize_t res = ::aio_return(&r.cb);
    issued_ -= std::min<std::size_t>(1, issued_);
    return {err, res};
  }

  void complete(transfer& op, std::error_code ec) noexcept {
    device_.add_if_served(op.device_total_ns());
    // Before the callback, which may submit this very operation again.
    release(op);
    retire(op);
    op.done(&op, ec);
  }

  // Takes a transfer off the books without reporting it: what a forget
  // does, and the first half of what a completion does.
  void retire(transfer& op) noexcept {
    in_flight_ -= std::min<std::size_t>(1, in_flight_);
    assert(op.owner_file != nullptr &&
           "every transfer is armed with its file");
    op.owner_file->inflight_ -=
        std::min<std::size_t>(1, op.owner_file->inflight_);
  }

  // What one completion of `op` means. A transfer short of its length
  // continues from where it stopped, reads and writes alike; only the
  // span it walks differs. Returns whether it was requeued rather than
  // completed, which is the caller's cue to flush.
  [[nodiscard]] bool settle(transfer& op, reaped r) noexcept {
    // This piece is back from the kernel, whatever it reports.
    op.device_end();
    if (r.err == ECANCELED) {
      complete(op, std::make_error_code(std::errc::operation_canceled));
      return false;
    }
    if (r.err != 0 || r.res < 0) {
      complete(op, std::error_code(r.err != 0 ? r.err : EIO,
                                   std::generic_category()));
      return false;
    }
    if (r.res == 0) {
      // Short of the length with nothing left to give: the file ended
      // where the engine was told it would not.
      complete(op, std::error_code(EIO, std::generic_category()));
      return false;
    }
    if (!op.is_write) {
      msan_unpoison(op.buf.data() + op.filled,
                    static_cast<std::size_t>(r.res));
    }
    op.filled += static_cast<std::size_t>(r.res);
    if (op.filled < op.length()) {
      const std::byte* const from =
          op.is_write ? op.wbuf.data() + op.filled : op.buf.data() + op.filled;
      issue(op, from, op.length() - op.filled);
      // issue() either queued the remainder or handed it to the pump;
      // either way the transfer is still owed and nothing was reported.
      return true;
    }
    complete(op, {});
    return false;
  }

  // Consumes whatever the kqueue has, reissuing short transfers. Returns
  // the number of callbacks run.
  std::size_t reap(bool block) {
    if (!use_aio_) {
      if (block) {
        // No queue to wait on: the only event that can end this sleep is
        // a wake, and that is what the waiter is for.
        while (!waiter_.take()) {
          waiter_.sleep();
        }
        woken_ = true;
      }
      return 0;
    }
    const int got = wait_events(block);
    if (got < 0) {
      // The mechanism failed while waiting, which is the one thing
      // poll() is allowed to throw for.
      throw_errno("kevent(wait)");
    }
    std::size_t ran = 0;
    bool requeued = false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(got); ++i) {
      request* const req = event_request(events_[i]);
      if (req == nullptr) {
        continue;
      }
      const reaped r = take_result(*req);
      transfer* const op = req->op;
      if (op == nullptr) {
        release_request(req);  // a drain forgot it; nobody is owed
        continue;
      }
      op->issued = false;
      if (settle(*op, r)) {
        requeued = true;
      } else {
        ran++;
      }
    }
    if (requeued) {
      // The continuation of a short transfer is the same kind of request
      // as the transfer itself, and the same refusal leaves it owed.
      flush();
    }
    return ran;
  }

  // One wait's worth of completions, reaped and dropped: the drain's
  // step. Nothing is continued, nothing is reported, and no transfer is
  // read: the requests carry everything the reap needs.
  void reap_quietly() noexcept {
    const int got = wait_events(true);
    if (got < 0) {
      abandoned(issued_);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(got); ++i) {
      request* const req = event_request(events_[i]);
      if (req == nullptr) {
        continue;
      }
      (void)take_result(*req);
      release_request(req);
    }
  }

  // One transfer, on a lane: perform_rest() touches the operation and its
  // file and nothing else of this context, so served_synchronously_ is
  // counted where the completion is run.
  static std::error_code perform_deferred(transfer& op) noexcept {
    return perform_rest(op);
  }

  // Runs the callbacks of the transfers the lanes finished, on this
  // thread. The inline step serves a machine that would not give the
  // pump a thread.
  std::size_t serve_deferred() noexcept {
    return pump_.report_finished(
        [this](transfer& op) noexcept { report_deferred(op); });
  }

  void report_deferred(transfer& op) noexcept {
    const std::error_code ec = op.result();
    if (!ec) {
      served_synchronously_++;
    }
    complete(op, ec);
  }

  // Drops one file's share of pending_, with no wait and no callback:
  // nothing on it was ever given to the kernel, so the request each entry
  // holds goes straight back to the pool.
  void drop_queued(file& f) noexcept {
    const std::size_t dropped = pending_.remove_if([&](transfer& op) noexcept {
      if (op.owner_file != &f) {
        return false;
      }
      release(op);
      return true;
    });
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
  }

  // The pump's share of the same job. What is still queued was never
  // performed and is dropped; what a lane holds is waited out, because it
  // is reading or writing through this file's descriptor and into the
  // caller's buffer. The lanes are the context's, so the wait covers
  // sibling files too, and a sibling's finished transfer stays on the
  // done list for a later poll() to report.
  void drop_pumped(file& f) noexcept {
    const auto mine = [&](transfer& op) noexcept {
      if (op.owner_file != &f) {
        return false;
      }
      release(op);
      return true;
    };
    std::size_t dropped = pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
  }

  // True means a wake() had been issued since the last time one was
  // taken.
  [[nodiscard]] bool take_wake() noexcept {
    return std::exchange(woken_, false);
  }

  // Declaration order is the teardown contract: the kqueue and the
  // request pool outlive the queues whose transfers borrow from them, and
  // the destructor's drain runs before any of them goes.
  unique_fd kq_;
  // Every aiocb this context has ever needed, and the ones free to reuse.
  // Never shrunk: an address the kernel holds has to stay put, and the
  // high-water mark of a run is what the next window costs anyway.
  std::deque<request> requests_;
  request* free_ = nullptr;
  deferred_ops<transfer> pending_;  // queued, not yet offered to the kernel
  std::vector<struct aiocb*> batch_;
  std::vector<struct kevent> events_;
  // Every open file of this context, so its own drain can zero the shares
  // they count. Linked and unlinked by the file: forget() leaves one on
  // the list, because an idled file is still open and may be read again.
  file* files_ = nullptr;
  std::string refusal_;  // why AIO was not had, for the file names
  std::size_t in_flight_ = 0;
  std::size_t issued_ = 0;
  std::size_t served_synchronously_ = 0;
  int queue_limit_ = 0;
  // What fill_batch() leaves in _aiocb_private.error: no errno, and not
  // EINPROGRESS or 0, so offer() reads an entry the kernel never reached
  // as not taken.
  static constexpr long not_looked_at = -1;
  // The wake for a context that never got a kqueue: the user event is the
  // wake everywhere else.
  poll_waiter waiter_;
  // What the device itself took, stamped by the kernel's queue where it
  // carried a piece and by a lane where one did.
  device_time device_{};
  // What the caller asked to have in flight at once; device_depth()
  // reports it where the kernel took the transfers.
  unsigned queue_depth_ = 1;
  bool async_requested_ = true;
  bool use_aio_ = false;
  bool woken_ = false;
  // The lanes that serve what the kernel will not take. Declared last, so
  // the destructor's join runs while the kqueue and the waiter its wake()
  // reaches are still alive. Do not move this up.
  deferred_pump<transfer> pump_;
};

// Definition-site conformance check. Concepts only verify use-sites, so
// without this a drifting backend wouldn't be diagnosed until an engine
// instantiation in some other TU; this makes the header self-checking.
static_assert(io_context<aio_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // __FreeBSD__
