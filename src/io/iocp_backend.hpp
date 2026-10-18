#pragma once

// The Windows backend: CreateFileW handles (std::filesystem::path's native
// wide string is the whole reason the public API trades in paths) and an
// I/O completion port as the completion queue. The mapping to the io_uring
// backend is
// nearly 1:1: one OVERLAPPED per buffer slot plays the SQE,
// GetQueuedCompletionStatus plays the completion-queue wait, and
// FILE_FLAG_NO_BUFFERING is
// O_DIRECT (same sector-alignment demands, same buffered-handle escape
// hatch for the unaligned tail). Degrades per-feature at RUNTIME: no port
// -> sync ReadFile/WriteFile, NO_BUFFERING refused -> buffered. Internal
// to src/io/, never installed.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <string_view>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "io/backend.hpp"

namespace blake3pp::detail::io_impl {

[[noreturn]] inline void throw_winerr(const char* what) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), what);
}

// A read the port reports as aborted is one CancelIoEx reached. The
// contract names that outcome operation_canceled on every platform, so a
// caller can recognise it without asking which OS it is on.
[[nodiscard]] inline std::error_code read_error(DWORD err) noexcept {
  return err == ERROR_OPERATION_ABORTED
             ? std::make_error_code(std::errc::operation_canceled)
             : std::error_code(static_cast<int>(err), std::system_category());
}

// Owns one Win32 HANDLE and closes it exactly once. Win32 spells "no
// handle" two ways (CreateFileW yields INVALID_HANDLE_VALUE, the IOCP
// calls yield nullptr), so both count as empty here and neither is ever
// handed to CloseHandle. Move-only, so a handle can never be owned twice.
class unique_handle {
 public:
  unique_handle() = default;
  explicit unique_handle(HANDLE h) noexcept : h_(h) {}
  unique_handle(const unique_handle&) = delete;
  unique_handle& operator=(const unique_handle&) = delete;
  unique_handle(unique_handle&& other) noexcept
      : h_(std::exchange(other.h_, INVALID_HANDLE_VALUE)) {}
  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.h_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }
  ~unique_handle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return h_; }
  explicit operator bool() const noexcept {
    return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
  }
  void reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      ::CloseHandle(h_);
    }
    h_ = h;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

// Shared Windows file plumbing, the twin of posix_file: the buffered
// handle that always exists, the optional NO_BUFFERING/OVERLAPPED reopen
// next to it (both are per-open flags, hence a second handle), and the
// completion port when async engages. Every handle lives in a
// unique_handle, which makes the type non-copyable by construction and
// unwinds the whole set when a constructor throws part-way through.
//
// `fast` is held ONLY when the reopen genuinely engaged, so it never
// aliases `plain` and no destructor has to test for that.
// Shared verbatim between reader and writer; only access/creation differ.
struct win_file {
  unique_handle plain;  // always-buffered+sync: unaligned tails, fallback
  unique_handle fast;   // the reopened handle, when one engaged
  unique_handle port;   // IOCP, when async engaged
  bool direct = false;
  bool use_iocp = false;

  // The handle the fast path should use: the reopened one when it
  // engaged, else the plain one. Borrowed: the caller never closes it.
  [[nodiscard]] HANDLE h() const noexcept {
    return fast ? fast.get() : plain.get();
  }

  void open(const wchar_t* path, DWORD access, DWORD share, DWORD creation,
            DWORD flags) {
    plain.reset(::CreateFileW(path, access, share, nullptr, creation, flags,
                              nullptr));
    if (!plain) {
      throw_winerr("CreateFileW");
    }
  }

  [[nodiscard]] std::uint64_t stat_size() const {
    LARGE_INTEGER sz;
    if (::GetFileSizeEx(plain.get(), &sz) == 0) {
      throw_winerr("GetFileSizeEx");
    }
    return static_cast<std::uint64_t>(sz.QuadPart);
  }

  // NO_BUFFERING and OVERLAPPED are per-open flags: engage by reopening,
  // keeping the plain handle for unaligned lengths. Best-effort: when
  // nothing can be engaged the plain handle stays in charge with direct
  // and use_iocp still false.
  //
  // Per feature. The two flags are asked for together first, and a
  // refusal drops one and asks again: NO_BUFFERING is a property of the
  // filesystem (SMB and some virtual volumes reject it) while OVERLAPPED is
  // not, and on Windows buffered async measures within noise of direct
  // async (docs/performance.md), so the port is the half to keep.
  //
  // shared_port, when given, is a port this file does not own: the reader
  // context keeps one port for every file it opens, while the writer still
  // creates its own here. Ownership is the only difference; association
  // and the failure handling are the same either way.
  void engage(const wchar_t* path, DWORD access, DWORD share,
              bool want_direct, bool want_async,
              HANDLE shared_port = nullptr, bool writing = false) noexcept {
    // OPEN_EXISTING on the reopen in both directions: the plain handle has
    // already created and truncated a file being written, and a second
    // CREATE_ALWAYS here would truncate it again -- including any
    // preallocation just set on it.
    (void)writing;
    if (want_direct && want_async) {
      if (engage_one(path, access, share, true, true, shared_port)) {
        return;
      }
      // The port first: per the measurement above, Windows can do
      // without the cache bypass more easily than without the asynchrony.
      if (engage_one(path, access, share, false, true, shared_port)) {
        return;
      }
      (void)engage_one(path, access, share, true, false, shared_port);
      return;
    }
    if (want_direct || want_async) {
      (void)engage_one(path, access, share, want_direct, want_async,
                       shared_port);
    }
  }

  // One rung: reopen with exactly these two flags, attach the port when
  // async was asked for, and take the handle only if both steps worked.
  // Leaves everything untouched on failure, so the caller may ask again
  // for less.
  [[nodiscard]] bool engage_one(const wchar_t* path, DWORD access, DWORD share,
                                bool with_direct, bool with_async,
                                HANDLE shared_port) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (with_direct) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (with_async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    unique_handle cand(::CreateFileW(path, access, share, nullptr,
                                     OPEN_EXISTING, flags, nullptr));
    if (!cand) {
      return false;
    }
    unique_handle own_port;
    if (with_async) {
      if (shared_port != nullptr) {
        // An unattachable port takes the fast handle down with it: a
        // FILE_FLAG_OVERLAPPED handle cannot serve the synchronous path,
        // so the candidate closes on the way out and the caller asks for
        // a rung without it.
        if (::CreateIoCompletionPort(cand.get(), shared_port, 0, 0) ==
            nullptr) {
          return false;
        }
      } else {
        unique_handle p(
            ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!p ||
            ::CreateIoCompletionPort(cand.get(), p.get(), 0, 0) == nullptr) {
          return false;
        }
        own_port = std::move(p);
      }
    }
    if (own_port) {
      port = std::move(own_port);
    }
    use_iocp = with_async;
    fast = std::move(cand);
    direct = with_direct;
    return true;
  }
};

// The IOCP reader context: one completion port, any number of files,
// reads named by the caller's transfer, completions delivered as callbacks
// out of poll(). The port is the context's; each file associates its
// overlapped handle with it at construction.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is NOT set. With it, a
// read that completes synchronously returns without posting, and its
// callback would have to run inside submit_read() -- exactly what the
// contract forbids. Leaving it unset costs a post per fast completion and
// buys one path: every read, fast or slow, reports from poll().
class iocp_context {
 public:
  class file;

  struct transfer : transfer_base {
    OVERLAPPED ov{};
    std::byte* dst = nullptr;         // reads fill this
    const std::byte* src = nullptr;   // writes read from this
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool is_write = false;
    HANDLE h = INVALID_HANDLE_VALUE;
    win_file* sync_file = nullptr;  // the deferred path needs the handles
    file* owner_file = nullptr;     // which file's drain owes this op
    transfer* next_deferred = nullptr;
    bool on_port = false;   // the port owes a completion, so cancel is CancelIoEx
    bool cancelled = false;  // a cancel was asked for; one is enough
  };

  class file {
   public:
    file(iocp_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : write_(how.write) {
      // FILE_SHARE_READ and nothing else: a digest describes the bytes
      // that were there, so nothing may rewrite or shorten the file
      // while it is being read. The cost is that a test cannot truncate
      // a file the pipeline holds open, which is why the truncation
      // case in tests/file_pipeline.cpp does not run here.
      const DWORD access = write_ ? GENERIC_WRITE : GENERIC_READ;
      const DWORD creation = write_ ? CREATE_ALWAYS : OPEN_EXISTING;
      // A writable open shares write access with ITSELF: engage() reopens
      // the same path for the direct/overlapped handle, and two
      // GENERIC_WRITE handles can only coexist if the first one permits
      // it. Without FILE_SHARE_WRITE the reopen fails with a sharing
      // violation every time, and every write silently takes the
      // synchronous buffered rung.
      const DWORD share =
          write_ ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
      f_.open(path.c_str(), access, share, creation,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
      size_ = write_ ? 0 : f_.stat_size();
      // Preallocation belongs to the OPEN, not to the finish: a write that
      // extends the file serializes, and with an async direct queue that
      // collapses it to one stalled write at a time. On Windows that is
      // SetEndOfFile, and set_size() trims back afterwards. The writes
      // still zero-fill up to the valid data length as NTFS requires;
      // SetFileValidData would waive that, but it needs
      // SeManageVolumePrivilege and hands the file whatever those disk
      // blocks held until they are written, so it is not done here.
      if (write_ && how.preallocate_bytes > 0) {
        if (set_end_of_file(how.preallocate_bytes)) {
          prealloc_ = how.preallocate_bytes;
        }
      }
      f_.engage(path.c_str(), access, share, how.direct_io,
                ctx.port_wanted(), ctx.port(), write_);
      name_ = ctx.name_for(f_.direct, f_.use_iocp, write_);
      ctx.adopt(*this);
    }

    ~file() {
      if (ctx_ != nullptr) {
        // Reclaimed, then off the context's list: forget() leaves a file on
        // it, because a file that is merely idled is still open and may be
        // read again. This one is not -- and a destroyed file still on the
        // list is what a later drain() would walk.
        ctx_->forget(*this);
        ctx_->unlink(*this);
      }
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is.
    void set_size(std::uint64_t bytes) {
      if (!set_end_of_file(bytes)) {
        throw_winerr("SetEndOfFile");
      }
      prealloc_ = 0;
    }

   private:
    friend class iocp_context;

    // SetFilePointerEx + SetEndOfFile on the PLAIN handle: the fast one
    // may be FILE_FLAG_NO_BUFFERING, whose file pointer has alignment
    // rules of its own, and the plain handle has none.
    [[nodiscard]] bool set_end_of_file(std::uint64_t bytes) noexcept {
      LARGE_INTEGER to;
      to.QuadPart = static_cast<LONGLONG>(bytes);
      if (::SetFilePointerEx(f_.plain.get(), to, nullptr, FILE_BEGIN) == 0) {
        return false;
      }
      return ::SetEndOfFile(f_.plain.get()) != 0;
    }

    win_file f_;
    std::uint64_t size_ = 0;
    std::uint64_t prealloc_ = 0;
    bool write_ = false;
    std::string name_ = "readfile";
    iocp_context* ctx_ = nullptr;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's
  };

  // The port exists whether or not files engage the async path: it is
  // also the wake primitive, so a context whose reads all go through the
  // deferred list still has exactly one thing a blocked poll sleeps on.
  iocp_context(const reader_context_options& opts, unsigned)
      : async_requested_(opts.async),
        port_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)) {
    if (!port_) {
      throw_winerr("CreateIoCompletionPort");
    }
  }
  iocp_context(const iocp_context&) = delete;
  iocp_context& operator=(const iocp_context&) = delete;

  // Every file is destroyed first, and each one's destructor forgot its
  // own transfers, so the drain finds nothing owed. What can remain is a
  // wake nobody consumed, which the port drops with itself.
  ~iocp_context() { drain(); }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    place(op, f);
  }

  // The write half of the same contract, and the same ladder: a length
  // NO_BUFFERING cannot take, or a file whose reopen never engaged,
  // rides the deferred list and is written synchronously inside poll()
  // through the plain handle. Nothing above the context sees the
  // difference.
  //
  // A zero-length write takes the deferred path too: WriteFile of zero
  // bytes posts a completion carrying zero bytes, which is the same
  // shape as a short transfer and cannot be told apart from one.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    place(op, f);
  }

 private:
  // What a transfer needs that does not depend on its direction.
  void arm(transfer& op, file& f, std::uint64_t off, std::size_t len) noexcept {
    op.len = len;
    op.off = off;
    op.filled = 0;
    op.h = f.f_.h();
    op.sync_file = &f.f_;
    op.next_deferred = nullptr;
    op.owner_file = &f;
    op.on_port = false;
    op.cancelled = false;
  }

  // How much of a piece of a transfer the port takes; zero means none of
  // it. Asked for both directions and for every piece: the initial one,
  // and what is left after a short completion, which resumes at
  // off + filled -- a granule boundary only if the device stopped on one.
  //
  // A buffered overlapped handle takes any shape, so a buffered file's
  // pieces ride whole. NO_BUFFERING rejects an unaligned length and an
  // unaligned offset, so a direct file's piece is cut to the whole
  // granules it holds at a granule offset, and what that leaves -- the
  // sub-granule end of a file, or a resumption off a boundary -- goes
  // through the synchronous path inside poll(), on the plain handle,
  // because that handle is not overlapped and cannot be given to the
  // port. The io_uring backend's piece_for() is the same question with a
  // plain descriptor that can ride.
  //
  // A zero-length transfer takes the synchronous path too: it would post
  // a completion carrying zero bytes, the same shape as a short transfer.
  // And a ReadFile or WriteFile refused outright owes no completion, so
  // nothing would ever take it off in_flight(): it goes down the same
  // ladder, where a second failure reaches the caller as every other
  // transfer error does.
  [[nodiscard]] DWORD port_len(const transfer& op, std::uint64_t off,
                               std::size_t len) const noexcept {
    if (!op.sync_file->use_iocp || len == 0) {
      return 0;
    }
    if (op.sync_file->direct) {
      const std::size_t granules = len - len % direct_align;
      return off % direct_align == 0 ? static_cast<DWORD>(granules) : 0;
    }
    return static_cast<DWORD>(len);
  }

  void place(transfer& op, file& f) {
    const DWORD len = port_len(op, op.off, op.len);
    bool queued = len != 0;
    if (queued) {
      try {
        issue(op, len);
      } catch (const std::system_error&) {
        queued = false;
      }
    }
    op.on_port = queued;
    in_flight_++;
    f.inflight_++;
    if (!queued) {
      deferred_.push(op);
    }
  }

 public:

  // CancelIoEx names one request by its OVERLAPPED, so a sibling read on
  // the same handle is untouched. It fails with ERROR_NOT_FOUND for a
  // request that has already finished, which is not an error here: that
  // completion is already on the port and arrives normally. A read on
  // the deferred list was never issued to anything, so the flag alone is
  // the whole cancel and run_one_deferred() honours it.
  void cancel(transfer& op) noexcept {
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    if (op.on_port) {
      ::CancelIoEx(op.h, &op.ov);
    }
  }

  // ReadFile queues the request itself, so there is nothing batched to
  // push. The contract keeps the call because io_uring has.
  void flush() noexcept {}

  // The port first, then the deferred list, in that order and never
  // the other way round: a completion the port hands back can park its
  // remainder on the list, and the list is what serves it.
  std::size_t poll(bool block) {
    std::size_t ran = reap_ready(false);
    ran += run_one_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (std::exchange(woken_, false)) {
        return ran;
      }
      ran += reap_ready(true);
      ran += run_one_deferred();
      if (ran > 0 || std::exchange(woken_, false)) {
        return ran;
      }
    }
  }

  // Posts a sentinel the port hands back with a null OVERLAPPED, which is
  // how a blocked GetQueuedCompletionStatus is ended from another thread.
  void wake() noexcept {
    ::PostQueuedCompletionStatus(port_.get(), 0, wake_key, nullptr);
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }
  // Transfers, or remainders of transfers, finished on the synchronous
  // path inside poll() since construction: zero for a buffered file on a
  // port, one per direct file whose size is not a multiple of the granule.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }

  // Cancels what every open file still has on the port, reaps all of it
  // without reporting any, and forgets the deferred list. CancelIoEx
  // bounds the wait: every cancelled request completes, successfully or
  // with ERROR_OPERATION_ABORTED.
  //
  // A null OVERLAPPED that is not the wake sentinel means the port itself
  // has failed, so no completion can arrive and the requests the kernel
  // holds can never be given back. The caller frees the buffers they name
  // as soon as this returns -- that is the whole contract of a drain -- so
  // the process ends with the reason instead, the same answer the ring
  // gives in the same position (src/io/uring_backend.hpp).
  void drain() noexcept {
    for (file* f = files_; f != nullptr; f = f->next_) {
      if (f->f_.use_iocp && f->inflight_ > 0) {
        ::CancelIoEx(f->f_.h(), nullptr);
      }
    }
    while (port_owed_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      ::GetQueuedCompletionStatus(port_.get(), &bytes, &key, &pov, INFINITE);
      if (pov == nullptr) {
        if (key == wake_key) {
          woken_ = true;
          continue;
        }
        std::fprintf(stderr,
                     "blake3pp: the completion port failed while %zu "
                     "transfer(s) were still owed (error %lu); their buffers "
                     "cannot be released\n",
                     port_owed_, static_cast<unsigned long>(::GetLastError()));
        std::abort();
      }
      --port_owed_;
    }
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
    deferred_.clear();
    in_flight_ = 0;
  }

  // A file about to close cancels what it still owes and waits it out: the
  // requests target the engine's pool, which is freed just after, and a
  // request still pending would write into it afterwards. Nothing of this
  // file's is reported, which is the contract's teardown drain.
  //
  // One sharp edge, since the port is shared: a completion belonging to
  // ANOTHER file can surface while this one drains, and it is reported
  // normally rather than dropped -- losing it would strand an op that can
  // never complete. So destroying one file of a multi-file context while
  // a sibling has reads in flight can run that sibling's callback from a
  // destructor. The engine never does it (one file per context), and a
  // multi-file driver should idle a file before closing it.
  void forget(file& f) noexcept {
    // The file stays on files_: it is still open and may be read again (see
    // the uring backend's forget).
    // What never reached the port goes first. A deferred transfer names
    // this file's handles and this file's share of the count, and the
    // next poll() would run it through both after they are gone; and
    // the wait below is on the port, which will never hand back a
    // transfer that was never given to it.
    const std::size_t dropped = deferred_.remove_if(
        [&f](const transfer& op) noexcept { return op.owner_file == &f; });
    in_flight_ -= dropped;
    f.inflight_ -= dropped;
    if (!f.f_.use_iocp || f.inflight_ == 0) {
      return;
    }
    ::CancelIoEx(f.f_.h(), nullptr);
    while (f.inflight_ > 0) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok =
          ::GetQueuedCompletionStatus(port_.get(), &bytes, &key, &pov,
                                      INFINITE);
      if (pov == nullptr) {
        if (key == wake_key) {
          woken_ = true;  // remember it; a later poll() still owes it
          continue;
        }
        // The port is unusable, so this file's transfers can never come
        // back. Its handles close next and the buffers they are writing into
        // are released. Nothing here can make the kernel let go, and
        // returning would leave the context counting a file that no longer
        // exists, so the process ends with the reason, as the context-wide
        // drain does.
        std::fprintf(stderr,
                     "blake3pp: the completion port failed while %zu "
                     "transfer(s) of a closing file were still owed (error "
                     "%lu); their buffers cannot be released\n",
                     f.inflight_, static_cast<unsigned long>(::GetLastError()));
        std::abort();
      }
      transfer& op = *op_of(pov);
      --port_owed_;
      if (op.owner_file == &f) {
        in_flight_--;
        f.inflight_--;  // reaped, not reported
        continue;
      }
      if (ok == 0) {
        complete(op, read_error(::GetLastError()));
      } else if (bytes == 0) {
        complete(op, std::error_code(EIO, std::generic_category()));
      } else {
        op.filled += bytes;
        if (op.filled < op.len) {
          (void)reissue(op);
        } else {
          complete(op, {});
        }
      }
    }
  }

 private:
  static constexpr ULONG_PTR wake_key = ~ULONG_PTR{0};

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool use_iocp,
                                     bool write) const {
    const char* const sync_name = write ? "writefile" : "readfile";
    if (use_iocp) {
      return direct ? "iocp+direct" : "iocp";
    }
    return direct ? std::string(sync_name) + "+direct" : std::string(sync_name);
  }

  [[nodiscard]] bool port_wanted() const noexcept { return async_requested_; }
  [[nodiscard]] HANDLE port() const noexcept {
    return port_ ? port_.get() : nullptr;
  }

  void adopt(file& f) noexcept {
    f.ctx_ = this;
    f.next_ = files_;
    files_ = &f;
  }

  void unlink(file& f) noexcept {
    for (file** link = &files_; *link != nullptr; link = &(*link)->next_) {
      if (*link == &f) {
        *link = f.next_;
        return;
      }
    }
  }


  // Queues the rest of a short read. False means ReadFile refused it and
  // the op was completed with that error instead. Both callers are
  // inside poll(), which is where a read's error is allowed to reach its
  // callback; what cannot happen is leaving it counted and unissued.
  [[nodiscard]] bool reissue(transfer& op) noexcept {
    const DWORD len = port_len(op, op.off + op.filled, op.len - op.filled);
    if (len == 0) {
      // The remainder is not something this file may hand the port. It is
      // still owed, so it goes down the same rung an unalignable transfer
      // takes from the start, which finishes what is left inside poll().
      op.on_port = false;
      deferred_.push(op);
      return true;
    }
    try {
      issue(op, len);
      return true;
    } catch (const std::system_error& e) {
      complete(op, e.code());
      return false;
    }
  }

  // Hands the port `len` bytes of what the transfer still owes, from
  // op.filled: all of it, or the granules of it that port_len() allowed.
  void issue(transfer& op, DWORD len) {
    std::memset(&op.ov, 0, sizeof(op.ov));
    const std::uint64_t off = op.off + op.filled;
    op.ov.Offset = static_cast<DWORD>(off);
    op.ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    const BOOL ok =
        op.is_write
            ? ::WriteFile(op.h, op.src + op.filled, len, nullptr, &op.ov)
            : ::ReadFile(op.h, op.dst + op.filled, len, nullptr, &op.ov);
    if (ok == 0 && ::GetLastError() != ERROR_IO_PENDING) {
      throw_winerr(op.is_write ? "WriteFile(async)" : "ReadFile(async)");
    }
    ++port_owed_;
  }

  void complete(transfer& op, std::error_code ec) noexcept {
    in_flight_--;
    if (op.owner_file != nullptr) {
      op.owner_file->inflight_--;
    }
    op.done(&op, ec);
  }

  // The op is the record its OVERLAPPED is a member of. The caller keeps
  // it at a fixed address from submit until its callback has run, which
  // is what makes the walk back valid; it is the documented IOCP idiom
  // and the reason transfer is the caller's type rather than the
  // context's.
  [[nodiscard]] static transfer* op_of(OVERLAPPED* pov) noexcept {
    return CONTAINING_RECORD(pov, transfer, ov);
  }

  // Takes what the port has, running the callbacks of transfers that
  // ended and reissuing the rest of the ones that stopped short. With
  // `block`, waits for the first completion -- but not past a remainder
  // the port would not take: that one is parked on the deferred list,
  // and only poll()'s next step serves it, so waiting on the port with
  // nothing else owed to it would never return.
  std::size_t reap_ready(bool block) {
    std::size_t ran = 0;
    bool parked = false;
    for (;;) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* pov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(
          port_.get(), &bytes, &key, &pov,
          block && ran == 0 && !parked ? INFINITE : 0);
      if (pov == nullptr) {
        if (ok == 0 && ::GetLastError() == WAIT_TIMEOUT) {
          return ran;  // nothing more queued
        }
        if (key == wake_key) {
          woken_ = true;
          return ran;
        }
        throw_winerr("GetQueuedCompletionStatus");
      }
      transfer& op = *op_of(pov);
      --port_owed_;
      if (ok == 0) {
        complete(op, read_error(::GetLastError()));
        ran++;
        continue;
      }
      if (bytes == 0) {
        complete(op, std::error_code(EIO, std::generic_category()));
        ran++;
        continue;
      }
      op.filled += bytes;
      if (op.filled < op.len) {
        if (!reissue(op)) {
          ran++;  // it ended here, with the error ReadFile gave
        } else if (!op.on_port) {
          parked = true;  // the rest is on the deferred list
        }
        continue;
      }
      complete(op, {});
      ran++;
    }
  }

  std::size_t run_one_deferred() noexcept {
    transfer* const next = deferred_.take();
    if (next == nullptr) {
      return 0;
    }
    transfer& op = *next;
    std::error_code ec;
    if (op.cancelled) {
      ec = std::make_error_code(std::errc::operation_canceled);
    } else {
      // From op.filled, not from zero: a short transfer whose remainder the
      // port would not take arrives here part-done, and the bytes already
      // in the buffer must not be fetched again over themselves.
      try {
        const std::size_t rest = op.len - op.filled;
        if (op.is_write) {
          write_sync(*op.sync_file, op.off + op.filled,
                     {op.src + op.filled, rest});
        } else {
          read_sync(*op.sync_file, op.off + op.filled,
                    {op.dst + op.filled, rest});
        }
        op.filled = op.len;
        served_synchronously_++;
      } catch (const std::system_error& e) {
        ec = e.code();
      }
    }
    complete(op, ec);
    return 1;
  }

  // A non-OVERLAPPED handle plus an OVERLAPPED offset blocks until
  // complete. The use_iocp guard makes it structural that the overlapped
  // handle is never used synchronously.
  static void read_sync(win_file& f, std::uint64_t off,
                        std::span<std::byte> buf) {
    const HANDLE use_h =
        f.direct && !f.use_iocp && buf.size() % direct_align == 0
            ? f.h()
            : f.plain.get();
    std::size_t got = 0;
    while (got < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + got;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::ReadFile(use_h, buf.data() + got,
                     static_cast<DWORD>(buf.size() - got), &n, &ov) == 0) {
        throw_winerr("ReadFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += n;
    }
  }

  // The write twin of read_sync, and the same handle choice: the
  // overlapped handle is never used synchronously, and a length
  // NO_BUFFERING cannot take goes through the plain one.
  //
  // A zero-length write returns without calling WriteFile, which would
  // only set the file pointer.
  static void write_sync(win_file& f, std::uint64_t off,
                         std::span<const std::byte> buf) {
    if (buf.empty()) {
      return;
    }
    const HANDLE use_h =
        f.direct && !f.use_iocp && buf.size() % direct_align == 0
            ? f.h()
            : f.plain.get();
    std::size_t put = 0;
    while (put < buf.size()) {
      OVERLAPPED ov{};
      const std::uint64_t o = off + put;
      ov.Offset = static_cast<DWORD>(o);
      ov.OffsetHigh = static_cast<DWORD>(o >> 32);
      DWORD n = 0;
      if (::WriteFile(use_h, buf.data() + put,
                      static_cast<DWORD>(buf.size() - put), &n, &ov) == 0) {
        throw_winerr("WriteFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "write made no progress");
      }
      put += n;
    }
  }

  bool async_requested_ = true;
  unique_handle port_;
  deferred_ops<transfer> deferred_;
  file* files_ = nullptr;      // every open file, for the drain
  std::size_t in_flight_ = 0;
  std::size_t served_synchronously_ = 0;
  std::size_t port_owed_ = 0;  // requests the port has yet to hand back
  bool woken_ = false;
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(io_context<iocp_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
