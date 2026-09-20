#pragma once

// The compiled seam between the file pipeline and the platform's reader
// context (src/io/backend.hpp). Standard library only: the pipeline is a
// header that callers compile, and the backends are not something they
// should see, so everything platform-shaped lives behind a pimpl here
// and the native context's contract is mirrored member for member.
//
// The one addition over that contract is allocate(): the driver owns the
// buffer memory, because the teardown rule -- drain every read BEFORE
// freeing what it reads into -- is easy to get wrong at a distance and
// belongs in compiled code next to the drain itself.
//
// Internal to the library. Never installed as public API, and never
// included by <blake3pp/io.hpp>: it names no execution provider, and
// nothing here should drag one in.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace blake3pp::detail {

// What the driver is allowed to try. Per file settings (direct I/O) are
// the file's, not the driver's: one driver serves several files.
struct io_driver_options {
  bool async = true;           // use the OS's async engine at all
  bool offload_submit = true;  // io_uring: issue on io-wq (IOSQE_ASYNC)
};

// A read in flight. Caller-owned and at a fixed address from submit_read
// until its callback has run.
//
// The backend's own operation lives in `storage`: sized and aligned here
// for the widest backend, checked against the real type by a
// static_assert in io_driver.cpp, so a backend that outgrows it fails the
// build rather than corrupting the neighbouring bytes. One io_read_op
// serves read after read, each submit constructing a fresh backend op
// over the last one's bytes, which is why that type has to be trivially
// destructible -- also checked there.
struct io_read_op {
  void (*done)(io_read_op*, std::error_code) noexcept = nullptr;
  void* owner = nullptr;

  static constexpr std::size_t storage_size = 160;
  static constexpr std::size_t storage_align = 16;
  alignas(storage_align) std::byte storage[storage_size];
};

// Size and alignment of the platform backend's own read operation: what
// io_read_op::storage has to hold. The static_assert in io_driver.cpp is
// the one that sees the real type, so this reports the margin on
// platforms whose backend nobody here can compile.
struct io_read_op_layout {
  std::size_t size;
  std::size_t align;
};

[[nodiscard]] io_read_op_layout native_read_op_layout() noexcept;

class io_driver {
 public:
  io_driver(const io_driver_options& opts, unsigned max_inflight);
  ~io_driver();
  io_driver(const io_driver&) = delete;
  io_driver& operator=(const io_driver&) = delete;

  // An open file bound to a driver. Destroy every file before its
  // driver: the driver's drain protects the memory these are reading
  // into.
  class file {
   public:
    file(io_driver& drv, const std::filesystem::path& path, bool direct_io);
    ~file();
    file(const file&) = delete;
    file& operator=(const file&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept;
    // What file_reader::backend() reports for this file.
    [[nodiscard]] std::string_view name() const noexcept;

   private:
    friend class io_driver;
    struct impl;
    std::unique_ptr<impl> impl_;
  };

  // Queues one read of exactly buf.size() bytes. No syscall, no
  // callback. A window the file cannot take asynchronously is served
  // synchronously inside poll(), as it is in the backends.
  void submit_read(file& f, std::uint64_t off, std::span<std::byte> buf,
                   io_read_op& op);
  // Pushes everything queued to the OS in one call.
  void flush();
  // Reaps completions, reissues short reads, performs at most one
  // deferred synchronous read, and runs the callbacks of every op that
  // finished. Flushes first. With block, sleeps until at least one
  // callback ran or wake() was called. Returns callbacks run.
  std::size_t poll(bool block);
  // The only member another thread may call.
  void wake() noexcept;
  [[nodiscard]] std::size_t in_flight() const noexcept;

  // Direct-I/O-aligned memory owned by the driver, valid until the
  // driver is destroyed -- which drains first, so no read is ever in
  // flight into freed memory. One call, at pipeline construction; there
  // is no free().
  [[nodiscard]] std::span<std::byte> allocate(std::size_t bytes);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

// What the pipeline needs of whatever is feeding it. io_driver models
// it; so does the bench's driver over the null source, which is how the
// pipeline gets measured with no device under it.
template <class D>
concept file_driver =
    requires(D& d, const D& cd, typename D::file& f, io_read_op& op,
             std::uint64_t off, std::span<std::byte> buf, bool block,
             std::size_t bytes) {
      typename D::file;
      { f.size() } noexcept -> std::same_as<std::uint64_t>;
      { f.name() } noexcept -> std::same_as<std::string_view>;
      { d.submit_read(f, off, buf, op) };
      { d.flush() };
      { d.poll(block) } -> std::same_as<std::size_t>;
      { d.wake() } noexcept;
      { cd.in_flight() } noexcept -> std::same_as<std::size_t>;
      { d.allocate(bytes) } -> std::same_as<std::span<std::byte>>;
    };

static_assert(file_driver<io_driver>);

}  // namespace blake3pp::detail
