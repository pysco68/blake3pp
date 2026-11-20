#pragma once

// The Windows backend: CreateFileW handles (std::filesystem::path's native
// wide string is the whole reason the public API trades in paths) and an
// I/O completion port as the completion queue. The mapping to the io_uring
// backend is nearly 1:1: one OVERLAPPED per transfer plays the SQE,
// GetQueuedCompletionStatus plays the completion-queue wait, and
// FILE_FLAG_NO_BUFFERING is O_DIRECT (same sector-alignment demands, a
// buffered handle for the unaligned tail). Degrades per feature at
// runtime: no overlapped handle -> synchronous ReadFile/WriteFile on the
// deferred pump's lanes, NO_BUFFERING refused -> buffered. Internal to
// src/io/, never installed.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <string_view>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include "io/completion_context.hpp"
#include "io/io_context.hpp"
#include "io/ladder.hpp"
#include "io/options.hpp"
#include "io/windows/file.hpp"

namespace blake3pp::detail::io_impl {

#if defined(BLAKE3PP_TEST_IO_SEAMS)
// The test executable compiles this backend with seams and under a name
// of its own: the library's copy, which the same executable links, stays
// one definition, and nothing shipped carries the seams.
inline namespace test_seams {
// While positive, each read the port is handed is issued at half its
// length, and the count drops: the completion comes back short, as one
// may, and the rest goes through the port again.
inline std::atomic<unsigned> short_port_reads{0};
// While positive, each read the port is handed is refused, as ReadFile
// refuses one for want of resources, and the count drops.
inline std::atomic<unsigned> refuse_port_reads{0};

[[nodiscard]] inline bool take_seam(std::atomic<unsigned>& count) noexcept {
  unsigned n = count.load();
  while (n != 0 && !count.compare_exchange_weak(n, n - 1)) {
  }
  return n != 0;
}
#endif

// The completion-port engine under completion_context: one port, any
// number of files, each associating its overlapped handle with it at
// open. The port is also the wake primitive, so a context whose
// transfers all go to the pump still has exactly one thing a blocked
// poll sleeps on.
//
// FILE_SKIP_COMPLETION_PORT_ON_SUCCESS is NOT set. With it, a read that
// completes synchronously returns without posting, and its callback
// would have to run inside submit_read() -- exactly what the contract
// forbids. Leaving it unset costs a post per fast completion and buys one
// path: every read, fast or slow, reports from poll().
class iocp_engine {
 public:
  struct transfer_part : win_transfer {
    OVERLAPPED ov{};
  };

  class file_part {
   public:
    file_part(const iocp_engine& e, const std::filesystem::path& path,
              const file_open_options& how) {
      f.open_file(path, how);
      f.engage(path.c_str(), how.direct_io, e.async_requested_, e.port_.get());
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

  iocp_engine(const reader_context_options& opts, unsigned max_inflight)
      : async_requested_(opts.async),
        port_(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)),
        queue_depth_(std::max(1u, max_inflight)) {
    if (!port_) {
      throw_winerr("CreateIoCompletionPort");
    }
  }
  iocp_engine(const iocp_engine&) = delete;
  iocp_engine& operator=(const iocp_engine&) = delete;

  static constexpr io_engine rung_engine = io_engine::iocp;
  // What failed, as the shared context's messages name it.
  [[nodiscard]] std::string_view mechanism() const noexcept {
    return "the completion port";
  }
  using rung_below = win_sync_rung;

  // What a file needs from the engine to open against it.
  [[nodiscard]] bool async_requested() const noexcept { return async_requested_; }
  [[nodiscard]] HANDLE port() const noexcept { return port_.get(); }

  // A constructed engine has a port, because the constructor throws
  // otherwise. This answers the synchronous rung only for a caller who
  // asked for no async, which says nothing about the machine, so that
  // verdict is unsettled.
  [[nodiscard]] rung_verdict engaged_rung() const noexcept {
    return async_requested_ ? rung_verdict{rung_engine, true}
                            : rung_verdict{rung_below::rung_engine, false};
  }
  [[nodiscard]] bool engaged() const noexcept { return true; }

  // The port's depth where any transfer of this run rode it, and one
  // otherwise. A file whose reopen never engaged sends everything to the
  // pump, whose lanes read through one non-overlapped handle. More lanes
  // measured no faster there.
  [[nodiscard]] unsigned device_depth(bool used, unsigned) const noexcept {
    return used ? queue_depth_ : 1u;
  }

  template <class Op>
  static std::error_code perform(Op& op) noexcept {
    return win_perform_rest(op);
  }

  // The rest of a transfer, from op.filled, on the port: all of it for a
  // buffered overlapped handle, the whole granules of it at a granule
  // offset for a direct one. The rest (the sub-granule end of a file, or
  // a resumption off a boundary) goes to a lane, on the plain handle,
  // which is not overlapped. A zero-length transfer goes to a lane too,
  // because it would post a completion carrying zero bytes, the same
  // shape as a short transfer. A ReadFile or WriteFile refused outright
  // owes no completion, so it also goes to a lane.
  template <class Op>
  [[nodiscard]] bool place(Op& op) noexcept {
    const win_file& f = op.owner_file->part().f;
    std::size_t len = op.remaining();
    if (!f.overlapped || len == 0) {
      return false;
    }
    len = std::min(len, max_win_piece);
    const std::uint64_t at = op.position();
    if (f.direct) {
      len -= len % direct_align;
      if (len == 0 || at % direct_align != 0) {
        return false;
      }
    }
    return issue(op, f.h(), at, static_cast<DWORD>(len));
  }

  // CancelIoEx names one request by its OVERLAPPED, so a sibling read on
  // the same handle is untouched. It fails with ERROR_NOT_FOUND for a
  // request that has already finished, which is not an error here: that
  // completion is already on the port and arrives normally.
  template <class Op>
  void cancel(Op& op) noexcept {
    ::CancelIoEx(op.owner_file->part().f.h(), &op.ov);
  }

  // ReadFile queues the request itself, so there is nothing batched to
  // push and nothing to take back.
  template <class Op, class Refused>
  void flush(Refused&&) noexcept {}
  template <class Op, class Refused>
  void take_back(Refused&&) noexcept {}
  [[nodiscard]] std::error_code take_failure() noexcept { return {}; }

  // One step of the port. The error is read before anything else runs,
  // since any call -- a clock read included -- may overwrite it, and the
  // key is read only for a dequeue that succeeded with no OVERLAPPED,
  // which is the wake.
  template <class Op>
  [[nodiscard]] engine_event<Op> next(bool block) noexcept {
    using event = engine_event<Op>;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* pov = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(port_.get(), &bytes, &key,
                                                &pov, block ? INFINITE : 0);
    const DWORD err = ok != 0 ? ERROR_SUCCESS : ::GetLastError();
    if (pov != nullptr) {
      --port_owed_;
      Op* const op = static_cast<Op*>(CONTAINING_RECORD(pov, transfer_part, ov));
      return {event::kind::completion, op,
              err != ERROR_SUCCESS ? read_error(err) : std::error_code{},
              bytes};
    }
    if (ok == 0 && err == WAIT_TIMEOUT) {
      return {};
    }
    if (ok != 0 && key == wake_key) {
      return {event::kind::wake};
    }
    return {event::kind::failed, nullptr,
            std::error_code(static_cast<int>(err), std::system_category())};
  }

  // Posts a sentinel the port hands back with a null OVERLAPPED, which is
  // how a blocked GetQueuedCompletionStatus is ended from another thread
  // or from a lane reporting a finished transfer.
  void wake() noexcept {
    ::PostQueuedCompletionStatus(port_.get(), 0, wake_key, nullptr);
  }

  // CancelIoEx bounds a closing file's wait: every cancelled request
  // completes, successfully or with ERROR_OPERATION_ABORTED.
  template <class Part>
  void cancel_file(Part& p, std::size_t owed) noexcept {
    if (p.f.overlapped && owed > 0) {
      ::CancelIoEx(p.f.h(), nullptr);
    }
  }
  template <class Op, class Pred>
  [[nodiscard]] std::size_t drop_unsubmitted(Pred&&) noexcept {
    return 0;
  }
  [[nodiscard]] std::size_t kernel_owed() const noexcept { return port_owed_; }

  void begin_drain() noexcept {}
  void end_drain() noexcept {}

  template <class Op>
  void release(Op&) noexcept {}

  // The degradation strings are observable API: tools print them and a
  // run is read differently depending on which rung it names, so the
  // spellings and suffixes are fixed.
  [[nodiscard]] std::string name_for(bool direct, bool overlapped,
                                     bool write) const {
    const char* const sync_name = write ? "writefile" : "readfile";
    if (overlapped) {
      return direct ? "iocp+direct" : "iocp";
    }
    return direct ? std::string(sync_name) + "+direct" : std::string(sync_name);
  }

 private:
  static constexpr ULONG_PTR wake_key = ~ULONG_PTR{0};

  // Hands the port `len` bytes from `at`. False when ReadFile or
  // WriteFile refused it outright, which owes no completion; the charge
  // ends there, so a lane's own charge does not take in the wait for the
  // lane.
  template <class Op>
  [[nodiscard]] bool issue(Op& op, HANDLE h, std::uint64_t at,
                           DWORD len) noexcept {
    std::memset(&op.ov, 0, sizeof(op.ov));
    op.ov.Offset = static_cast<DWORD>(at);
    op.ov.OffsetHigh = static_cast<DWORD>(at >> 32);
#if defined(BLAKE3PP_TEST_IO_SEAMS)
    if (!op.is_write && take_seam(refuse_port_reads)) {
      return false;
    }
    if (!op.is_write && len > 2 * direct_align && take_seam(short_port_reads)) {
      len = static_cast<DWORD>(len / 2 - len / 2 % direct_align);
    }
#endif
    // The piece is the kernel's from here, and charged to the device
    // from here: a cached read completes inside the call.
    op.device_begin();
    const BOOL ok =
        op.is_write
            ? ::WriteFile(h, op.write_remaining().data(), len, nullptr, &op.ov)
            : ::ReadFile(h, op.read_remaining().data(), len, nullptr, &op.ov);
    if (ok == 0 && ::GetLastError() != ERROR_IO_PENDING) {
      op.device_end();
      return false;
    }
    ++port_owed_;
    return true;
  }

  bool async_requested_ = true;
  unique_handle port_;
  std::size_t port_owed_ = 0;  // requests the port has yet to hand back
  unsigned queue_depth_ = 1;
};

using iocp_context = completion_context<iocp_engine>;

// Definition-site conformance check (see linux/io_uring.hpp): fails here,
// with the missed requirement named, the first time MSVC compiles this
// header, before any engine instantiation exists.
static_assert(io_context<iocp_context>);

// The longest read name iocp_engine::name_for() gives. A port that stands
// in for a refused ring adds the ring's reason (windows/engine.hpp).
inline constexpr std::size_t iocp_read_name_max =
    longest_of({"iocp+direct", "readfile+direct"});
static_assert(iocp_read_name_max <= max_read_backend_length,
              "a read backend name would not fit driver_stats::read_backend");

#if defined(BLAKE3PP_TEST_IO_SEAMS)
}  // inline namespace test_seams
#endif

}  // namespace blake3pp::detail::io_impl

#endif  // _WIN32
