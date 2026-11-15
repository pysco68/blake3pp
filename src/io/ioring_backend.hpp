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
// "no api" (Windows 10, or a KernelBase without the exports), a version
// below 3 and a ring that cannot read or write are otherwise
// indistinguishable in bench output, and a CreateIoRing failure is a
// fourth thing again.
struct ioring_refusal {
  enum class kind { none, no_api, old_version, no_read_write, create_failed };
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
    case ioring_refusal::kind::no_read_write:
      return " [ioring: no read/write ops]";
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
      refusal_ = {ioring_refusal::kind::no_read_write,
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

// The I/O ring engine under completion_context: one ring, its pieces and
// completions, and the event the ring signals, which is also the wake.
//
// A ring is single-threaded, as the driver model is: wake() is the only
// member another thread may call, and it signals the event the ring was
// given, so completions, a lane's report and another thread's wake share
// one wait.
class ioring_engine {
 public:
  // No OVERLAPPED: the ring carries the offset in the submission entry
  // and the operation's address is its userData.
  using transfer_part = win_transfer;

  class file_part {
   public:
    // No port: the ring's completions come back through the ring, and
    // without a ring there is nothing for an overlapped handle to ride,
    // so only the cache bypass is asked for.
    file_part(const ioring_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      f.open_file(path, how);
      f.engage(path.c_str(), how.direct_io, e.use_ring_, nullptr);
      name_ = e.name_for(f.direct, f.overlapped, how.write);
      name_ += f.suffix();
    }
    [[nodiscard]] std::uint64_t size() const noexcept { return f.size; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_size(std::uint64_t bytes) { f.set_size(bytes); }

    win_file f;

   private:
    std::string name_ = "readfile";
  };

  // The event exists whether or not a ring does: it is the wake
  // primitive, so a context whose transfers all go to the pump still has
  // exactly one thing a blocked poll sleeps on.
  ioring_engine(const reader_context_options& opts, unsigned max_inflight)
      : event_(::CreateEventW(nullptr, /*bManualReset=*/FALSE,
                              /*bInitialState=*/FALSE, nullptr)),
        queue_depth_(std::max(1u, max_inflight)),
        async_requested_(opts.async) {
    if (!event_) {
      throw_winerr("CreateEventW");
    }
#if defined(BLAKE3PP_HAS_IORING_API)
    if (!opts.async) {
      return;
    }
    // What one batch between two flushes can hold: a transfer and its
    // continuation for every slot, plus four entries for a cancel and
    // spares. The completion queue is twice that, so a completion can
    // never be dropped for want of room.
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
  ioring_engine(const ioring_engine&) = delete;
  ioring_engine& operator=(const ioring_engine&) = delete;

  // Why no ring was had, for a caller that falls back to the port and
  // wants its file names to say so; `none` while a ring is in use.
  [[nodiscard]] const ioring_refusal& refusal() const noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    return ring_.refusal();
#else
    return refusal_;
#endif
  }

  // This engine's rung and the one it degrades to, declared for the
  // ladder walk in src/io/io_engines.cpp: a ring it cannot have leaves
  // its transfers to ReadFile on the lanes. windows_context composes a
  // second ladder on top, with the completion port between the two.
  static constexpr io_engine rung_engine = io_engine::ioring;
  // What failed, as the shared context's messages name it.
  static constexpr std::string_view mechanism = "the I/O ring";
  using rung_below = win_sync_rung;

  // Settled unless the machine might hand over a ring next time. An
  // absent API, a version below 3 and a ring that cannot read or write
  // are properties of this Windows; a CreateIoRing or event failure is a
  // resource error, and a caller that asks again may well get one.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    if (use_ring_) {
      return {rung_engine, true};
    }
    const ioring_refusal::kind why = refusal().why;
    return {rung_below::rung_engine,
            why == ioring_refusal::kind::no_api ||
                why == ioring_refusal::kind::old_version ||
                why == ioring_refusal::kind::no_read_write};
  }
  [[nodiscard]] bool engaged() const noexcept { return use_ring_; }

  // The ring's depth where any transfer of this run rode it, and one
  // otherwise: a file whose reopen never engaged sends everything to the
  // pump, whose lanes read through one non-overlapped handle.
  [[nodiscard]] unsigned device_depth(bool used, unsigned) const noexcept {
    return used ? queue_depth_ : 1u;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return win_perform_rest(op);
  }

  // The rest of a transfer, from op.filled, as one ring entry: all of it
  // for a buffered overlapped handle, the whole granules of it at a
  // granule offset for a direct one. The rest goes to a lane, on the
  // plain handle, which is not overlapped and cannot ride. A zero-length
  // transfer goes to a lane too: the ring would report zero bytes, the
  // same shape as a short transfer.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    const win_file& f = op.owner_file->part().f;
    std::size_t len = op.remaining();
    if (!use_ring_ || !f.overlapped || len == 0) {
      return false;
    }
    len = std::min(len, max_win_piece);
    if (f.direct) {
      len -= len % direct_align;
      if (len == 0 || op.position() % direct_align != 0) {
        return false;
      }
    }
    return build(op, static_cast<std::uint32_t>(len));
  }

  // Asks the ring to abandon one transfer. The request is a submission of
  // its own, sent with the next flush, and its completion carries a
  // userData no transfer can wear so next() drops it; what matters is the
  // target's own completion, which arrives either way.
  template <class Op>
  void cancel([[maybe_unused]] Op& op) noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (FAILED(ioring_entry_points().build_cancel(
            ring_.get(), handle_ref(op.owner_file->part().f), op_ud(&op),
            cancel_ud))) {
      return;
    }
    ++queued_;
    ++ring_owed_;
#endif
  }

  // One kernel transition for everything queued since the last one. A
  // failure is the mechanism's, not a transfer's: there is no way to take
  // a built entry back out of the ring, so it is latched for
  // take_failure(), which the context asks before anything waits.
  template <class Op, class Refused>
  void flush(Refused&&) noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (use_ring_ && queued_ > 0) {
      (void)submit_quiet();
    }
#endif
  }
  template <class Op, class Refused>
  void take_back(Refused&&) noexcept {}
  [[nodiscard]] std::error_code take_failure() noexcept {
    if (submit_failed_ == 0) {
      return {};
    }
    return std::error_code(static_cast<int>(std::exchange(submit_failed_, 0u)),
                           std::system_category());
  }

  // One completion, or a wake, or nothing. Blocking, it waits on the
  // event, which the ring signals on every completion and wake() signals
  // too: one wait serves both.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    for (;;) {
#if defined(BLAKE3PP_HAS_IORING_API)
      while (use_ring_) {
        IORING_CQE cqe{};
        const HRESULT hr = ioring_entry_points().pop(ring_.get(), &cqe);
        if (hr == S_FALSE) {
          break;  // nothing more completed
        }
        if (FAILED(hr)) {
          return {event::kind::failed, nullptr,
                  std::error_code(static_cast<int>(hr), std::system_category())};
        }
        ring_owed_ -= std::min<std::size_t>(1, ring_owed_);
        if (cqe.UserData == cancel_ud) {
          // The cancel request reports whether it found anything, which
          // changes nothing; the target's own completion is what matters.
          return {event::kind::progress};
        }
        Op* const op = op_from<Op>(cqe.UserData);
        if (FAILED(cqe.ResultCode)) {
          return {event::kind::completion, op, ring_error(cqe.ResultCode), 0};
        }
        return {event::kind::completion, op, {},
                static_cast<std::size_t>(cqe.Information)};
      }
#endif
      if (!block) {
        return {};
      }
      if (woken_.exchange(false, std::memory_order_acquire)) {
        return {event::kind::wake};
      }
      if (::WaitForSingleObject(event_.get(), INFINITE) == WAIT_FAILED) {
        return {event::kind::failed, nullptr,
                std::error_code(static_cast<int>(::GetLastError()),
                                std::system_category())};
      }
    }
  }

  // The flag is the memory and the event is only the nudge: the ring
  // signals the same event on every completion, so a wake whose signal a
  // completion's wait consumed is still seen by the next take_wake().
  [[nodiscard]] bool take_wake() noexcept {
    return woken_.exchange(false, std::memory_order_acquire);
  }
  void wake() noexcept {
    woken_.store(true, std::memory_order_release);
    (void)::SetEvent(event_.get());
  }

  // There is no handle-wide cancel here: BuildIoRingCancelRequest names
  // one request by its userData, and nothing keeps a list of them, so a
  // closing file's requests are waited out.
  template <class Part>
  void cancel_file(Part&, std::size_t) noexcept {}
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return ring_owed_; }

  void begin_drain() noexcept {
#if defined(BLAKE3PP_HAS_IORING_API)
    if (use_ring_ && queued_ > 0) {
      (void)submit_quiet();
    }
#endif
  }
  void end_drain() noexcept {}

  template <class Op>
  void release(Op&) noexcept {}

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

 private:
  template <class Op>
  [[nodiscard]] static std::uintptr_t op_ud(Op* op) noexcept {
    return std::bit_cast<std::uintptr_t>(op);
  }
  template <class Op>
  [[nodiscard]] static Op* op_from(std::uintptr_t ud) noexcept {
    return std::bit_cast<Op*>(ud);
  }
  // The cancel request's completion needs a userData no transfer can
  // wear: reaping it as a transfer would walk to an operation that does
  // not exist. A transfer's userData is its aligned address, which is
  // never 1.
  static constexpr std::uintptr_t cancel_ud = 1;
  static_assert(alignof(win_transfer) > cancel_ud);

#if defined(BLAKE3PP_HAS_IORING_API)
  [[nodiscard]] static IORING_HANDLE_REF handle_ref(const win_file& f) noexcept {
    return IoRingHandleRefFromHandle(f.h());
  }

  // Queues one read or write of `len` bytes from where the transfer
  // stopped. False means the ring refused the entry, and the transfer
  // goes to the pump instead: a counted transfer has to reach its
  // callback whatever the ring does.
  template <class Op>
  [[nodiscard]] bool build(Op& op, std::uint32_t len) noexcept {
    const ioring_api& api = ioring_entry_points();
    const IORING_HANDLE_REF h = handle_ref(op.owner_file->part().f);
    const std::uint64_t at = op.position();
    HRESULT hr = 0;
    if (op.is_write) {
      void* const from = const_cast<std::byte*>(op.write_remaining().data());
      hr = api.build_write(ring_.get(), h, IoRingBufferRefFromPointer(from),
                           len, at, FILE_WRITE_FLAGS_NONE, op_ud(&op),
                           IOSQE_FLAGS_NONE);
    } else {
      hr = api.build_read(ring_.get(), h,
                          IoRingBufferRefFromPointer(op.read_remaining().data()),
                          len, at, op_ud(&op), IOSQE_FLAGS_NONE);
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
  // take_failure(). A built entry cannot be taken back out of the ring,
  // and a wait does not submit, so a submit that succeeds but leaves
  // entries behind is asked once more and then latched as a failure too:
  // the context must not wait on entries the ring was never given.
  [[nodiscard]] bool submit_quiet() noexcept {
    for (int attempt = 0; attempt < 2; ++attempt) {
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
      if (queued_ == 0) {
        // The entries a failed submit left in the ring are offered again
        // by the next one, so a success is what settles the mechanism
        // question.
        submit_failed_ = 0;
        return true;
      }
    }
    if (submit_failed_ == 0) {
      submit_failed_ =
          static_cast<std::uint32_t>(HRESULT_FROM_WIN32(ERROR_RETRY));
    }
    return false;
  }
#else
  // Without the SDK's declarations there is no ring: use_ring_ is false
  // for the life of the engine, so place() takes nothing.
  template <class Op>
  [[nodiscard]] static bool build(Op&, std::uint32_t) noexcept {
    return false;
  }
#endif  // BLAKE3PP_HAS_IORING_API

  // Declaration order is the teardown contract: the ring is closed before
  // the event it was given. Do not move ring_ above event_.
  unique_handle event_;
#if defined(BLAKE3PP_HAS_IORING_API)
  ioring ring_;
#else
  ioring_refusal refusal_{};
#endif
  std::size_t queued_ = 0;     // entries built and not yet submitted
  std::size_t ring_owed_ = 0;  // requests the ring has yet to hand back
  std::uint32_t submit_failed_ = 0;  // latched for take_failure()
  unsigned queue_depth_ = 1;
  bool async_requested_ = true;
  bool use_ring_ = false;
  std::atomic<bool> woken_{false};
};

using ioring_context = completion_context<ioring_engine>;

// Definition-site conformance check (see uring_backend.hpp): fails here,
// with the missed requirement named, the first time this header is
// compiled, before any engine instantiation exists.
static_assert(io_context<ioring_context>);

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
