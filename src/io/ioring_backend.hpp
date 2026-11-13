#pragma once

// The Windows 11 backend: the same CreateFileW handles the completion-port
// backend opens, driven through an I/O ring instead of a port. The mapping
// to the io_uring backend is the closer of the two: BuildIoRingReadFile
// fills a submission entry without entering the kernel, SubmitIoRing is
// io_uring_enter, PopIoRingCompletion drains the completion queue, and
// FILE_FLAG_NO_BUFFERING is O_DIRECT (same sector-alignment demands, same
// buffered handle for the unaligned tail). Degrades per feature at
// runtime: no ring -> synchronous ReadFile/WriteFile on the deferred
// pump's lanes, NO_BUFFERING refused -> buffered. Internal to src/io/,
// never installed.
//
// The entry points are resolved from KernelBase.dll at first use, not
// linked, so the binary loads on Windows 10, where they do not exist;
// <ioringapi.h> is included for the types only. Which rung was
// reached is in the file's name(), including why a ring was refused.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#if defined(__has_include)
#if __has_include(<ioringapi.h>)
#include <ioringapi.h>
#define BLAKE3PP_HAS_IORING_API 1
#endif
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "io/backend.hpp"
// unique_handle, win_file, throw_winerr and read_error: the Windows
// handle plumbing is the port backend's and is shared verbatim.
#include "io/iocp_backend.hpp"

namespace blake3pp::detail::io_impl {

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// With the port's helpers, under their seamed name (see iocp_backend.hpp).
inline namespace test_seams {
#endif

// Why a ring could not be had, for the fallback name the tools print.
// "no api" (Windows 10, or a KernelBase without the exports) and a
// version below 3 are otherwise indistinguishable in bench output, and a
// CreateIoRing failure is a third thing again.
struct ioring_refusal {
  enum class kind { none, no_api, old_version, create_failed };
  kind why = kind::none;
  std::uint32_t version = 0;  // what QueryIoRingCapabilities reported
  std::uint32_t hr = 0;       // what CreateIoRing or the op probe answered
};

inline std::string hex32(std::uint32_t v) {
  return std::format("{:#010x}", v);
}

inline std::string no_ioring_suffix(const ioring_refusal& r) {
  switch (r.why) {
    case ioring_refusal::kind::no_api:
      return " [ioring: no api]";
    case ioring_refusal::kind::old_version:
      return " [ioring: version " + std::to_string(r.version) + "]";
    case ioring_refusal::kind::create_failed:
      return " [ioring: CreateIoRing failed " + hex32(r.hr) + "]";
    case ioring_refusal::kind::none:
      break;
  }
  return {};
}

#if defined(BLAKE3PP_HAS_IORING_API)

// The IoRing entry points, looked up once in the KernelBase that is
// already mapped into every process. Nothing is loaded here: a
// GetModuleHandleW of a module that is absent answers null and the table
// stays empty, which is the "no api" rung. The pointer types come from
// the SDK's declarations, so a signature cannot drift silently.
struct ioring_api {
  decltype(&::QueryIoRingCapabilities) query_capabilities = nullptr;
  decltype(&::CreateIoRing) create = nullptr;
  decltype(&::CloseIoRing) close = nullptr;
  decltype(&::SubmitIoRing) submit = nullptr;
  decltype(&::PopIoRingCompletion) pop = nullptr;
  decltype(&::SetIoRingCompletionEvent) set_completion_event = nullptr;
  decltype(&::BuildIoRingReadFile) build_read = nullptr;
  decltype(&::BuildIoRingWriteFile) build_write = nullptr;
  decltype(&::BuildIoRingCancelRequest) build_cancel = nullptr;
  decltype(&::GetIoRingInfo) get_info = nullptr;
  decltype(&::IsIoRingOpSupported) is_op_supported = nullptr;
  // Optional, and resolved for completeness; nothing calls it, because the writer's
  // finish() lands its bytes through SetEndOfFile on the plain handle,
  // exactly as the completion-port backend's does.
  decltype(&::BuildIoRingFlushFile) build_flush = nullptr;

  bool ok = false;  // every entry point the read/write path needs is here

  ioring_api() noexcept {
    HMODULE const base = ::GetModuleHandleW(L"KernelBase.dll");
    if (base == nullptr) {
      return;
    }
    // GetProcAddress answers a generic function pointer; the typed slot
    // takes it through std::bit_cast, as every pointer conversion in the
    // library does, which is well-defined for two function pointer types
    // of one size.
    const auto resolve = [base]<class F>(F& slot, const char* name) noexcept {
      slot = std::bit_cast<F>(::GetProcAddress(base, name));
    };
    resolve(query_capabilities, "QueryIoRingCapabilities");
    resolve(create, "CreateIoRing");
    resolve(close, "CloseIoRing");
    resolve(submit, "SubmitIoRing");
    resolve(pop, "PopIoRingCompletion");
    resolve(set_completion_event, "SetIoRingCompletionEvent");
    resolve(build_read, "BuildIoRingReadFile");
    resolve(build_write, "BuildIoRingWriteFile");
    resolve(build_cancel, "BuildIoRingCancelRequest");
    resolve(get_info, "GetIoRingInfo");
    resolve(is_op_supported, "IsIoRingOpSupported");
    resolve(build_flush, "BuildIoRingFlushFile");
    ok = query_capabilities != nullptr && create != nullptr &&
         close != nullptr && submit != nullptr && pop != nullptr &&
         set_completion_event != nullptr && build_read != nullptr &&
         build_write != nullptr && build_cancel != nullptr;
  }
};

[[nodiscard]] inline const ioring_api& ioring_entry_points() noexcept {
  static const ioring_api table;
  return table;
}

// The ring itself: creation and the version gate, the submission and
// completion calls, and nothing about any file. Closed exactly once.
class ioring {
 public:
  ioring() = default;
  ioring(const ioring&) = delete;
  ioring& operator=(const ioring&) = delete;
  ~ioring() { close(); }

  // IORING_VERSION_3 is the floor: version 1 (21H2) has reads only, and
  // reads without writes and cancel is not a rung worth
  // keeping a second backend for. The op probe is the second half of the
  // same question -- a version number is what the ring CAN do, and
  // IsIoRingOpSupported is what this ring DOES.
  [[nodiscard]] bool init(std::uint32_t sq_entries,
                          std::uint32_t cq_entries) noexcept {
    const ioring_api& api = ioring_entry_points();
    if (!api.ok) {
      refusal_ = {ioring_refusal::kind::no_api, 0, 0};
      return false;
    }
    IORING_CAPABILITIES caps{};
    HRESULT hr = api.query_capabilities(&caps);
    if (FAILED(hr)) {
      refusal_ = {ioring_refusal::kind::no_api, 0, static_cast<std::uint32_t>(hr)};
      return false;
    }
    if (caps.MaxVersion < IORING_VERSION_3) {
      refusal_ = {ioring_refusal::kind::old_version,
                  static_cast<std::uint32_t>(caps.MaxVersion), 0};
      return false;
    }
    IORING_CREATE_FLAGS flags{};
    flags.Required = IORING_CREATE_REQUIRED_FLAGS_NONE;
    flags.Advisory = IORING_CREATE_ADVISORY_FLAGS_NONE;
    HIORING h = nullptr;
    hr = api.create(IORING_VERSION_3, flags, sq_entries, cq_entries, &h);
    if (FAILED(hr) || h == nullptr) {
      refusal_ = {ioring_refusal::kind::create_failed, 0,
                  static_cast<std::uint32_t>(hr)};
      return false;
    }
    ring_ = h;
    if (!op_supported(IORING_OP_READ) || !op_supported(IORING_OP_WRITE)) {
      // A ring that claims version 3 and cannot read or write would
      // answer every transfer with an error while name() said "ioring".
      refusal_ = {ioring_refusal::kind::old_version,
                  static_cast<std::uint32_t>(caps.MaxVersion), 0};
      close();
      return false;
    }
    // What the ring actually granted, which is at least what was asked
    // for. The submission bound below rests on it.
    IORING_INFO info{};
    if (api.get_info != nullptr && SUCCEEDED(api.get_info(ring_, &info))) {
      sq_size_ = info.SubmissionQueueSize;
    } else {
      sq_size_ = sq_entries;
    }
    return true;
  }

  [[nodiscard]] HIORING get() const noexcept { return ring_; }
  [[nodiscard]] bool valid() const noexcept { return ring_ != nullptr; }
  [[nodiscard]] std::uint32_t sq_size() const noexcept { return sq_size_; }
  [[nodiscard]] const ioring_refusal& refusal() const noexcept {
    return refusal_;
  }

  [[nodiscard]] bool op_supported(IORING_OP_CODE op) const noexcept {
    const ioring_api& api = ioring_entry_points();
    return ring_ != nullptr && api.is_op_supported != nullptr &&
           api.is_op_supported(ring_, op) != FALSE;
  }

  void close() noexcept {
    if (ring_ != nullptr) {
      (void)ioring_entry_points().close(ring_);
      ring_ = nullptr;
    }
  }

 private:
  HIORING ring_ = nullptr;
  std::uint32_t sq_size_ = 0;
  ioring_refusal refusal_{};
};

#endif  // BLAKE3PP_HAS_IORING_API

// A transfer the ring reports as aborted is one BuildIoRingCancelRequest
// reached. The ring wraps a Win32 status in an HRESULT, so it is
// unwrapped here and named the way the completion-port backend names it:
// a caller sees operation_canceled either way.
[[nodiscard]] inline std::error_code ring_error(HRESULT hr) noexcept {
  if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
    return read_error(static_cast<DWORD>(HRESULT_CODE(hr)));
  }
  return std::error_code(static_cast<int>(hr), std::system_category());
}

// The IoRing reader/writer context: one ring, any number of files,
// transfers named by the caller's operation, completions delivered as
// callbacks out of poll().
//
// A ring is single-threaded, as the driver model is: wake() is the only
// member another thread may call, and it signals the event the ring was
// given, so completions, a lane's report and another thread's wake share
// one wait.
class ioring_context {
 public:
  class file;

  // No OVERLAPPED: the ring carries the offset in the submission entry
  // and the operation's address is its userData, so what is left here is
  // continuation state.
  struct transfer : performed_transfer {
    std::byte* dst = nullptr;        // reads fill this
    const std::byte* src = nullptr;  // writes read from this
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    win_file* sync_file = nullptr;  // the deferred path needs the handles
    file* owner_file = nullptr;     // which file's drain owes this op
    transfer* next_deferred = nullptr;
    bool is_write = false;
    bool on_ring = false;    // the ring owes a completion for it
    bool cancelled = false;  // a cancel was asked for; one is enough
  };

  class file {
   public:
    file(ioring_context& ctx, const std::filesystem::path& path,
         const file_open_options& how)
        : write_(how.write) {
      // FILE_SHARE_READ and nothing else for a read: a digest describes
      // the bytes that were there, so nothing may rewrite or shorten the
      // file while it is being read. A writable open shares write access
      // with ITSELF, because engage() reopens the same path for the
      // direct handle and two GENERIC_WRITE handles can only coexist if
      // the first one permits it.
      const DWORD access = write_ ? GENERIC_WRITE : GENERIC_READ;
      const DWORD creation = write_ ? CREATE_ALWAYS : OPEN_EXISTING;
      const DWORD share =
          write_ ? (FILE_SHARE_READ | FILE_SHARE_WRITE) : FILE_SHARE_READ;
      f_.open(path.c_str(), access, share, creation,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
      size_ = write_ ? 0 : f_.stat_size();
      // Preallocation belongs to the OPEN, not to the finish: a write
      // that extends the file serializes, and with an async direct queue
      // that collapses it to one stalled write at a time.
      //
      // NTFS then zeroes every byte between the valid data length and a
      // write that lands beyond it, which with several writes in flight
      // cost the writer half the device. Two ways out, in the order the
      // file system allows them:
      //  - the valid data length set to the preallocation up front. It
      //    needs SeManageVolumePrivilege and exposes the old blocks until
      //    they are written, so only on request (waive_zero_fill);
      //  - else a sparse file, whose gaps stay holes that read as zeros,
      //    made plain again by set_size() once the file is complete.
      // The sparse flag has to precede the preallocation for the range to
      // be a hole, and a valid data length can only be set on a file that
      // is not sparse.
      if (write_ && how.preallocate_bytes > 0) {
        const bool valid_data =
            how.waive_zero_fill && enable_manage_volume_privilege();
        if (!valid_data) {
          sparse_ = set_sparse(true);
        }
        if (set_end_of_file(how.preallocate_bytes)) {
          if (valid_data) {
            valid_data_ =
                ::SetFileValidData(
                    f_.plain.get(),
                    static_cast<LONGLONG>(how.preallocate_bytes)) != 0;
          }
        }
      }
      ring_handle_ = ctx.engage(f_, path.c_str(), access, share, how.direct_io);
      name_ = ctx.name_for(f_.direct, ring_handle_, write_);
      if (valid_data_) {
        name_ += "+validdata";
      } else if (sparse_) {
        name_ += "+sparse";
      }
      ctx.adopt(*this);
    }

    ~file() {
      if (ctx_ != nullptr) {
        // Reclaimed, then off the context's list: forget() leaves a file
        // on it, because a file that is merely idled is still open and
        // may be read again. This one is not -- and a destroyed file
        // still on the list is what a later drain() would walk.
        ctx_->forget(*this);
        ctx_->unlink(*this);
      }
    }
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Trims a preallocation back to what was written. Harmless when
    // nothing was preallocated, which is what a reader's file is. A file
    // made sparse for the run is a plain file again from here: every byte
    // below the new end has been written, so there is no hole to keep.
    void set_size(std::uint64_t bytes) {
      if (!set_end_of_file(bytes)) {
        throw_winerr("SetEndOfFile");
      }
      if (sparse_ && set_sparse(false)) {
        sparse_ = false;
      }
    }

   private:
    friend class ioring_context;

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

    // Sets or clears the sparse attribute on the plain handle. A file
    // system without sparse files refuses, and the writer then runs with
    // the zero-fill it always had.
    [[nodiscard]] bool set_sparse(bool on) noexcept {
      FILE_SET_SPARSE_BUFFER in{};
      in.SetSparse = on ? TRUE : FALSE;
      DWORD got = 0;
      return ::DeviceIoControl(f_.plain.get(), FSCTL_SET_SPARSE, &in,
                               sizeof in, nullptr, 0, &got, nullptr) != 0;
    }

    // Enables SeManageVolumePrivilege on the process token, which is what
    // SetFileValidData checks. False where the account does not hold it,
    // which is every non-elevated session: ERROR_NOT_ALL_ASSIGNED comes
    // back from a successful AdjustTokenPrivileges call in that case.
    [[nodiscard]] static bool enable_manage_volume_privilege() noexcept {
      HANDLE tok = nullptr;
      if (::OpenProcessToken(::GetCurrentProcess(),
                             TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                             &tok) == 0) {
        return false;
      }
      TOKEN_PRIVILEGES tp{};
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      const bool ok =
          ::LookupPrivilegeValueW(nullptr, L"SeManageVolumePrivilege",
                                  &tp.Privileges[0].Luid) != 0 &&
          ::AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) != 0 &&
          ::GetLastError() == ERROR_SUCCESS;
      ::CloseHandle(tok);
      return ok;
    }

    win_file f_;
    std::uint64_t size_ = 0;
    bool write_ = false;
    std::string name_ = "readfile";
    bool sparse_ = false;      // made sparse for the run; cleared by set_size()
    bool valid_data_ = false;  // the valid data length was set up front
    // The reopened handle is FILE_FLAG_OVERLAPPED and may ride the ring.
    // win_file::use_iocp stays false throughout: there is no port here,
    // and the synchronous ladder reads this instead.
    bool ring_handle_ = false;
    ioring_context* ctx_ = nullptr;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's
  };

  // The event exists whether or not a ring does: it is the wake
  // primitive, so a context whose transfers all go to the pump still has
  // exactly one thing a blocked poll sleeps on.
  ioring_context(const reader_context_options& opts, unsigned max_inflight)
      : event_(::CreateEventW(nullptr, /*bManualReset=*/FALSE,
                              /*bInitialState=*/FALSE, nullptr)),
        queue_depth_(std::max(1u, max_inflight)),
        async_requested_(opts.async),
        // Declared last, and initialized last: see the member list.
        pump_(io_lanes::waker<ioring_context>(), this,
              &perform_deferred, io_lanes_for(max_inflight)) {
    if (!event_) {
      throw_winerr("CreateEventW");
    }
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!opts.async) {
      return;
    }
    // What one batch between two flushes can hold: a transfer and its
    // continuation for every slot, plus four entries for a cancel and
    // spares. The completion queue is twice
    // that, so a completion can never be dropped for want of room.
    const std::uint32_t sq = 2u * max_inflight + 4u;
    if (!ring_.init(sq, 2u * sq)) {
      return;
    }
    if (FAILED(ioring_entry_points().set_completion_event(ring_.get(),
                                                          event_.get()))) {
      // Without the event a blocked poll could not be woken, which is
      // worse than not having a ring at all.
      ring_.close();
      return;
    }
    use_ring_ = true;
#else
    (void)max_inflight;
    refusal_ = {ioring_refusal::kind::no_api, 0, 0};
#endif
  }
  ioring_context(const ioring_context&) = delete;
  ioring_context& operator=(const ioring_context&) = delete;

  // Every file is destroyed first, and each one's destructor forgot its
  // own transfers, so the drain finds nothing owed. What can remain is a
  // wake nobody consumed, which the event drops with itself.
  ~ioring_context() { drain(); }

  // Whether a ring was had, which is what backend_select.hpp branches on
  // and what the degradation suffix explains when it was not.
  [[nodiscard]] bool using_ring() const noexcept { return use_ring_; }
  // Why no ring was had, for a caller that falls back to the port and
  // wants its file names to say so; `none` while a ring is in use.
  [[nodiscard]] const ioring_refusal& refusal() const noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    return ring_.refusal();
#else
    return refusal_;
#endif
  }

  // This context's rung and the one it degrades to, declared for the
  // ladder walk in src/io/io_engines.cpp. This is the ladder inside one
  // ioring_context: a ring it cannot have leaves its transfers to
  // ReadFile on the lanes. windows_context composes a second ladder on
  // top, with the completion port between the two, and declares that.
  static constexpr io_engine rung_engine = io_engine::ioring;
  using rung_below = win_sync_rung;

  // Settled unless the machine might hand over a ring next time. An
  // absent API and a version below 3 are properties of this Windows; a
  // CreateIoRing or event failure is a resource error, and a caller that
  // asks again may well get one.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    if (use_ring_) {
      return {rung_engine, true};
    }
    const ioring_refusal::kind why = refusal().why;
    return {rung_below::rung_engine,
            why == ioring_refusal::kind::no_api || why == ioring_refusal::kind::old_version};
  }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    place(op, f);
  }

  // The write half of the same contract and the same ladder: a length
  // NO_BUFFERING cannot take, or a file whose reopen never engaged, goes
  // to the pump and a lane writes it through the plain handle. Nothing
  // above the context sees the difference.
  //
  // A zero-length write goes to the pump too: the ring would
  // report zero bytes, which is the same shape as a short transfer and
  // cannot be told apart from one.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) noexcept {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    place(op, f);
  }

  // The ring's depth where any transfer of this run rode it, and one
  // otherwise. A file whose reopen never engaged sends everything to the
  // pump, whose lanes read through the one non-overlapped handle from the
  // first open, the case iocp_context::device_depth() reports as one.
  [[nodiscard]] unsigned device_depth() const noexcept {
    return used_ring_ ? queue_depth_ : 1u;
  }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept {
    return pump_.lane_wait_ns();
  }
  [[nodiscard]] device_time device() const noexcept { return device_; }

  // Asks the ring to abandon one transfer. The request is a submission of
  // its own, sent with the next flush, and its completion carries a
  // userData no transfer can wear so poll() drops it; what matters is
  // the target's own completion, which arrives either way. A transfer the
  // pump has is cancelled through the pump, under its lock, because the
  // flag is what a lane reads when it takes one; one a lane already has
  // runs to completion.
  void cancel(transfer& op) noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!op.on_ring) {
      pump_.cancel(op);
      return;
    }
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    const ioring_api& api = ioring_entry_points();
    if (FAILED(api.build_cancel(ring_.get(), handle_ref(*op.owner_file),
                                op_ud(&op), cancel_ud()))) {
      return;
    }
    ++queued_;
    ++ring_owed_;
#else
    pump_.cancel(op);
#endif
  }

  // One kernel transition for everything queued since the last one. A
  // failure is the mechanism's, not a transfer's, and this is noexcept by
  // contract: it is latched and the next poll() throws it, which is the
  // one place the contract allows a mechanism failure to surface.
  void flush() noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!use_ring_ || queued_ == 0) {
      return;
    }
    (void)submit_quiet();
#endif
  }

  // The ring first, then the pump's finished list: a completion the ring
  // hands back can move its remainder to the pump.
  std::size_t poll(bool block) {
    flush();
    throw_if_submit_failed();
    const std::size_t before = reported_;
    reap_ready();
    serve_deferred();
    if (reported_ != before || !block) {
      return reported_ - before;
    }
    for (;;) {
      if (take_wake()) {
        serve_deferred();
        return reported_ - before;
      }
      wait_once();
      reap_ready();
      serve_deferred();
      if (reported_ != before || take_wake()) {
        return reported_ - before;
      }
    }
  }

  // The one member another thread may call, and how a lane reports a
  // finished transfer. The flag is the memory and the event is only the
  // nudge: the ring signals the same event on every completion, so a wake
  // whose signal a completion's wait consumed is still seen by the next
  // take_wake().
  void wake() noexcept {
    woken_.store(true, std::memory_order_release);
    (void)::SetEvent(event_.get());
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Transfers, or remainders of transfers, performed by the lanes (or by
  // poll() where no lane runs) and reported since construction: zero for a
  // buffered file on a ring, one per direct file whose size is not a
  // multiple of the granule.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }

  // Reaps every transfer the ring still owes without reporting any, and
  // clears the pump, waiting its lanes out. There is no handle-wide cancel
  // here --
  // BuildIoRingCancelRequest names one request by its userData, and this
  // backend keeps no list of them -- so the requests are waited out, as
  // the io_uring backend's drain does.
  //
  // A wait the ring refuses means the requests it holds can never be
  // given back. The caller frees the buffers they name as soon as this
  // returns, so the process ends with the reason instead, the same answer
  // the port and the ring give in the same position.
  void drain() noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (use_ring_) {
      if (queued_ > 0) {
        (void)submit_quiet();
      }
      while (ring_owed_ > 0) {
        IORING_CQE cqe{};
        const HRESULT hr = pop(cqe);
        if (hr == S_OK) {
          continue;  // reaped, not reported
        }
        if (FAILED(hr) || !wait_ring()) {
          std::fprintf(stderr,
                       "blake3pp: the I/O ring failed while %zu request(s) "
                       "were still owed; their buffers cannot be released\n",
                       ring_owed_);
          std::abort();
        }
      }
    }
#endif
    pump_.clear();
    in_flight_ = 0;
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
  }

  // Gives up every transfer this file owns, running no callback, so the
  // file and the buffers its transfers name may be destroyed while the
  // context stays usable. The file's destructor calls this. Legal only
  // while the operations are alive.
  //
  // The ring is shared, so a completion belonging to another file can
  // surface while this one is waited out. It is reported, since dropping
  // it would strand an operation that can never complete, so reclaiming
  // one file while a sibling has transfers in flight can run that
  // sibling's callback from here.
  void forget(file& f) noexcept {
    // The file stays on files_: it is still open and may be read again.
    // What never reached the ring goes first: it names this file's handles
    // and its share of the count. A lane inside a transfer on these
    // handles cannot be recalled, so it is waited out, and what it
    // finished is dropped unreported.
    const auto mine = [&f](const transfer& op) noexcept {
      return op.owner_file == &f;
    };
    std::size_t dropped = pump_.remove_queued_if(mine);
    pump_.quiesce();
    dropped += pump_.remove_done_if(mine);
    in_flight_ -= std::min(dropped, in_flight_);
    f.inflight_ -= std::min(dropped, f.inflight_);
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!use_ring_ || f.inflight_ == 0) {
      return;
    }
    // Everything queued goes to the ring first: an entry still in the
    // submission queue would be sent by a later flush, reading into
    // memory this call is about to release.
    if (queued_ > 0) {
      (void)submit_quiet();
    }
    while (f.inflight_ > 0) {
      IORING_CQE cqe{};
      const HRESULT hr = pop(cqe);
      if (hr != S_OK) {
        if (FAILED(hr) || !wait_ring()) {
          std::fprintf(stderr,
                       "blake3pp: the I/O ring failed while %zu transfer(s) "
                       "of a closing file were still owed; their buffers "
                       "cannot be released\n",
                       f.inflight_);
          std::abort();
        }
        continue;
      }
      if (cqe.UserData == cancel_ud()) {
        continue;
      }
      transfer& op = *op_from(cqe.UserData);
      if (op.owner_file == &f) {
        in_flight_--;
        f.inflight_--;  // reaped, not reported
        continue;
      }
      if (settle(op, cqe) && queued_ > 0) {
        (void)submit_quiet();
      }
    }
#endif
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
    op.on_ring = false;
    op.cancelled = false;
    op.reset_performance();
  }

  // How much of a piece of a transfer the ring takes; zero means none of
  // it. Asked for both directions and for every piece: the initial one,
  // and what is left after a short completion, which resumes at
  // off + filled -- a granule boundary only if the device stopped on one.
  //
  // A buffered overlapped handle takes any shape, so a buffered file's
  // pieces ride whole. NO_BUFFERING rejects an unaligned length and an
  // unaligned offset, so a direct file's piece is cut to the whole
  // granules it holds at a granule offset. The rest (the sub-granule end
  // of a file, or a resumption off a boundary) goes to a lane, on the
  // plain handle, which is not overlapped and cannot ride. The
  // completion-port backend's port_len() and the io_uring backend's
  // piece_for() ask the same question.
  //
  // A zero-length transfer goes to a lane too: the ring would report zero
  // bytes, the same shape as a short transfer.
  [[nodiscard]] std::uint32_t ring_len(const transfer& op, std::uint64_t off,
                                       std::size_t len) const noexcept {
    if (!use_ring_ || op.owner_file == nullptr ||
        !op.owner_file->ring_handle_ || len == 0) {
      return 0;
    }
    if (op.sync_file->direct) {
      const std::size_t granules = len - len % direct_align;
      return off % direct_align == 0 ? static_cast<std::uint32_t>(granules) : 0;
    }
    return static_cast<std::uint32_t>(len);
  }

  void place(transfer& op, file& f) noexcept {
    const std::uint32_t len = ring_len(op, op.off, op.len);
    const bool queued = len != 0 && build(op, len);
    op.on_ring = queued;
    used_ring_ = used_ring_ || queued;
    in_flight_++;
    f.inflight_++;
    if (!queued) {
      pump_.push(op);
    }
  }

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool on_ring,
                                     bool write) const {
    if (on_ring) {
      return direct ? "ioring+direct" : "ioring";
    }
    const char* const sync_name = write ? "writefile" : "readfile";
    std::string n =
        direct ? std::string(sync_name) + "+direct" : std::string(sync_name);
    if (async_requested_ && !use_ring_) {
      n += no_ioring_suffix(refusal());
    }
    return n;
  }

  // NO_BUFFERING and OVERLAPPED are per-open flags, so engaging them
  // means reopening the path. The plain handle from the first open stays
  // for whatever the ring cannot carry. NO_BUFFERING is a property of the
  // filesystem (SMB and some virtual volumes reject it) and OVERLAPPED is
  // not, so a refusal of both drops the cache bypass and asks again for
  // the asynchrony alone.
  //
  // This is win_file::engage() without the completion port: that one
  // associates the reopened handle with an IOCP, which would send the
  // completions somewhere the ring cannot see them.
  [[nodiscard]] bool engage(win_file& f, const wchar_t* path, DWORD access,
                            DWORD share, bool want_direct) const noexcept {
    if (!use_ring_) {
      // No ring to ride: the cache bypass alone is still worth having,
      // and the synchronous path can use a non-overlapped direct handle.
      if (want_direct) {
        (void)reopen(f, path, access, share, /*with_direct=*/true,
                     /*with_async=*/false);
      }
      return false;
    }
    if (want_direct && reopen(f, path, access, share, true, true)) {
      return true;
    }
    return reopen(f, path, access, share, false, true);
  }

  // One rung: reopen with exactly these two flags and take the handle
  // only if it opened. Leaves everything untouched on failure, so the
  // caller may ask again for less.
  [[nodiscard]] static bool reopen(win_file& f, const wchar_t* path,
                                   DWORD access, DWORD share,
                                   bool with_direct,
                                   bool with_async) noexcept {
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (with_direct) {
      flags |= FILE_FLAG_NO_BUFFERING;
    }
    if (with_async) {
      flags |= FILE_FLAG_OVERLAPPED;
    }
    // OPEN_EXISTING in both directions: the plain handle has already
    // created and truncated a file being written, and a second
    // CREATE_ALWAYS here would truncate it again -- including any
    // preallocation just set on it.
    unique_handle cand(::CreateFileW(path, access, share, nullptr,
                                     OPEN_EXISTING, flags, nullptr));
    if (!cand) {
      return false;
    }
    f.fast = std::move(cand);
    f.direct = with_direct;
    return true;
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
        f.next_ = nullptr;
        return;
      }
    }
  }

  // Every callback runs from here, which is what poll() counts: a
  // remainder moved to the pump is still owed, and is not one.
  void complete(transfer& op, std::error_code ec) noexcept {
    device_.add_if_served(op.device_total_ns());
    in_flight_--;
    if (op.owner_file != nullptr) {
      op.owner_file->inflight_--;
    }
    ++reported_;
    op.done(&op, ec);
  }

  [[nodiscard]] static std::uintptr_t op_ud(transfer* op) noexcept {
    return std::bit_cast<std::uintptr_t>(op);
  }
  [[nodiscard]] static transfer* op_from(std::uintptr_t ud) noexcept {
    return std::bit_cast<transfer*>(ud);
  }
  // The cancel request's completion needs a userData no transfer can
  // wear: reaping it as a transfer would walk to an operation that does
  // not exist. The cancel's own address is such a value and costs nothing.
  [[nodiscard]] std::uintptr_t cancel_ud() const noexcept {
    return std::bit_cast<std::uintptr_t>(&cancel_op_);
  }

  // Drains the wake flag. True means a wake() had been issued.
  [[nodiscard]] bool take_wake() noexcept {
    return woken_.exchange(false, std::memory_order_acquire);
  }

  // Blocks until a completion or a wake. One wait serves both because
  // the ring was given this event.
  void wait_once() {
    if (::WaitForSingleObject(event_.get(), INFINITE) == WAIT_FAILED) {
      throw_winerr("WaitForSingleObject(ioring)");
    }
  }

  void throw_if_submit_failed() {
    if (submit_failed_ == 0) {
      return;
    }
    const auto hr = std::exchange(submit_failed_, 0u);
    throw std::system_error(static_cast<int>(hr), std::system_category(),
                            "SubmitIoRing");
  }

  // One transfer, on a lane. Touches the operation and the file it names,
  // and nothing else of this context, so served_synchronously_ is counted
  // where the completion is run.
  static std::error_code perform_deferred(transfer& op) noexcept {
    // From op.filled, not from zero: a short transfer whose remainder
    // the ring would not take arrives here part-done, and the bytes
    // already in the buffer must not be fetched again over themselves.
    try {
      const std::size_t rest = op.len - op.filled;
      const bool fast_async =
          op.owner_file != nullptr && op.owner_file->ring_handle_;
      if (op.is_write) {
        write_sync(*op.sync_file, fast_async, op.off + op.filled,
                   {op.src + op.filled, rest});
      } else {
        read_sync(*op.sync_file, fast_async, op.off + op.filled,
                  {op.dst + op.filled, rest});
      }
      op.filled = op.len;
    } catch (const std::system_error& e) {
      return e.code();
    } catch (...) {
      // read_sync and write_sync throw only system_error; anything else
      // is reported as EIO so it cannot terminate a noexcept lane.
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  // Runs the callbacks of the transfers the lanes finished, on this
  // thread. The inline step serves a machine that would not give the
  // pump a thread.
  void serve_deferred() noexcept {
    for (transfer* op = pump_.take_done(); op != nullptr;) {
      transfer* const next = op->next_deferred;
      report_deferred(*op);
      op = next;
    }
    if (transfer* const op = pump_.run_one_here(); op != nullptr) {
      report_deferred(*op);
    }
  }

  void report_deferred(transfer& op) noexcept {
    const std::error_code ec = op.result();
    if (!ec) {
      served_synchronously_++;
    }
    complete(op, ec);
  }

  // A non-OVERLAPPED handle plus an OVERLAPPED offset blocks until
  // complete. `fast_async` makes it structural that an overlapped handle
  // is never used synchronously.
  static void read_sync(win_file& f, bool fast_async, std::uint64_t off,
                        std::span<std::byte> buf) {
    const HANDLE use_h =
        f.direct && !fast_async && buf.size() % direct_align == 0
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
        throw std::system_error(read_error(::GetLastError()), "ReadFile");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += n;
    }
  }

  // The write twin of read_sync, and the same handle choice. A
  // zero-length write returns without calling WriteFile, which would only
  // set the file pointer.
  static void write_sync(win_file& f, bool fast_async, std::uint64_t off,
                         std::span<const std::byte> buf) {
    if (buf.empty()) {
      return;
    }
    const HANDLE use_h =
        f.direct && !fast_async && buf.size() % direct_align == 0
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

#if defined(BLAKE3PP_HAS_IORING_API)
  [[nodiscard]] static IORING_HANDLE_REF handle_ref(const file& f) noexcept {
    return IoRingHandleRefFromHandle(f.f_.h());
  }

  // Queues one read or write of `len` bytes from where the transfer
  // stopped. False means the ring refused the entry, and the caller hands
  // the transfer to the pump instead: a counted transfer has to reach its
  // callback whatever the ring does.
  [[nodiscard]] bool build(transfer& op, std::uint32_t len) noexcept {
    const ioring_api& api = ioring_entry_points();
    const std::uint64_t at = op.off + op.filled;
    HRESULT hr = 0;
    if (op.is_write) {
      void* const from = const_cast<std::byte*>(op.src) + op.filled;
      hr = api.build_write(ring_.get(), handle_ref(*op.owner_file),
                           IoRingBufferRefFromPointer(from), len, at, FILE_WRITE_FLAGS_NONE,
                           op_ud(&op), IOSQE_FLAGS_NONE);
    } else {
      hr = api.build_read(ring_.get(), handle_ref(*op.owner_file),
                          IoRingBufferRefFromPointer(op.dst + op.filled), len, at,
                          op_ud(&op), IOSQE_FLAGS_NONE);
    }
    // The submission queue holds a transfer and its continuation for
    // every slot plus four, and nothing queues past that between two
    // flushes, so a full queue is a sizing mistake, not a condition to
    // recover from.
    assert(hr != IORING_E_SUBMISSION_QUEUE_FULL &&
           "the I/O ring's submission queue is too small for the queue "
           "depth it was built for");
    if (FAILED(hr)) {
      return false;
    }
    // The piece is the ring's from here, and charged to the device from
    // here.
    op.device_begin();
    ++queued_;
    ++ring_owed_;
    return true;
  }

  // One transition for everything queued. False latches the failure for
  // the next poll() to throw: there is no way to take a built entry back
  // out of the ring, so the transfers it names stay owed and the
  // mechanism is reported rather than guessed at.
  [[nodiscard]] bool submit_quiet() noexcept {
    UINT32 submitted = 0;
    const HRESULT hr =
        ioring_entry_points().submit(ring_.get(), 0, 0, &submitted);
    if (FAILED(hr)) {
      if (submit_failed_ == 0) {
        submit_failed_ = static_cast<std::uint32_t>(hr);
      }
      return false;
    }
    queued_ -= std::min<std::size_t>(submitted, queued_);
    // The entries a failed submit left in the ring are offered again by
    // the next one, so a success is what settles the mechanism question.
    submit_failed_ = 0;
    return true;
  }

  // Takes one completion if the ring has one: S_OK took one, S_FALSE
  // means empty, anything else is the ring's own failure.
  [[nodiscard]] HRESULT pop(IORING_CQE& cqe) noexcept {
    const HRESULT hr = ioring_entry_points().pop(ring_.get(), &cqe);
    if (hr == S_OK) {
      ring_owed_ -= std::min<std::size_t>(1, ring_owed_);
    }
    return hr;
  }

  // Sleeps until the ring has at least one completion, submitting
  // whatever is queued on the way. Used where a wake must not end the
  // wait: a drain and a closing file owe callbacks to nobody.
  [[nodiscard]] bool wait_ring() noexcept {
    UINT32 submitted = 0;
    const HRESULT hr =
        ioring_entry_points().submit(ring_.get(), 1, INFINITE, &submitted);
    if (FAILED(hr)) {
      return false;
    }
    queued_ -= std::min<std::size_t>(submitted, queued_);
    return true;
  }

  // What one completion of `op` means. A transfer short of its length
  // continues from where it stopped, reads and writes alike. Returns
  // whether it was requeued rather than completed, which is the caller's
  // cue to submit.
  [[nodiscard]] bool settle(transfer& op, const IORING_CQE& cqe) noexcept {
    // This piece is back from the ring, whatever it reports.
    op.device_end();
    if (FAILED(cqe.ResultCode)) {
      complete(op, ring_error(cqe.ResultCode));
      return false;
    }
    const auto moved = static_cast<std::size_t>(cqe.Information);
    if (moved == 0) {
      // Short of the length with nothing left to give: the file ended
      // where the engine was told it would not.
      complete(op, std::error_code(EIO, std::generic_category()));
      return false;
    }
    op.filled += moved;
    if (op.filled < op.len) {
      const std::uint32_t len =
          ring_len(op, op.off + op.filled, op.len - op.filled);
      if (len == 0 || !build(op, len)) {
        // The remainder is not something this file may hand the ring. It
        // is still owed, so it goes to the pump like an unalignable
        // transfer does from the start, and a lane finishes what is left.
        op.on_ring = false;
        pump_.push(op);
        return false;
      }
      return true;
    }
    complete(op, {});
    return false;
  }

  // Consumes every completion the ring has without blocking, continuing
  // short transfers.
  void reap_ready() {
    if (!use_ring_) {
      return;
    }
    bool requeued = false;
    for (;;) {
      IORING_CQE cqe{};
      const HRESULT hr = pop(cqe);
      if (hr == S_FALSE) {
        break;  // nothing more queued
      }
      if (FAILED(hr)) {
        throw std::system_error(static_cast<int>(hr), std::system_category(),
                                "PopIoRingCompletion");
      }
      if (cqe.UserData == cancel_ud()) {
        // The cancel request reports whether it found anything, which
        // changes nothing a caller has to do; the target's own completion
        // is the one that matters and arrives either way.
        continue;
      }
      transfer& op = *op_from(cqe.UserData);
      requeued |= settle(op, cqe);
    }
    if (requeued) {
      // The continuation of a short transfer is the same kind of entry as
      // the transfer itself.
      (void)submit_quiet();
    }
  }
#else
  // Without the SDK's declarations there is no ring: use_ring_ is false
  // for the life of the context, so ring_len() takes nothing, place()
  // hands every transfer to the pump and poll() has no completion queue
  // to read.
  [[nodiscard]] static bool build(transfer&, std::uint32_t) noexcept {
    return false;
  }
  static void reap_ready() noexcept {}
#endif  // BLAKE3PP_HAS_IORING_API

  // Declaration order is the teardown contract: the ring is closed before
  // the event it was given, and both after the operation whose address
  // every cancel completion wears. Do not move ring_ above these.
  unique_handle event_;
  transfer cancel_op_{};  // address only: every cancel completion's userData
#if defined(BLAKE3PP_HAS_IORING_API)
  ioring ring_;
#else
  ioring_refusal refusal_{};
#endif
  file* files_ = nullptr;  // every open file, for the drain
  std::size_t in_flight_ = 0;
  std::size_t served_synchronously_ = 0;
  // Callbacks run since construction; poll() returns its growth.
  std::size_t reported_ = 0;
  std::size_t queued_ = 0;     // entries built and not yet submitted
  std::size_t ring_owed_ = 0;  // requests the ring has yet to hand back
  std::uint32_t submit_failed_ = 0;  // latched for the next poll() to throw
  // What the device itself took, stamped by the ring where it carried a
  // piece and by a lane where one did.
  device_time device_{};
  // What the caller asked to have in flight at once; device_depth()
  // reports it where the ring took the transfers.
  unsigned queue_depth_ = 1;
  bool async_requested_ = true;
  bool use_ring_ = false;
  // Whether any transfer of this run reached the ring, which is what
  // device_depth() reports against.
  bool used_ring_ = false;
  std::atomic<bool> woken_{false};
  // The lanes that serve what the ring will not take. Declared last,
  // so the destructor's join runs while the event its wake() signals is
  // still open. Do not move this up.
  deferred_pump<transfer> pump_;
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time this header is
// compiled, before any engine instantiation exists.
static_assert(io_context<ioring_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
