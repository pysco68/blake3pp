#pragma once

// The portable contract between polled_reader/polled_writer and the
// per-OS contexts. Reads and writes share it: an io_context owns the
// completion mechanism, a file is opened against it, and every transfer
// is named by a caller-owned operation whose callback reports it.
//
// Three rules span every operation:
//  - Teardown drain: a context's destructor drains every in-flight
//    operation, running no callback. A file's destructor that closes with
//    transfers owed does the same for its own, through forget().
//    polled_reader and polled_writer declare their buffer pool member
//    before the context, so the drain runs before the pool is freed.
//  - Operation lifetime: an operation must outlive the drain of its file
//    and of its context. A lane or a dispatch worker may be finishing a
//    transfer until that drain returns, and it writes the result into the
//    operation; a queued operation is read when a lane takes it. Destroy
//    the file (or call drain()/forget()) first, then the operations.
//  - Callback discipline: a transfer's callback runs only inside poll(),
//    on the thread that called poll(). Never from submit_read(),
//    submit_write(), flush(), wake() or a destructor, and never on a lane
//    or another backend thread. The pipelines (several windows, then
//    several files, driven as senders from one thread) depend on there
//    being exactly one place where caller code regains control.
//    forget(file&) is the one exception, and only for a sibling's
//    transfer: where the completion mechanism is shared (one ring, one
//    port, one kqueue), waiting out this file's transfers can reap
//    another file's, and that completion is reported so its operation is
//    not stranded. It runs on the polling thread, from a call the pipeline
//    makes while its own cells are alive. A file destructor that calls
//    forget() can therefore run a sibling's callback.
// Internal to src/io/, never installed.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <blake3pp/detail/driver.hpp>

#include "io/transfer.hpp"

namespace blake3pp::detail::io_impl {

// The completion-callback I/O contract.
//
// A context owns the completion mechanism (a ring, a port, a queue) and
// nothing about any particular file. C::file is an open file bound to a
// context: constructed from (C&, const std::filesystem::path&,
// const file_open_options&) and exposing size() and name(). A source that
// has no path constructs its file differently -- the tests' fake_context
// takes a size -- which is why construction is described here rather than
// required below.
//
// name() is where the degradation ladder becomes visible: it is settled
// when the file is opened, against the context that opened it, and
// file_reader::backend() reports it verbatim.
//
// Lifetime: every C::file must be destroyed before its context, and the
// context's destructor drains every in-flight transfer without running
// callbacks. Every operation must outlive both drains: the file's, which
// its destructor runs when transfers are owed, and the context's. The
// member order of polled_reader and polled_writer enforces all three.
//
// Errors reach the callback as std::error_code:
//  - errno values in the generic category;
//  - Windows errors in the system category;
//  - a cancel as operation_canceled, an unexpected EOF as EIO.
// poll() throws only when the mechanism itself fails, never for a
// transfer's own error.
//
// A transfer that in_flight() counts must reach its callback, so a
// refused submission is a degradation, not an exception: the context
// moves the transfer to its deferred list, where a lane performs it
// (poll() does, when no lane runs) and poll() reports the result. An
// exception at that point would leave the transfer counted, uncompleted
// and unreachable.
template <class C>
concept io_context = requires(C c, const C cc, typename C::file& f,
                              typename C::transfer& op, std::uint64_t off,
                              std::span<std::byte> buf,
                              std::span<const std::byte> cbuf, bool block,
                              std::uint64_t bytes) {
  typename C::file;
  typename C::transfer;
  requires std::derived_from<typename C::transfer, transfer_base>;
  requires std::default_initializable<typename C::transfer>;
  // The file_reader::backend() string, computed when the file was opened
  // and owned by it: polled_reader hands it straight to file_reader.
  { f.name() } noexcept -> std::same_as<std::string_view>;
  // The file's length as the open found it, which polled_reader,
  // polled_writer and the driver read to plan and to bound every
  // transfer.
  { f.size() } noexcept -> std::same_as<std::uint64_t>;
  // Queues one read of exactly buf.size() bytes at off. Performs no
  // transfer, runs no callback and cannot fail. A transfer the
  // asynchronous engine cannot take (no engine, a length NO_BUFFERING
  // cannot take on Windows, a zero length) goes on the context's deferred
  // list, where a lane reads it (poll() does, when no lane runs).
  { c.submit_read(f, off, buf, op) } noexcept;
  // Queues one write of exactly cbuf.size() bytes at off, under the same
  // rules as submit_read. The caller's bytes must stay put until the
  // callback runs.
  { c.submit_write(f, off, cbuf, op) } noexcept;
  // Sets the file's size, for trimming a preallocation back to what was
  // actually written. Only meaningful on a file opened for writing.
  { f.set_size(bytes) };
  // Pushes everything queued to the OS in one call. Never throws: what
  // the OS refuses moves to the deferred list, still owed.
  { c.flush() } noexcept;
  // Flushes, reaps completions, reissues short transfers, runs the
  // callbacks of every transfer the engine or the lanes finished, and
  // performs at most one deferred transfer itself when no lane runs.
  // With block == true, sleeps until at least one callback ran or wake()
  // was called. Returns the number of callbacks run.
  { c.poll(block) } -> std::same_as<std::size_t>;
  // The only member callable from another thread: makes a blocked poll()
  // return. The caller's other threads use it to get the polling thread
  // back, and the lanes use it to report a finished transfer.
  // Idempotent; a wake with no blocked poll is remembered.
  { c.wake() } noexcept;
  { cc.in_flight() } noexcept -> std::same_as<std::size_t>;
  // Waits out every transfer the kernel, a lane or a dispatch worker
  // holds and forgets the rest, running no callback: afterwards
  // in_flight() is zero and the context references no transfer, so the
  // caller may destroy the operations. Legal only while those operations
  // are alive. The context stays usable, and its destructor's drain is
  // then a no-op. Never throws.
  { c.drain() } noexcept;
  // The same for one file: afterwards the context references no transfer
  // of this file and in_flight() counts none of them, so the file and the
  // buffers its transfers named may be destroyed. Legal only while those
  // operations are alive. Runs no callback of this file's. A sibling
  // file's completion that surfaces during the wait is reported, or left
  // for a later poll(), and never dropped. Every file destructor calls
  // this.
  { c.forget(f) } noexcept;
  // Asks the OS to abandon one transfer. Advisory, and never a
  // completion in itself: the op is still owed its callback and still
  // counted by in_flight(), and the callback arrives from a later poll().
  // It reports whichever happened first: operation_canceled if the
  // cancel won, the data if the transfer did, or the transfer's own
  // error. A transfer a lane has already taken runs to completion.
  //
  // Legal only from the thread that calls poll(), and only for an op
  // that was submitted and whose callback has not run. A backend with no
  // cancel mechanism may do nothing: the transfer runs to completion and
  // reports normally.
  { c.cancel(op) } noexcept;
  // The most transfers this context can have at the device at once: the
  // caller's depth where an asynchronous engine took them, and otherwise
  // the number of lanes serving the deferred list (one on the completion
  // port's synchronous rung). Never zero. diagnose_read() and
  // diagnose_write() read the device's queue against it, so a rung that
  // serves two transfers at a time is not reported as a full queue of
  // eight.
  { cc.device_depth() } noexcept -> std::same_as<unsigned>;
  // Nanoseconds transfers spent waiting for a lane before they started,
  // with none of the device in it: the measure of too few lanes, as
  // opposed to a device that is the wall. Zero on a context that served
  // everything through its asynchronous engine.
  { cc.lane_wait() } noexcept -> std::same_as<std::chrono::nanoseconds>;
  // What the device itself took over this run, stamped by whoever
  // performed each transfer: a lane or a dispatch worker around its own
  // pread, a kernel queue from submission to completion. A wait for a
  // lane is never part of it. Zero transfers means this context cannot
  // say.
  { cc.device() } noexcept -> std::same_as<device_time>;
};

}  // namespace blake3pp::detail::io_impl
