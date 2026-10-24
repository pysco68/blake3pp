#pragma once

// The Windows 11 backend: the same CreateFileW handles the completion-port
// backend opens, driven through an I/O ring instead of a port. The mapping
// to the io_uring backend is the closer of the two: BuildIoRingReadFile
// fills a submission entry without entering the kernel, SubmitIoRing is
// io_uring_enter, PopIoRingCompletion drains the completion queue, and
// FILE_FLAG_NO_BUFFERING is O_DIRECT (same sector-alignment demands, same
// buffered-handle escape hatch for the unaligned tail). Degrades
// per-feature at RUNTIME: no ring -> sync ReadFile/WriteFile, NO_BUFFERING
// refused -> buffered. Internal to src/io/, never installed.
//
// The entry points are resolved from KernelBase.dll at first use rather
// than linked, so the binary loads on Windows 10, where they do not
// exist; <ioringapi.h> is included for the types only. Which rung was
// reached is in the file's name(), including why a ring was refused.
//
// Registered buffers and registered file handles are behind
// reader_context_options::registered_buffers and default OFF: this is the
// experiment the io_uring backend's header reports for Linux (28-38%
// slower there) asked of a different kernel.

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

// Why a ring could not be had, for the fallback name the tools print.
// "no api" (Windows 10, or a KernelBase without the exports) and a
// version below 3 are otherwise indistinguishable in bench output, and a
// CreateIoRing failure is a third thing again.
struct ioring_refusal {
  enum kind { none, no_api, old_version, create_failed };
  kind why = none;
  std::uint32_t version = 0;  // what QueryIoRingCapabilities reported
  std::uint32_t hr = 0;       // what CreateIoRing or the op probe answered
};

inline std::string hex32(std::uint32_t v) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string s = "0x";
  for (int shift = 28; shift >= 0; shift -= 4) {
    s += digits[(v >> shift) & 0xFu];
  }
  return s;
}

inline std::string no_ioring_suffix(const ioring_refusal& r) {
  switch (r.why) {
    case ioring_refusal::no_api:
      return " [ioring: no api]";
    case ioring_refusal::old_version:
      return " [ioring: version " + std::to_string(r.version) + "]";
    case ioring_refusal::create_failed:
      return " [ioring: CreateIoRing failed " + hex32(r.hr) + "]";
    case ioring_refusal::none:
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
  // Registration and the explicit flush are optional: a ring without
  // them still reads and writes, and registration is off by default.
  decltype(&::BuildIoRingRegisterBuffers) build_register_buffers = nullptr;
  decltype(&::BuildIoRingRegisterFileHandles) build_register_files = nullptr;
  // Resolved for completeness; nothing calls it, because the writer's
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
    resolve(build_register_buffers, "BuildIoRingRegisterBuffers");
    resolve(build_register_files, "BuildIoRingRegisterFileHandles");
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
  // reads without writes, cancel and registration is not a rung worth
  // keeping a second backend for. The op probe is the second half of the
  // same question -- a version number is what the ring CAN do, and
  // IsIoRingOpSupported is what this ring DOES.
  [[nodiscard]] bool init(std::uint32_t sq_entries,
                          std::uint32_t cq_entries) noexcept {
    const ioring_api& api = ioring_entry_points();
    if (!api.ok) {
      refusal_ = {ioring_refusal::no_api, 0, 0};
      return false;
    }
    IORING_CAPABILITIES caps{};
    HRESULT hr = api.query_capabilities(&caps);
    if (FAILED(hr)) {
      refusal_ = {ioring_refusal::no_api, 0, static_cast<std::uint32_t>(hr)};
      return false;
    }
    if (caps.MaxVersion < IORING_VERSION_3) {
      refusal_ = {ioring_refusal::old_version,
                  static_cast<std::uint32_t>(caps.MaxVersion), 0};
      return false;
    }
    IORING_CREATE_FLAGS flags{};
    flags.Required = IORING_CREATE_REQUIRED_FLAGS_NONE;
    flags.Advisory = IORING_CREATE_ADVISORY_FLAGS_NONE;
    HIORING h = nullptr;
    hr = api.create(IORING_VERSION_3, flags, sq_entries, cq_entries, &h);
    if (FAILED(hr) || h == nullptr) {
      refusal_ = {ioring_refusal::create_failed, 0,
                  static_cast<std::uint32_t>(hr)};
      return false;
    }
    ring_ = h;
    if (!op_supported(IORING_OP_READ) || !op_supported(IORING_OP_WRITE)) {
      // A ring that claims version 3 and cannot read or write would
      // answer every transfer with an error while name() said "ioring".
      refusal_ = {ioring_refusal::old_version,
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
[[nodiscard]] inline std::error_code ring_error(long hr) noexcept {
  const auto code = static_cast<unsigned long>(hr);
  if (((code >> 16) & 0x7FFFu) == FACILITY_WIN32) {
    return read_error(static_cast<DWORD>(code & 0xFFFFu));
  }
  return std::error_code(static_cast<int>(hr), std::system_category());
}

// The IoRing reader/writer context: one ring, any number of files,
// transfers named by the caller's operation, completions delivered as
// callbacks out of poll().
//
// A ring is single-threaded by design, which is the driver model exactly:
// wake() is the only member another thread may call, and it is the event
// the ring was given, so completions and wakes share one wait.
class ioring_context {
 public:
  class file;

  // No OVERLAPPED: the ring carries the offset in the submission entry
  // and the operation's address is its userData, so what is left here is
  // continuation state.
  struct transfer : transfer_base {
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
      // The handle table is the ring's, not the file's, and registering
      // it replaces the whole table, so only a context with this one file
      // open may have one. That covers the engines, which run one file
      // per context, and leaves the multi-file driver on raw handle refs.
      ctx.try_register_handle(*this);
    }

    ~file() {
      if (ctx_ != nullptr) {
        // Reclaimed, then off the context's list: forget() leaves a file
        // on it, because a file that is merely idled is still open and
        // may be read again. This one is not -- and a destroyed file
        // still on the list is what a later drain() would walk.
        ctx_->forget(*this);
        ctx_->release_handle(*this);
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
    int reg_index_ = -1;  // the ring's handle table slot, or -1 for none
    ioring_context* ctx_ = nullptr;
    file* next_ = nullptr;      // the context's list of open files
    std::size_t inflight_ = 0;  // this file's share of the context's
  };

  // The event exists whether or not a ring does: it is the wake
  // primitive, so a context whose transfers all go through the deferred
  // list still has exactly one thing a blocked poll sleeps on.
  ioring_context(const reader_context_options& opts, unsigned max_inflight)
      : event_(::CreateEventW(nullptr, /*bManualReset=*/FALSE,
                              /*bInitialState=*/FALSE, nullptr)),
        async_requested_(opts.async),
        want_registered_(opts.registered_buffers) {
    if (!event_) {
      throw_winerr("CreateEventW");
    }
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!opts.async) {
      return;
    }
    // What one batch between two flushes can hold: a transfer and its
    // continuation for every slot, plus four entries for the
    // registration, a cancel and spares. The completion queue is twice
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
    refusal_ = {ioring_refusal::no_api, 0, 0};
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
  // ladder walk in src/io/io_engines.cpp. Note that this is the ladder
  // INSIDE one ioring_context -- a ring it cannot have leaves its own
  // transfers on ReadFile. windows_context composes a second one on top,
  // putting the completion port between the two, and declares that.
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
            why == ioring_refusal::no_api || why == ioring_refusal::old_version};
  }

  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = buf.data();
    op.src = nullptr;
    op.is_write = false;
    place(op, f);
  }

  // The write half of the same contract and the same ladder: a length
  // NO_BUFFERING cannot take, or a file whose reopen never engaged,
  // rides the deferred list and is written synchronously inside poll()
  // through the plain handle. Nothing above the context sees the
  // difference.
  //
  // A zero-length write takes the deferred path too: the ring would
  // report zero bytes, which is the same shape as a short transfer and
  // cannot be told apart from one.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) {
    arm(op, f, off, buf.size());
    op.dst = nullptr;
    op.src = buf.data();
    op.is_write = true;
    place(op, f);
  }

  // Hands the ring the arena the engine or the driver reads into, so
  // transfers inside it can name a registered buffer instead of a
  // pointer. Optional, off by default, and silent about failure: a ring
  // that refuses the registration keeps serving pointer refs, which is
  // what every transfer outside the arena uses anyway.
  //
  // Called once, before any transfer: the registration is itself a
  // submission, and its completion is waited out here rather than
  // reported from poll().
  void register_arena(std::span<std::byte> arena) noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!use_ring_ || !want_registered_ || arena_registered_ ||
        arena.empty() || in_flight_ != 0) {
      return;
    }
    const ioring_api& api = ioring_entry_points();
    if (api.build_register_buffers == nullptr ||
        !ring_.op_supported(IORING_OP_REGISTER_BUFFERS)) {
      return;
    }
    // One region, one index: the arena is a single allocation, so a
    // buffer inside it is index 0 plus its offset. A region longer than
    // a UINT32 cannot be described at all, and declines quietly.
    if (arena.size() > 0xFFFFFFFFull) {
      return;
    }
    IORING_BUFFER_INFO info{};
    info.Address = arena.data();
    info.Length = static_cast<UINT32>(arena.size());
    if (FAILED(api.build_register_buffers(ring_.get(), 1, &info, reg_ud()))) {
      return;
    }
    ++queued_;
    ++ring_owed_;
    if (!submit_quiet() || !await_registration()) {
      return;
    }
    arena_ = arena;
    arena_registered_ = true;
    // The rung is part of a file's name, and the files are usually open
    // by the time the arena exists, so they learn it here; one opened
    // afterwards gets it from name_for().
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->name_ += "+regbuf";
    }
#else
    (void)arena;
#endif
  }

  // Whether the arena registration engaged, which name() also reports as
  // "+regbuf": a run has to be readable as the rung it actually took.
  [[nodiscard]] bool arena_registered() const noexcept {
    return arena_registered_;
  }

  // Asks the ring to abandon one transfer. The request is a submission of
  // its own, sent with the next flush, and its completion carries a
  // userData no transfer can wear so poll() drops it; what matters is
  // the target's own completion, which arrives either way. A transfer on
  // the deferred list was never given to the ring, so the flag alone is
  // the whole cancel and run_one_deferred() honours it.
  void cancel(transfer& op) noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (std::exchange(op.cancelled, true)) {
      return;
    }
    if (!op.on_ring) {
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
    op.cancelled = true;
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

  // The ring first, then the deferred list, in that order and never the
  // other way round: a completion the ring hands back can park its
  // remainder on the list, and the list is what serves it.
  std::size_t poll(bool block) {
    flush();
    throw_if_submit_failed();
    std::size_t ran = reap_ready();
    ran += run_one_deferred();
    if (ran > 0 || !block) {
      return ran;
    }
    for (;;) {
      if (take_wake()) {
        return ran;
      }
      wait_once();
      ran += reap_ready();
      ran += run_one_deferred();
      if (ran > 0 || take_wake()) {
        return ran;
      }
    }
  }

  // The one member another thread may call. The flag is the memory and
  // the event is only the nudge: the ring signals the same event on every
  // completion, so a wake whose signal a completion's wait consumed is
  // still seen by the next take_wake().
  void wake() noexcept {
    woken_.store(true, std::memory_order_release);
    (void)::SetEvent(event_.get());
  }

  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_; }

  // Transfers, or remainders of transfers, finished on the synchronous
  // path inside poll() since construction: zero for a buffered file on a
  // ring, one per direct file whose size is not a multiple of the granule.
  [[nodiscard]] std::size_t served_synchronously() const noexcept {
    return served_synchronously_;
  }

  // Reaps every transfer the ring still owes without reporting any, and
  // forgets the deferred list. There is no handle-wide cancel here --
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
    deferred_.clear();
    in_flight_ = 0;
    for (file* f = files_; f != nullptr; f = f->next_) {
      f->inflight_ = 0;
    }
  }

  // A file about to close gives up every transfer it owns, running no
  // callback, so the file and the buffers its transfers name may be
  // destroyed while the context stays usable.
  //
  // One sharp edge, since the ring is shared: a completion belonging to
  // ANOTHER file can surface while this one is waited out, and it is
  // reported normally rather than dropped -- losing it would strand an op
  // that can never complete. So reclaiming one file of a multi-file
  // context while a sibling has transfers in flight runs that sibling's
  // callback from here, which is the one place outside poll() where one
  // runs.
  void forget(file& f) noexcept {
    // The file stays on files_: it is still open and may be read again.
    // What never reached the ring goes first, because a deferred transfer
    // names this file's handles and this file's share of the count, and
    // the next poll() would run it through both after they are gone.
    const std::size_t dropped = deferred_.remove_if(
        [&f](const transfer& op) noexcept { return op.owner_file == &f; });
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
      if (cqe.UserData == reg_ud() || cqe.UserData == cancel_ud()) {
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
  }

  // How much of a piece of a transfer the ring takes; zero means none of
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
  // which is not overlapped and cannot ride. The completion-port
  // backend's port_len() and the io_uring backend's piece_for() are the
  // same question.
  //
  // A zero-length transfer takes the synchronous path too: the ring would
  // report zero bytes, the same shape as a short transfer.
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

  void place(transfer& op, file& f) {
    const std::uint32_t len = ring_len(op, op.off, op.len);
    const bool queued = len != 0 && build(op, len);
    op.on_ring = queued;
    in_flight_++;
    f.inflight_++;
    if (!queued) {
      deferred_.push(op);
    }
  }

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool on_ring,
                                     bool write) const {
    if (on_ring) {
      std::string n = direct ? "ioring+direct" : "ioring";
      if (arena_registered_) {
        n += "+regbuf";
      }
      return n;
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

  void complete(transfer& op, std::error_code ec) noexcept {
    in_flight_--;
    if (op.owner_file != nullptr) {
      op.owner_file->inflight_--;
    }
    op.done(&op, ec);
  }

  [[nodiscard]] static std::uintptr_t op_ud(transfer* op) noexcept {
    return std::bit_cast<std::uintptr_t>(op);
  }
  [[nodiscard]] static transfer* op_from(std::uintptr_t ud) noexcept {
    return std::bit_cast<transfer*>(ud);
  }
  // The cancel request's completion needs a userData no transfer can
  // wear, and so does the registration's: reaping either as a transfer
  // would walk to an operation that does not exist. Their own addresses
  // are such values and cost nothing.
  [[nodiscard]] std::uintptr_t cancel_ud() const noexcept {
    return std::bit_cast<std::uintptr_t>(&cancel_op_);
  }
  [[nodiscard]] std::uintptr_t reg_ud() const noexcept {
    return std::bit_cast<std::uintptr_t>(&reg_op_);
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
        served_synchronously_++;
      } catch (const std::system_error& e) {
        ec = e.code();
      }
    }
    complete(op, ec);
    return 1;
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
        throw_winerr("ReadFile");
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
  // The handle the ring should name: the registered table slot where one
  // was taken, else the handle itself.
  [[nodiscard]] static IORING_HANDLE_REF handle_ref(const file& f) noexcept {
    if (f.reg_index_ >= 0) {
      return IoRingHandleRefFromIndex(static_cast<UINT32>(f.reg_index_));
    }
    return IoRingHandleRefFromHandle(f.f_.h());
  }

  // The buffer the ring should name: the registered region plus an offset
  // where the bytes lie inside the arena, else the pointer. A transfer
  // into a caller's own buffer therefore keeps working while the arena is
  // registered, which is the whole reason the choice is per transfer.
  [[nodiscard]] IORING_BUFFER_REF buffer_ref(void* at) const noexcept {
    if (arena_registered_) {
      auto* const bytes = static_cast<std::byte*>(at);
      if (bytes >= arena_.data() && bytes < arena_.data() + arena_.size()) {
        return IoRingBufferRefFromIndexAndOffset(
            0, static_cast<UINT32>(bytes - arena_.data()));
      }
    }
    return IoRingBufferRefFromPointer(at);
  }

  // Queues one read or write of `len` bytes from where the transfer
  // stopped. False means the ring refused the entry, and the caller puts
  // the transfer on the deferred list instead: a counted transfer has to
  // reach its callback whatever the ring does.
  [[nodiscard]] bool build(transfer& op, std::uint32_t len) noexcept {
    const ioring_api& api = ioring_entry_points();
    const std::uint64_t at = op.off + op.filled;
    HRESULT hr = 0;
    if (op.is_write) {
      void* const from = const_cast<std::byte*>(op.src) + op.filled;
      hr = api.build_write(ring_.get(), handle_ref(*op.owner_file),
                           buffer_ref(from), len, at, FILE_WRITE_FLAGS_NONE,
                           op_ud(&op), IOSQE_FLAGS_NONE);
    } else {
      hr = api.build_read(ring_.get(), handle_ref(*op.owner_file),
                          buffer_ref(op.dst + op.filled), len, at,
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

  // Waits out the registration's own completion, which is the only thing
  // the ring owes at that point.
  [[nodiscard]] bool await_registration() noexcept {
    for (;;) {
      IORING_CQE cqe{};
      const HRESULT hr = pop(cqe);
      if (hr == S_OK) {
        if (cqe.UserData != reg_ud()) {
          continue;
        }
        return SUCCEEDED(cqe.ResultCode);
      }
      if (FAILED(hr) || !wait_ring()) {
        return false;
      }
    }
  }

  // What one completion of `op` means. A transfer short of its length
  // continues from where it stopped, reads and writes alike. Returns
  // whether it was requeued rather than completed, which is the caller's
  // cue to submit.
  [[nodiscard]] bool settle(transfer& op, const IORING_CQE& cqe) noexcept {
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
        // is still owed, so it goes down the same rung an unalignable
        // transfer takes from the start, which finishes what is left
        // inside poll().
        op.on_ring = false;
        deferred_.push(op);
        return false;
      }
      return true;
    }
    complete(op, {});
    return false;
  }

  // Consumes every completion the ring has without blocking, continuing
  // short transfers. Returns the number of callbacks run.
  std::size_t reap_ready() {
    if (!use_ring_) {
      return 0;
    }
    std::size_t ran = 0;
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
      if (cqe.UserData == cancel_ud() || cqe.UserData == reg_ud()) {
        // The cancel request reports whether it found anything, which
        // changes nothing a caller has to do; the target's own completion
        // is the one that matters and arrives either way.
        continue;
      }
      transfer& op = *op_from(cqe.UserData);
      if (settle(op, cqe)) {
        requeued = true;
      } else {
        ran++;
      }
    }
    if (requeued) {
      // The continuation of a short transfer is the same kind of entry as
      // the transfer itself.
      (void)submit_quiet();
    }
    return ran;
  }

  // Registers this file's handle in the ring's table, so its transfers
  // can name an index instead of a HANDLE. The table is the ring's and
  // registering replaces all of it, so only a context with this one file
  // open takes a slot: a second file would have to re-register while the
  // first one's transfers are in flight.
  void try_register_handle(file& f) noexcept {
    const ioring_api& api = ioring_entry_points();
    if (!use_ring_ || !want_registered_ || !f.ring_handle_ ||
        api.build_register_files == nullptr || in_flight_ != 0 ||
        f.next_ != nullptr || reg_handle_ != nullptr ||
        !ring_.op_supported(IORING_OP_REGISTER_FILES)) {
      return;
    }
    HANDLE h = f.f_.h();
    if (FAILED(api.build_register_files(ring_.get(), 1, &h, reg_ud()))) {
      return;
    }
    ++queued_;
    ++ring_owed_;
    if (!submit_quiet() || !await_registration()) {
      return;
    }
    f.reg_index_ = 0;
    reg_handle_ = h;
  }

  // Gives the slot back before the handle in it closes. Only reachable
  // with nothing in flight, which forget() has just established for this
  // file and which one open file makes true for the context.
  void release_handle(file& f) noexcept {
    if (f.reg_index_ < 0) {
      return;
    }
    f.reg_index_ = -1;
    reg_handle_ = nullptr;
    const ioring_api& api = ioring_entry_points();
    if (in_flight_ != 0 || api.build_register_files == nullptr) {
      return;
    }
    if (FAILED(api.build_register_files(ring_.get(), 0, nullptr, reg_ud()))) {
      return;
    }
    ++queued_;
    ++ring_owed_;
    if (submit_quiet()) {
      (void)await_registration();
    }
  }
#else
  // Without the SDK's declarations there is no ring: use_ring_ is false
  // for the life of the context, so ring_len() takes nothing, place()
  // defers every transfer and poll() has no completion queue to read.
  [[nodiscard]] static bool build(transfer&, std::uint32_t) noexcept {
    return false;
  }
  [[nodiscard]] static std::size_t reap_ready() noexcept { return 0; }
  void try_register_handle(file&) noexcept {}
  void release_handle(file&) noexcept {}
#endif  // BLAKE3PP_HAS_IORING_API

  // Declaration order is the teardown contract: the ring is closed before
  // the event it was given, and both after the operations whose addresses
  // the cancel and registration completions wear. Do not move ring_ above
  // these.
  unique_handle event_;
  transfer cancel_op_{};  // address only: every cancel completion's userData
  transfer reg_op_{};     // address only: every registration's userData
#if defined(BLAKE3PP_HAS_IORING_API)
  ioring ring_;
#else
  ioring_refusal refusal_{};
#endif
  deferred_ops<transfer> deferred_;
  file* files_ = nullptr;  // every open file, for the drain
  std::span<std::byte> arena_{};
  void* reg_handle_ = nullptr;  // the handle in the ring's table, if any
  std::size_t in_flight_ = 0;
  std::size_t served_synchronously_ = 0;
  std::size_t queued_ = 0;     // entries built and not yet submitted
  std::size_t ring_owed_ = 0;  // requests the ring has yet to hand back
  std::uint32_t submit_failed_ = 0;  // latched for the next poll() to throw
  bool async_requested_ = true;
  bool want_registered_ = false;
  bool arena_registered_ = false;
  bool use_ring_ = false;
  std::atomic<bool> woken_{false};
};

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time this header is
// compiled, before any engine instantiation exists.
static_assert(io_context<ioring_context>);

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
