#pragma once

// The compiled seam between the file pipeline and the platform's reader
// context (src/io/io_context.hpp). Standard library only: the pipeline is a
// header that callers compile, and the backends are not something they
// should see, so everything platform-shaped lives behind a pimpl here
// and the native context's contract is mirrored member for member.
//
// The one addition over that contract is reserve(): the driver owns the
// buffer memory, because the teardown rule -- drain every read BEFORE
// freeing what it reads into -- is easy to get wrong at a distance and
// belongs in compiled code next to the drain itself.
//
// Internal to the library. Never installed as public API, and never
// included by <blake3pp/io.hpp>: it names no execution provider, and
// nothing here should drag one in.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

#include <blake3pp/detail/driver.hpp>

namespace blake3pp::detail {

// What the driver is allowed to try. Per file settings (direct I/O) are
// the file's, not the driver's: one driver serves several files.
struct io_driver_options {
  bool async = true;           // use the OS's async engine at all
  bool offload_submit = true;  // io_uring: issue on io-wq (IOSQE_ASYNC)
  // Windows: prefer the I/O ring (Windows 11) over the completion port
  // where the machine has one; off selects the port. The engaged rung is
  // in file::name().
  bool ioring = true;
};


class io_driver {
 public:
  // The granule this driver's direct-I/O path needs, which is the
  // portable one; io_driver.cpp checks it against the backends' own.
  static constexpr std::size_t direct_alignment = detail::direct_alignment;

  io_driver(const io_driver_options& opts, unsigned max_inflight);
  ~io_driver();
  io_driver(const io_driver&) = delete;
  io_driver& operator=(const io_driver&) = delete;

  // How a file is opened: reading or writing, direct or buffered, and
  // for a write how much to preallocate. Preallocation belongs to the
  // open because a write that extends a file serializes on the inode
  // lock, which collapses an async direct queue to one write at a time.
  struct open_options {
    bool write = false;
    bool direct_io = false;
    std::uint64_t preallocate_bytes = 0;
    // See file_write_options::waive_zero_fill.
    bool waive_zero_fill = false;
  };

  // An open file bound to a driver. Destroy every file before its
  // driver: the driver's drain protects the memory these are reading
  // into. Destroy the operations submitted on a file after the file, or
  // after drain(file&): a file closing with transfers owed drains them
  // and may write into those operations until it returns.
  class file {
   public:
    file(io_driver& drv, const std::filesystem::path& path,
         const open_options& how);
    // A read-only open, as the pipeline's callers spell it.
    file(io_driver& drv, const std::filesystem::path& path, bool direct_io)
        : file(drv, path, open_options{.direct_io = direct_io}) {}
    ~file();
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept;
    // What file_reader::backend() reports for this file.
    [[nodiscard]] std::string_view name() const noexcept;
    // Trims a preallocation back to what was actually written.
    void set_size(std::uint64_t bytes);

   private:
    friend class io_driver;
    struct impl;
    std::unique_ptr<impl> impl_;
  };

  // Queues one read of exactly buf.size() bytes. No syscall, no
  // callback. A window the file cannot take asynchronously is read
  // synchronously by the backend's own I/O threads.
  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   io_op& op);
  // The write side of the same contract: no syscall, no callback, and a
  // length the platform cannot take asynchronously is written off the
  // polling thread like an unalignable read is.
  void submit_write(file& f, std::uint64_t off, std::span<const std::byte> buf,
                    io_op& op);
  // Pushes everything queued to the OS in one call. A read the OS
  // refuses is not lost: it is read synchronously off the polling
  // thread, which is why nothing here reports a submit failure.
  void flush() noexcept;
  // Reaps completions, reissues short reads, and runs the callbacks of
  // every op that finished -- including the ones the backend's deferred
  // threads performed. Flushes first. With block, sleeps until at least
  // one callback ran or wake() was called. Returns callbacks run.
  std::size_t poll(bool block);
  // The only member another thread may call.
  void wake() noexcept;
  // Asks the OS to abandon one transfer. Advisory, and never a completion in
  // itself: the op is still owed its callback and still counted by
  // in_flight(), and the only way it can arrive is a later poll(). What
  // the callback reports is whichever happened first --
  // std::errc::operation_canceled if the cancel won, the data if the
  // read did, some other error if the read failed on its own.
  //
  // Legal only from the thread that calls poll(), and only for an op
  // that was submitted and whose callback has not run. A platform with
  // no cancel mechanism does nothing and the read runs to completion.
  void cancel(io_op& op) noexcept;
  [[nodiscard]] std::size_t in_flight() const noexcept;
  // The most transfers the engine behind this driver can have at the
  // device at once: the depth it was built with where an asynchronous
  // engine took them, and on the synchronous rung the I/O threads serving
  // it -- one on Windows, whose threads share one non-overlapped handle
  // and were measured to gain nothing over one. What the diagnosis reads
  // the device's queue against.
  [[nodiscard]] unsigned device_depth() const noexcept;
  // What one finished transfer cost: the device's own time, stamped by
  // whoever performed it rather than inferred from a span of the
  // pipeline's timeline, and how long it waited for one of the driver's
  // threads first. Only from inside that transfer's completion.
  [[nodiscard]] transfer_service service_of(const io_op& op) const noexcept;
  // Waits out every read still owed, running no callback, and forgets
  // the rest. The invariant it establishes: afterwards in_flight() is
  // zero, the driver references no io_op, and every op it owed has
  // `live` lowered, so the caller may destroy the ops it submitted or
  // sweep them with cancel() and meet nothing. The driver stays usable.
  // The destructor drains too, a no-op after this.
  void drain() noexcept;
  // drain() for one file. Afterwards no transfer of this file is owed or
  // referenced and its operations have `live` lowered, so its buffers and
  // the operations naming them may be destroyed. The driver stays usable
  // for this file and every other.
  //
  // A sibling file's completion that surfaces during the wait is reported
  // normally, because dropping it would strand an op that can never
  // complete. A caller closing one file of several should idle it first.
  //
  // The file's destructor does NOT do this: a file destroyed with
  // transfers still owed drains the WHOLE context, the backends' last
  // resort, which strands every sibling's owed transfer. Either way the
  // operations must outlive the drain, because the backend's own threads
  // may write into one until it returns. Destroy the operations only after
  // this call or after the file.
  void drain(file& f) noexcept;

  // Takes the window memory: one direct-I/O-aligned arena of
  // windows * window_bytes, valid until the driver is destroyed. The
  // destructor drains first, so no read is ever in flight into freed
  // memory. Exactly one call per driver, made by the window pool at
  // construction; a second call is a contract violation that debug builds
  // assert on. There is no free(). Several pipelines may run over one
  // arena at once, each borrowing windows from that pool.
  void reserve(unsigned windows, std::size_t window_bytes);

  // The slice of that arena window `slot` reads into, and the same bytes
  // as the view the pipeline reads back. The view names the arena from
  // here, but what is IN it is the driver's until the read completes.
  [[nodiscard]] window_storage window_storage_for(file& f, unsigned slot,
                                                  std::uint64_t off,
                                                  std::size_t bytes) noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

static_assert(file_driver<io_driver>);

}  // namespace blake3pp::detail
