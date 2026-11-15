// The completion-callback reader contract (src/io/io_context.hpp): what a
// context promises polled_reader and polled_writer, tested on the contexts
// themselves rather than through file_reader.
//
// The invariant the pipeline rests on is where a callback runs: inside
// poll(), on the thread that called it, and nowhere else. Most of what
// follows checks that from several directions: after submit, after
// flush, from a second thread, and from a context that is being
// destroyed with reads still owed.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <doctest/doctest.h>

#include "temp_names.hpp"
#include "io/io_context.hpp"
#include "io/lanes.hpp"
#include "io/options.hpp"
#include "io/sync_context.hpp"
#include "io/transfer.hpp"
#include "io/waiter.hpp"
#include "io/polled_io.hpp"
#include "io/portable/stdio.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include "io/posix/pread.hpp"
#endif
#if defined(__linux__)
#include <unistd.h>  // getpid, for a temporary name under /dev/shm

#include "io/linux/io_uring.hpp"
#endif
#if defined(__FreeBSD__)
#include <aio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "io/freebsd/aio.hpp"
#endif
#if defined(_WIN32)
#include "io/windows/iocp.hpp"
#include "io/windows/ioring.hpp"
#endif
#if defined(__APPLE__)
#include "io/macos/dispatch.hpp"
#endif

namespace {

namespace fs = std::filesystem;
namespace io_impl = blake3pp::detail::io_impl;

std::vector<std::byte> pattern(std::size_t len, std::uint32_t seed) {
  std::vector<std::byte> v(len);
  std::uint32_t x = seed;
  for (auto& b : v) {
    x = x * 1'664'525u + 1'013'904'223u;
    b = static_cast<std::byte>(x >> 24);
  }
  return v;
}

struct temp_file {
  fs::path path;
  explicit temp_file(const std::vector<std::byte>& content) {
    path = fs::temp_directory_path() /
           ("blake3pp_ctx_test_" + b3test::process_salt() + "_" +
            std::to_string(counter++));
    std::string chars(content.size(), '\0');
    std::ranges::transform(content, chars.begin(),
                           [](std::byte b) { return static_cast<char>(b); });
    std::ofstream out(path, std::ios::binary);
    out.write(chars.data(), static_cast<std::streamsize>(chars.size()));
  }
  ~temp_file() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  static inline int counter = 0;
};

// One counted completion. `owner` points here, which is the only thing
// the contract says that field is for. The thread id is recorded because
// "callbacks run inside poll()" means on the caller's thread: a backend
// whose transfers finish on its own workers (GCD) or on lanes hands the
// completions over to poll() instead of calling from there.
struct probe {
  int calls = 0;
  std::error_code ec{};
  std::thread::id ran_on{};
  static void on_done(io_impl::transfer_base* base,
                      std::error_code e) noexcept {
    auto* const p = static_cast<probe*>(base->owner);
    p->calls++;
    p->ec = e;
    p->ran_on = std::this_thread::get_id();
  }
};

// A context that misbehaves in every way the contract tolerates: it fills
// buffers in pieces, finishes operations out of the order they were
// submitted, and fails a chosen one. It still calls each op's callback
// exactly once and only from poll(), which is what the engine above it is
// entitled to assume.
class fake_context {
 public:
  struct transfer : io_impl::transfer_base {
    std::span<std::byte> buf{};
    std::uint64_t off = 0;
    std::size_t filled = 0;
    bool done_once = false;
    bool cancelled = false;
  };

  class file {
   public:
    file(fake_context&, std::uint64_t size) noexcept : size_(size) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "fake"; }
    void set_size(std::uint64_t bytes) noexcept { size_ = bytes; }

   private:
    std::uint64_t size_ = 0;
  };

  fake_context(const io_impl::reader_context_options&, unsigned) noexcept {}

  [[nodiscard]] bool async() const noexcept { return true; }

  void submit_read(file&, std::uint64_t off, std::span<std::byte> buf,
                   transfer& op) noexcept {
    op.buf = buf;
    op.off = off;
    op.filled = 0;
    op.done_once = false;
    op.cancelled = false;
    queue_.push_back(&op);
  }
  void flush() noexcept {}

  // The engine under test never writes; the contract still asks, so this
  // records the write and completes it like any other operation.
  void submit_write(file&, std::uint64_t off, std::span<const std::byte> buf,
                    transfer& op) noexcept {
    op.buf = {};
    op.off = off;
    op.filled = buf.size();
    op.done_once = false;
    op.cancelled = false;
    queue_.push_back(&op);
  }

  void cancel(transfer& op) noexcept { op.cancelled = true; }

  std::size_t poll(bool) {
    if (queue_.empty()) {
      return 0;
    }
    // Out of order: the newest submission finishes first.
    transfer& op = *queue_.back();
    queue_.pop_back();
    if (op.off == fail_offset) {
      op.done_once = true;
      op.done(&op, std::error_code(EIO, std::generic_category()));
      return 1;
    }
    if (op.cancelled) {
      op.done_once = true;
      op.done(&op, std::make_error_code(std::errc::operation_canceled));
      return 1;
    }
    // In pieces, a third at a time, but only one callback at the end.
    while (op.filled < op.buf.size()) {
      const std::size_t n =
          std::min(op.buf.size() - op.filled, op.buf.size() / 3 + 1);
      for (std::size_t i = 0; i < n; ++i) {
        op.buf[op.filled + i] =
            static_cast<std::byte>((op.off + op.filled + i) & 0xff);
      }
      op.filled += n;
    }
    op.done_once = true;
    // Attributed like a real performer's, so the engine over this context
    // sees a device time rather than a hole.
    device_.add(1);
    op.done(&op, {});
    return 1;
  }

  void wake() noexcept { waiter_.wake(); }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return queue_.size();
  }
  void drain() noexcept { queue_.clear(); }
  // Nothing is ever in the OS's hands here, and the cases build one file
  // per context, so forgetting a file's transfers is forgetting the queue.
  void forget(file&) noexcept { queue_.clear(); }

  // This context performs its transfers on the thread that polls it and
  // queues nothing anywhere else: one at the device, no wait for a lane,
  // and the transfer itself stamped like any other performer's.
  [[nodiscard]] unsigned device_depth() const noexcept { return 1; }
  [[nodiscard]] std::uint64_t lane_wait_ns() const noexcept { return 0; }
  [[nodiscard]] blake3pp::detail::device_time device() const noexcept {
    return device_;
  }

  // The engine gives no way to reach its context, so the
  // window to fail is named before the engine is built. A static on a
  // type that exists only in this file is the smallest thing that can
  // say it.
  static inline std::uint64_t fail_offset = ~std::uint64_t{0};

 private:
  std::vector<transfer*> queue_;
  io_impl::poll_waiter waiter_;
  blake3pp::detail::device_time device_{};
};

static_assert(io_impl::io_context<fake_context>);

// submit_read() then flush() must produce no callback at all; poll() must
// produce every one of them.
template <class C, class MakeFile>
void check_callback_discipline(std::string_view tag, MakeFile make_file) {
  CAPTURE(tag);
  C ctx({}, 4);
  auto f = make_file(ctx);
  std::array<std::byte, 8192> a{};
  std::array<std::byte, 8192> b{};
  typename C::transfer op_a{};
  typename C::transfer op_b{};
  probe pa;
  probe pb;
  op_a.done = &probe::on_done;
  op_a.owner = &pa;
  op_b.done = &probe::on_done;
  op_b.owner = &pb;

  ctx.submit_read(f, 0, std::span{a}, op_a);
  ctx.submit_read(f, a.size(), std::span{b}, op_b);
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  ctx.flush();
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(ctx.in_flight() == 2);

  std::size_t ran = 0;
  while (pa.calls + pb.calls < 2) {
    ran += ctx.poll(true);
  }
  CHECK(ran == 2);
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  CHECK(!pa.ec);
  CHECK(!pb.ec);
  CHECK(ctx.in_flight() == 0);
  // On the thread that called poll(), not on whatever thread the backend
  // happens to complete on.
  CHECK(pa.ran_on == std::this_thread::get_id());
  CHECK(pb.ran_on == std::this_thread::get_id());
}

// cancel() is advisory: what a read reports afterwards is whichever
// happened first, and that differs per backend and per run. What must
// hold everywhere is the discipline around it -- one callback, from
// poll(), an error that is operation_canceled and nothing else, a
// neighbouring read untouched, and a context still usable.
template <class C, class MakeFile>
void check_cancel_discipline(std::string_view tag, MakeFile make_file,
                             std::span<const std::byte> content) {
  CAPTURE(tag);
  C ctx({}, 4);
  auto f = make_file(ctx);
  std::array<std::byte, 8192> a{};
  std::array<std::byte, 8192> b{};
  typename C::transfer op_a{};
  typename C::transfer op_b{};
  probe pa;
  probe pb;
  op_a.done = &probe::on_done;
  op_a.owner = &pa;
  op_b.done = &probe::on_done;
  op_b.owner = &pb;

  ctx.submit_read(f, 0, std::span{a}, op_a);
  ctx.submit_read(f, a.size(), std::span{b}, op_b);
  ctx.flush();
  ctx.cancel(op_a);
  // Still owed: a cancel completes nothing by itself.
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(ctx.in_flight() == 2);

  while (pa.calls + pb.calls < 2) {
    ctx.poll(true);
  }
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  CHECK(pa.ran_on == std::this_thread::get_id());
  CHECK(ctx.in_flight() == 0);
  // Either the cancel won or the read did; no third answer.
  if (pa.ec) {
    CHECK(pa.ec == std::errc::operation_canceled);
  }
  const std::string verdict =
      pa.ec ? "the cancel took the read" : "the read won the race";
  MESSAGE(tag << ": " << verdict);
  // The read nobody cancelled is untouched, which is the part a
  // whole-handle cancel would get wrong. An empty expectation is the
  // null source, which completes reads without filling anything.
  CHECK(!pb.ec);
  if (!content.empty()) {
    CHECK(std::ranges::equal(b, content.subspan(a.size(), b.size())));
  }

  // A cancel leaves the context usable, like drain() does.
  typename C::transfer op_c{};
  probe pc;
  op_c.done = &probe::on_done;
  op_c.owner = &pc;
  std::array<std::byte, 8192> c{};
  ctx.submit_read(f, 0, std::span{c}, op_c);
  while (pc.calls == 0) {
    ctx.poll(true);
  }
  CHECK(pc.calls == 1);
  CHECK(!pc.ec);
}

// The write half of the same contract: submit queues, flush pushes,
// poll reports, and the bytes that land are the bytes that were handed
// over. Lengths either side of the direct-I/O granule are the point --
// an unaligned one ends in a piece the direct descriptor cannot take.
template <class C, class MakeFile>
void check_write_discipline(std::string_view tag, const fs::path& path,
                            bool direct, std::size_t len, MakeFile make_file) {
  CAPTURE(tag);
  CAPTURE(direct);
  CAPTURE(len);
  const auto content = pattern(len, static_cast<std::uint32_t>(len + 1));
  {
    C ctx({}, 4);
    auto f = make_file(ctx);
    typename C::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;

    // A direct write takes a buffer aligned as the engines' arena is, so
    // the test hands it the same kind of buffer.
    const auto buf = io_impl::make_aligned_buffer(std::max<std::size_t>(len, 1));
    std::ranges::copy(content, buf.get());
    ctx.submit_write(f, 0, std::span<const std::byte>{buf.get(), len}, op);
    CHECK(p.calls == 0);
    ctx.flush();
    CHECK(p.calls == 0);
    if (len > 0) {
      CHECK(ctx.in_flight() == 1);
    }
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    CHECK(p.ran_on == std::this_thread::get_id());
    CHECK(ctx.in_flight() == 0);
    f.set_size(len);
  }
  // What landed is what was submitted, byte for byte.
  std::ifstream in(path, std::ios::binary);
  std::vector<char> got((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  REQUIRE(got.size() == len);
  // memcmp's arguments are non-null even for a zero length, and an empty
  // vector's data() is not.
  if (len > 0) {
    CHECK(std::memcmp(got.data(), content.data(), len) == 0);
  }
}

}  // namespace

TEST_SUITE("io_context") {

TEST_CASE("callbacks run only inside poll") {
  const auto content = pattern(64 * 1024, 7);
  const temp_file f(content);
#if defined(__linux__)
  check_callback_discipline<io_impl::uring_context>(
      "uring", [&](io_impl::uring_context& c) {
        return io_impl::uring_context::file(c, f.path, io_impl::file_open_options{});
      });
#endif
#if defined(__FreeBSD__)
  check_callback_discipline<io_impl::aio_context>(
      "aio", [&](io_impl::aio_context& c) {
        return io_impl::aio_context::file(c, f.path,
                                          io_impl::file_open_options{});
      });
#endif
#if defined(__unix__) || defined(__APPLE__)
  check_callback_discipline<io_impl::pread_context>(
      "pread", [&](io_impl::pread_context& c) {
        return io_impl::pread_context::file(c, f.path, io_impl::file_open_options{});
      });
#endif
#if defined(_WIN32)
  check_callback_discipline<io_impl::iocp_context>(
      "iocp", [&](io_impl::iocp_context& c) {
        return io_impl::iocp_context::file(c, f.path, io_impl::file_open_options{});
      });
  check_callback_discipline<io_impl::ioring_context>(
      "ioring", [&](io_impl::ioring_context& c) {
        return io_impl::ioring_context::file(c, f.path,
                                             io_impl::file_open_options{});
      });
#endif
#if defined(__APPLE__)
  check_callback_discipline<io_impl::gcd_context>(
      "gcd", [&](io_impl::gcd_context& c) {
        return io_impl::gcd_context::file(c, f.path, io_impl::file_open_options{});
      });
#endif
  check_callback_discipline<io_impl::stdio_context>(
      "stdio", [&](io_impl::stdio_context& c) {
        return io_impl::stdio_context::file(c, f.path, io_impl::file_open_options{});
      });
}

TEST_CASE("a cancelled read still reports exactly once, from poll") {
  const auto content = pattern(64 * 1024, 11);
  const temp_file f(content);
  const std::span<const std::byte> expect{content};
#if defined(__linux__)
  check_cancel_discipline<io_impl::uring_context>(
      "uring",
      [&](io_impl::uring_context& c) {
        return io_impl::uring_context::file(c, f.path, io_impl::file_open_options{});
      },
      expect);
#endif
#if defined(__FreeBSD__)
  check_cancel_discipline<io_impl::aio_context>(
      "aio",
      [&](io_impl::aio_context& c) {
        return io_impl::aio_context::file(c, f.path,
                                          io_impl::file_open_options{});
      },
      expect);
#endif
#if defined(__unix__) || defined(__APPLE__)
  check_cancel_discipline<io_impl::pread_context>(
      "pread",
      [&](io_impl::pread_context& c) {
        return io_impl::pread_context::file(c, f.path, io_impl::file_open_options{});
      },
      expect);
#endif
#if defined(_WIN32)
  check_cancel_discipline<io_impl::iocp_context>(
      "iocp",
      [&](io_impl::iocp_context& c) {
        return io_impl::iocp_context::file(c, f.path, io_impl::file_open_options{});
      },
      expect);
  check_cancel_discipline<io_impl::ioring_context>(
      "ioring",
      [&](io_impl::ioring_context& c) {
        return io_impl::ioring_context::file(c, f.path,
                                             io_impl::file_open_options{});
      },
      expect);
#endif
#if defined(__APPLE__)
  // A read a dispatch worker already has cannot be recalled: it runs to
  // completion and reports its data, which is the contract's other legal
  // answer. Only a transfer still queued for a lane is dropped.
  check_cancel_discipline<io_impl::gcd_context>(
      "gcd",
      [&](io_impl::gcd_context& c) {
        return io_impl::gcd_context::file(c, f.path, io_impl::file_open_options{});
      },
      expect);
#endif
  check_cancel_discipline<io_impl::stdio_context>(
      "stdio",
      [&](io_impl::stdio_context& c) {
        return io_impl::stdio_context::file(c, f.path, io_impl::file_open_options{});
      },
      expect);
}

#if defined(_WIN32)
// The IOCP half of the unaligned-length contract: a buffered file hands
// the port any length, so none of it is synchronous; a NO_BUFFERING file
// hands it the whole granules and finishes the sub-granule rest on the
// plain handle, which is one synchronous piece per such file rather than
// the whole read.
TEST_CASE("IOCP: an unaligned length rides the port but for a direct tail") {
  namespace io_impl = blake3pp::detail::io_impl;
  constexpr std::size_t len = 2 * 4096 + 100;
  const auto content = pattern(len, 43);
  const temp_file f(content);
  for (const bool direct : {false, true}) {
    CAPTURE(direct);
    io_impl::iocp_context ctx({}, 4);
    io_impl::iocp_context::file file(
        ctx, f.path, io_impl::file_open_options{.direct_io = direct});
    MESSAGE("read through " << file.name());
    const bool on_port = file.name().starts_with("iocp");
    const bool bypassing = file.name().find("direct") != std::string_view::npos;
    std::vector<std::byte> got(len);
    probe p;
    io_impl::iocp_context::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span(got), op);
    ctx.flush();
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(!p.ec);
    CHECK(got == content);
    if (on_port) {
      CHECK(ctx.served_synchronously() == (bypassing ? 1u : 0u));
    }
  }
}

// The IoRing half of the unaligned-length contract: the same question the
// port answers above, with the same two answers. A buffered overlapped
// handle takes any length, so none of it is synchronous; a NO_BUFFERING
// one takes the whole granules and the sub-granule rest finishes on the
// plain handle, one synchronous piece per such file.
TEST_CASE("IoRing: an unaligned length rides the ring but for a direct tail") {
  namespace io_impl = blake3pp::detail::io_impl;
  constexpr std::size_t len = 2 * 4096 + 100;
  const auto content = pattern(len, 44);
  const temp_file f(content);
  for (const bool direct : {false, true}) {
    CAPTURE(direct);
    io_impl::ioring_context ctx({}, 4);
    io_impl::ioring_context::file file(
        ctx, f.path, io_impl::file_open_options{.direct_io = direct});
    MESSAGE("read through " << file.name());
    const bool on_ring = file.name().starts_with("ioring");
    const bool bypassing = file.name().find("direct") != std::string_view::npos;
    std::vector<std::byte> got(len);
    probe p;
    io_impl::ioring_context::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span(got), op);
    ctx.flush();
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(!p.ec);
    CHECK(got == content);
    if (on_ring) {
      CHECK(ctx.served_synchronously() == (bypassing ? 1u : 0u));
    }
  }
}

// poll() returns the callbacks it ran. A direct file whose length is not
// a whole number of granules sends its tail to the pump after the first
// piece comes back, and moving that remainder runs no callback: over the
// whole read, the returns add up to the one callback, on the ring and on
// the port.
TEST_CASE("Windows: poll() counts callbacks, not a remainder moved to the pump") {
  namespace io_impl = blake3pp::detail::io_impl;
  constexpr std::size_t len = 64 * 4096 + 100;
  const auto content = pattern(len, 46);
  const temp_file f(content);
  const auto check = [&](auto& ctx) {
    using ctx_t = std::remove_cvref_t<decltype(ctx)>;
    typename ctx_t::file file(ctx, f.path,
                              io_impl::file_open_options{.direct_io = true});
    MESSAGE("read through " << file.name());
    std::vector<std::byte> got(len);
    probe p;
    typename ctx_t::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span(got), op);
    ctx.flush();
    std::size_t reported = 0;
    for (std::size_t polls = 0; p.calls == 0; ++polls) {
      REQUIRE(polls < 1000);
      reported += ctx.poll(true);
    }
    CHECK(!p.ec);
    CHECK(got == content);
    CHECK(reported == p.calls);
  };
  {
    io_impl::ioring_context ctx({}, 4);
    check(ctx);
  }
  {
    io_impl::iocp_context ctx({}, 4);
    check(ctx);
  }
}

// A read the port refuses goes to a lane, whether it is a transfer's first
// piece or the rest of one that came back short, and its device time is
// what the port and the lane each had: a refusal ends the port's charge,
// so the wait for a lane is in neither, and the transfer is still
// counted. The seams make the port's first read short and refuse the one
// after it.
TEST_CASE("IOCP: a refused read goes to a lane and keeps its device time") {
  namespace io_impl = blake3pp::detail::io_impl;
  constexpr std::size_t len = 1024 * 1024;
  const auto content = pattern(len, 47);
  const temp_file f(content);
  struct seams_off {
    ~seams_off() {
      io_impl::short_port_reads.store(0);
      io_impl::refuse_port_reads.store(0);
    }
  } const reset;
  for (const bool first_piece : {false, true}) {
    CAPTURE(first_piece);
    io_impl::iocp_context ctx({}, 4);
    io_impl::iocp_context::file file(ctx, f.path, io_impl::file_open_options{});
    if (file.name() != "iocp") {
      MESSAGE("skipped: no overlapped handle here (" << file.name() << ")");
      return;
    }
    std::vector<std::byte> got(len);
    probe p;
    io_impl::iocp_context::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    if (first_piece) {
      io_impl::refuse_port_reads.store(1);
    } else {
      io_impl::short_port_reads.store(1);
    }
    ctx.submit_read(file, 0, std::span(got), op);
    ctx.flush();
    // The rest of the short piece is routed from poll(), and refused there.
    io_impl::refuse_port_reads.store(1);
    for (std::size_t polls = 0; p.calls == 0; ++polls) {
      REQUIRE(polls < 1000);
      (void)ctx.poll(true);
    }
    io_impl::refuse_port_reads.store(0);
    CHECK(!p.ec);
    CHECK(got == content);
    CHECK(ctx.served_synchronously() == 1);
    CHECK(ctx.device().transfers == 1);
  }
}

// A cancel on the ring is a submission of its own, and its completion
// carries a userData that belongs to no transfer. Reaped as one it would
// walk to an operation that does not exist, so it has to be dropped: the
// cancelled read reports exactly once, the uncancelled one reports once,
// and no later poll() produces a third callback. The canary is a transfer
// that was never submitted -- if a sentinel completion were walked as an
// operation, this is the shape of memory it would land in.
TEST_CASE("IoRing: the cancel request's completion reports no operation") {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = pattern(256 * 1024, 45);
  const temp_file f(content);
  io_impl::ioring_context ctx({}, 4);
  io_impl::ioring_context::file file(ctx, f.path,
                                     io_impl::file_open_options{});
  std::vector<std::byte> a(128 * 1024);
  std::vector<std::byte> b(128 * 1024);
  probe pa;
  probe pb;
  probe canary;
  io_impl::ioring_context::transfer op_a{};
  io_impl::ioring_context::transfer op_b{};
  io_impl::ioring_context::transfer never{};  // submitted to nothing
  op_a.done = &probe::on_done;
  op_a.owner = &pa;
  op_b.done = &probe::on_done;
  op_b.owner = &pb;
  never.done = &probe::on_done;
  never.owner = &canary;

  ctx.submit_read(file, 0, std::span{a}, op_a);
  ctx.submit_read(file, a.size(), std::span{b}, op_b);
  ctx.flush();
  ctx.cancel(op_a);
  while (pa.calls + pb.calls < 2) {
    ctx.poll(true);
  }
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  CHECK(ctx.in_flight() == 0);
  // The cancel's own completion is somewhere in this sweep, and none of
  // these polls may turn it into a callback.
  for (int i = 0; i < 8; ++i) {
    CHECK(ctx.poll(false) == 0);
  }
  CHECK(pa.calls == 1);
  CHECK(pb.calls == 1);
  CHECK(canary.calls == 0);
}

// The reasons a ring could not be had are part of the ladder a tool
// prints, and the machine that runs this suite is usually one that HAS a
// ring, so the spellings are asserted on the composer rather than waited
// for. A Windows 10 host reaches them through name() instead.
TEST_CASE("IoRing: a refused ring names the reason") {
  namespace io_impl = blake3pp::detail::io_impl;
  using refusal = io_impl::ioring_refusal;
  CHECK(io_impl::no_ioring_suffix(refusal{}).empty());
  CHECK(io_impl::no_ioring_suffix({refusal::kind::no_api, 0, 0}) ==
        " [ioring: no api]");
  CHECK(io_impl::no_ioring_suffix({refusal::kind::old_version, 1, 0}) ==
        " [ioring: version 1]");
  CHECK(io_impl::no_ioring_suffix({refusal::kind::no_read_write, 3, 0}) ==
        " [ioring: no read/write ops]");
  CHECK(io_impl::no_ioring_suffix(
            {refusal::kind::create_failed, 0, 0x80070057}) ==
        " [ioring: CreateIoRing failed 0x80070057]");
}
#endif

#if defined(__APPLE__)
// Closing a file's descriptor does not wait for the dispatch workers
// reading through it, so a file destroyed with a transfer in flight lets
// a worker read into memory its owner has released -- or, once the
// number is recycled, read a different file through it and report
// success. The per-file drain has to wait them out first, so after the
// destructor the transfer has completed into the buffer and none of it
// is reported. The read is large enough that it cannot have finished in
// the microseconds the destructor itself takes, so a missing drain fails
// here rather than by luck.
TEST_CASE("a GCD file waits out its workers before its descriptor closes") {
  const auto content = pattern(32 * 1024 * 1024, 71);
  const temp_file f(content);
  io_impl::gcd_context ctx({}, 4);
  std::vector<std::byte> buf(content.size());
  io_impl::gcd_context::transfer op{};
  probe p;
  op.done = &probe::on_done;
  op.owner = &p;
  {
    io_impl::gcd_context::file file(ctx, f.path, io_impl::file_open_options{});
    ctx.submit_read(file, 0, std::span{buf}, op);
    ctx.flush();
    CHECK(ctx.in_flight() == 1);
  }
  // The worker ran to completion through a descriptor that was still
  // open, and its result was dropped rather than delivered.
  CHECK(!op.result());
  CHECK(buf == content);
  CHECK(ctx.in_flight() == 0);
  CHECK(ctx.poll(false) == 0);
  CHECK(p.calls == 0);
}

// What a closing file waits for is its own: one that owes nothing closes
// at once, however long a sibling's transfer is held in a worker. The
// seam holds the dispatch group as such a worker does.
TEST_CASE("a GCD file that owes nothing closes without waiting on a sibling") {
  const auto content = pattern(64 * 1024, 72);
  const temp_file f(content);
  io_impl::gcd_context ctx({}, 4);
  if (!ctx.engine().test_uses_gcd()) {
    MESSAGE("skipped: no dispatch group here");
    return;
  }
  ctx.engine().test_hold_group();
  std::atomic<bool> closed{false};
  std::thread closer([&] {
    {
      io_impl::gcd_context::file idle(ctx, f.path, io_impl::file_open_options{});
    }
    closed.store(true);
  });
  for (int i = 0; i < 1000 && !closed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool closed_while_held = closed.load();
  ctx.engine().test_release_group();
  closer.join();
  CHECK(closed_while_held);
}

// A file that does owe a transfer waits for that one to land, and not for
// a sibling's worker as well.
TEST_CASE("a GCD file closing with a transfer in flight ignores a sibling's") {
  const auto content = pattern(64 * 1024, 73);
  const temp_file f(content);
  io_impl::gcd_context ctx({}, 4);
  if (!ctx.engine().test_uses_gcd()) {
    MESSAGE("skipped: no dispatch group here");
    return;
  }
  std::vector<std::byte> buf(content.size());
  io_impl::gcd_context::transfer op{};
  probe p;
  op.done = &probe::on_done;
  op.owner = &p;
  ctx.engine().test_hold_group();
  std::atomic<bool> closed{false};
  std::thread closer([&] {
    {
      io_impl::gcd_context::file busy(ctx, f.path,
                                      io_impl::file_open_options{});
      ctx.submit_read(busy, 0, std::span{buf}, op);
      ctx.flush();
    }
    closed.store(true);
  });
  for (int i = 0; i < 1000 && !closed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool closed_while_held = closed.load();
  ctx.engine().test_release_group();
  closer.join();
  CHECK(closed_while_held);
  CHECK(buf == content);
  CHECK(ctx.in_flight() == 0);
  CHECK(p.calls == 0);
}
#endif

TEST_CASE("a write reports only from poll, and lands what it was given") {
  // Every length in [0, 2 blocks] at the interesting boundaries: the
  // direct-I/O path can only take whole granules, so the rest go down
  // the backend's deferred ladder and must still arrive.
  const std::vector<std::size_t> lens{0,
                                      1,
                                      4095,
                                      4096,
                                      4097,
                                      8191,
                                      8192};
  for (const bool direct : {false, true}) {
    for (const std::size_t len : lens) {
      const temp_file out({});
#if defined(__linux__)
      check_write_discipline<io_impl::uring_context>(
          "uring", out.path, direct, len,
          [&](io_impl::uring_context& c) {
            return io_impl::uring_context::file(
                c, out.path,
                io_impl::file_open_options{.write = true,
                                           .direct_io = direct,
                                           .preallocate_bytes = len});
          });
#endif
#if defined(__FreeBSD__)
      check_write_discipline<io_impl::aio_context>(
          "aio", out.path, direct, len,
          [&](io_impl::aio_context& c) {
            return io_impl::aio_context::file(
                c, out.path,
                io_impl::file_open_options{.write = true,
                                           .direct_io = direct,
                                           .preallocate_bytes = len});
          });
#endif
#if defined(__unix__) || defined(__APPLE__)
      check_write_discipline<io_impl::pread_context>(
          "pread", out.path, direct, len,
          [&](io_impl::pread_context& c) {
            return io_impl::pread_context::file(
                c, out.path,
                io_impl::file_open_options{.write = true, .direct_io = direct});
          });
#endif
#if defined(_WIN32)
      check_write_discipline<io_impl::ioring_context>(
          "ioring", out.path, direct, len,
          [&](io_impl::ioring_context& c) {
            return io_impl::ioring_context::file(
                c, out.path,
                io_impl::file_open_options{.write = true,
                                           .direct_io = direct,
                                           .preallocate_bytes = len});
          });
#endif
      check_write_discipline<io_impl::stdio_context>(
          "stdio", out.path, /*direct=*/false, len,
          [&](io_impl::stdio_context& c) {
            return io_impl::stdio_context::file(
                c, out.path, io_impl::file_open_options{.write = true});
          });
    }
  }
}

// A read of nothing has the same ambiguity a write of nothing does:
// io_uring reports res == 0, which is how it reports a short transfer
// with nothing left to give. Neither may reach the kernel.
TEST_CASE("an empty transfer completes from poll, not as an error") {
  const auto content = pattern(8192, 21);
  const temp_file f(content);
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});
    io_impl::uring_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, {}, op);
    CHECK(p.calls == 0);
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    CHECK(ctx.in_flight() == 0);
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    io_impl::aio_context::file file(ctx, f.path, io_impl::file_open_options{});
    io_impl::aio_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, {}, op);
    CHECK(p.calls == 0);
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    CHECK(ctx.in_flight() == 0);
  }
#endif
  {
    io_impl::stdio_context ctx({}, 4);
    io_impl::stdio_context::file file(ctx, f.path, io_impl::file_open_options{});
    io_impl::stdio_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, {}, op);
    CHECK(p.calls == 0);
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
  }
}

#if defined(__linux__)
// Preallocation is a hint. A filesystem that refuses it must leave a
// usable file behind, not a failed open: the writes extend the
// file as they land, which is what every backend without fallocate does
// anyway. Forced here by asking for more than the filesystem holds.
TEST_CASE("a refused preallocation degrades to an ordinary writable open") {
  const fs::path out = fs::path("/dev/shm") /
                       ("blake3pp_prealloc_" + std::to_string(::getpid()));
  const auto content = pattern(64 * 1024, 22);
  {
    io_impl::uring_context ctx({}, 4);
    // Far more than /dev/shm holds: fallocate answers ENOSPC.
    io_impl::uring_context::file file(
        ctx, out,
        io_impl::file_open_options{.write = true,
                                   .direct_io = false,
                                   .preallocate_bytes = std::uint64_t{1} << 48});
    io_impl::uring_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_write(file, 0, std::span<const std::byte>{content}, op);
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    file.set_size(content.size());
  }
  std::ifstream in(out, std::ios::binary);
  std::vector<char> got((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  CHECK(got.size() == content.size());
  CHECK(std::memcmp(got.data(), content.data(), content.size()) == 0);
  std::error_code ec;
  fs::remove(out, ec);
}
#endif

TEST_CASE("the engine survives a context that finishes in pieces and out of order") {
  constexpr std::uint64_t size = 300 * 1024 + 77;
  io_impl::polled_reader<fake_context> r(
      size, {.window_bytes = 64 * 1024, .queue_depth = 3});
  CHECK(r.file_size() == size);
  CHECK(r.backend_name() == "fake");
  std::vector<std::byte> out;
  while (auto w = r.next()) {
    out.insert(out.end(), w->bytes.begin(), w->bytes.end());
    r.release(*w);
  }
  REQUIRE(out.size() == size);
  // The fake writes offset-derived bytes, so order is checkable.
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] != static_cast<std::byte>(i & 0xff)) {
      FAIL("window bytes out of order at " << i);
    }
  }
}

TEST_CASE("a read that fails reaches the caller as an exception, once") {
  constexpr std::uint64_t size = 256 * 1024;
  constexpr std::size_t window = 64 * 1024;
  fake_context::fail_offset = 2 * window;  // the third window
  io_impl::polled_reader<fake_context> r(
      size, {.window_bytes = window, .queue_depth = 2});
  std::uint64_t delivered = 0;
  CHECK_THROWS_AS(
      [&] {
        while (auto w = r.next()) {
          delivered++;
          r.release(*w);
        }
      }(),
      std::system_error);
  // The windows before the failure were delivered normally; the failing
  // one stopped the stream where it was.
  CHECK(delivered == 2);
  fake_context::fail_offset = ~std::uint64_t{0};
}

TEST_CASE("two files share one context") {
  const auto a = pattern(96 * 1024, 11);
  const auto b = pattern(96 * 1024, 29);
  const temp_file fa(a);
  const temp_file fb(b);
  const auto run = [&](auto& ctx) {
    using C = std::remove_reference_t<decltype(ctx)>;
    typename C::file file_a(ctx, fa.path, io_impl::file_open_options{});
    typename C::file file_b(ctx, fb.path, io_impl::file_open_options{});
    std::vector<std::byte> got_a(a.size());
    std::vector<std::byte> got_b(b.size());
    typename C::transfer op_a{};
    typename C::transfer op_b{};
    probe pa;
    probe pb;
    op_a.done = &probe::on_done;
    op_a.owner = &pa;
    op_b.done = &probe::on_done;
    op_b.owner = &pb;
    // Interleaved on purpose: the context must keep each op's file
    // straight, which is the whole point of separating them.
    ctx.submit_read(file_a, 0, std::span{got_a}, op_a);
    ctx.submit_read(file_b, 0, std::span{got_b}, op_b);
    ctx.flush();
    while (pa.calls + pb.calls < 2) {
      ctx.poll(true);
    }
    CHECK(!pa.ec);
    CHECK(!pb.ec);
    CHECK(got_a == a);
    CHECK(got_b == b);
  };
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    run(ctx);
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    run(ctx);
  }
#endif
#if defined(__unix__) || defined(__APPLE__)
  {
    io_impl::pread_context ctx({}, 4);
    run(ctx);
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    run(ctx);
  }
  {
    io_impl::ioring_context ctx({}, 4);
    run(ctx);
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    run(ctx);
  }
#endif
}

// A read the engine has taken is counted in in_flight(), and the caller
// is owed its callback whatever happens next. io_uring can refuse a
// submit, leaving the entries queued and unsent. The reads it refused
// have to reach their callback another way, or in_flight() never comes
// down and a blocking poll never returns. A working kernel cannot be
// asked to refuse, so the recovery step is driven directly.
#if defined(__linux__)
TEST_CASE("io_uring: reads the ring would not take are served by poll()") {
  constexpr std::size_t len = 16 * 1024;
  const auto content = pattern(len, 41);
  const temp_file f(content);

  io_impl::uring_context ctx({}, 4);
  io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});

  std::vector<std::byte> got_a(len / 2);
  std::vector<std::byte> got_b(len / 2);
  probe pa;
  probe pb;
  io_impl::uring_context::transfer a{};
  io_impl::uring_context::transfer b{};
  a.done = &probe::on_done;
  a.owner = &pa;
  b.done = &probe::on_done;
  b.owner = &pb;
  ctx.submit_read(file, 0, std::span(got_a), a);
  ctx.submit_read(file, len / 2, std::span(got_b), b);
  CHECK(ctx.in_flight() == 2);

  // What a refused io_uring_enter leaves behind, without one.
  ctx.defer_unsubmitted();
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(ctx.in_flight() == 2);

  // The lanes perform the deferred reads, as every read this backend
  // cannot put on the ring; each callback still runs from poll(), on this
  // thread.
  std::size_t polls = 0;
  while (pa.calls + pb.calls < 2) {
    ctx.poll(true);
    ++polls;
    REQUIRE(polls < 8);
  }
  CHECK(!pa.ec);
  CHECK(!pb.ec);
  CHECK(ctx.in_flight() == 0);
  CHECK(std::equal(got_a.begin(), got_a.end(), content.begin()));
  CHECK(std::equal(got_b.begin(), got_b.end(), content.begin() + len / 2));
}

// A refused submission moves the reads to the pump's lanes, and the
// re-arm of the wake poll is refused the same way. The sleep then has
// nothing in the kernel to wait for: it must watch the eventfd the lane
// reports through, or poll(true) never returns. The wake poll is
// consumed first, as every wake consumes it, so the re-arm is needed.
TEST_CASE("io_uring: a refused submit with the wake poll unarmed still completes") {
  constexpr std::size_t len = 32 * 1024 * 1024;
  const auto content = pattern(len, 43);
  const temp_file f(content);
  io_impl::uring_context ctx({}, 4);
  io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});
  if (file.name().rfind("io_uring", 0) != 0) {
    MESSAGE("skipped: no io_uring here (" << file.name() << ")");
    return;
  }
  std::vector<std::byte> got(len);
  probe p;
  io_impl::uring_context::transfer a{};
  a.done = &probe::on_done;
  a.owner = &p;
  ctx.wake();
  (void)ctx.poll(false);  // reaps the wake poll's completion
  ctx.submit_read(file, 0, std::span(got), a);

  struct refusal {
    refusal() { io_impl::refuse_submit.store(true); }
    ~refusal() { io_impl::refuse_submit.store(false); }
  } const refused;
  // A hang is the failure, so the poll runs on a thread the test can give
  // up on: a stuck poll(true) cannot be interrupted, and the process ends
  // with the reason instead.
  std::atomic<bool> returned{false};
  std::thread poller([&] {
    while (p.calls == 0) {
      (void)ctx.poll(true);
    }
    returned.store(true);
  });
  for (int i = 0; i < 1000 && !returned.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!returned.load()) {
    std::fprintf(stderr, "io_uring: poll(true) still blocked after 10 s with "
                         "the read on a lane\n");
    std::abort();
  }
  poller.join();
  CHECK(p.calls == 1);
  CHECK(!p.ec);
  CHECK(ctx.in_flight() == 0);
  CHECK(got == content);
}

// A transfer can still be published when the wake poll is re-armed: an
// earlier enter that accepted nothing leaves it there. If that re-arm is
// refused, the transfer has to move to the pump with the arm, not be
// taken back and dropped, or the blocking poll waits for it for ever.
TEST_CASE("io_uring: a read still published when a re-arm is refused completes") {
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 48);
  const temp_file f(content);
  io_impl::uring_context ctx({}, 4);
  io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});
  if (file.name().rfind("io_uring", 0) != 0) {
    MESSAGE("skipped: no io_uring here (" << file.name() << ")");
    return;
  }
  std::vector<std::byte> got(len);
  probe p;
  io_impl::uring_context::transfer a{};
  a.done = &probe::on_done;
  a.owner = &p;
  ctx.wake();
  (void)ctx.poll(false);  // reaps the wake poll's completion: the next block re-arms
  struct seams_off {
    ~seams_off() {
      io_impl::stall_submits.store(0);
      io_impl::refuse_submits.store(0);
    }
  } const reset;
  // The poll's own flush accepts nothing; the re-arm's flush is refused.
  io_impl::stall_submits.store(1);
  io_impl::refuse_submits.store(1);
  ctx.submit_read(file, 0, std::span(got), a);
  std::atomic<bool> returned{false};
  std::thread poller([&] {
    while (p.calls == 0) {
      (void)ctx.poll(true);
    }
    returned.store(true);
  });
  for (int i = 0; i < 1000 && !returned.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!returned.load()) {
    std::fprintf(stderr, "io_uring: poll(true) still blocked after 10 s with "
                         "the read dropped by a refused re-arm\n");
    std::abort();
  }
  poller.join();
  CHECK(p.calls == 1);
  CHECK(!p.ec);
  CHECK(got == content);
}

// An enter can succeed and take nothing, leaving the read published. A
// wait does not submit, so a poll that blocked with it there would sleep
// for ever; the flush hands it to the pump instead, and poll(true)
// returns with the read done.
TEST_CASE("io_uring: a submit that takes nothing does not leave poll waiting") {
  constexpr std::size_t len = 1024 * 1024;
  const auto content = pattern(len, 45);
  const temp_file f(content);
  io_impl::uring_context ctx({}, 4);
  io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});
  if (file.name().rfind("io_uring", 0) != 0) {
    MESSAGE("skipped: no io_uring here (" << file.name() << ")");
    return;
  }
  std::vector<std::byte> got(len);
  probe p;
  io_impl::uring_context::transfer a{};
  a.done = &probe::on_done;
  a.owner = &p;
  struct seams_off {
    ~seams_off() { io_impl::stall_submits.store(0); }
  } const reset;
  io_impl::stall_submits.store(1);
  ctx.submit_read(file, 0, std::span(got), a);
  std::atomic<bool> returned{false};
  std::thread poller([&] {
    while (p.calls == 0) {
      (void)ctx.poll(true);
    }
    returned.store(true);
  });
  for (int i = 0; i < 1000 && !returned.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!returned.load()) {
    std::fprintf(stderr, "io_uring: poll(true) still blocked after 10 s on a "
                         "read an enter took nothing of\n");
    std::abort();
  }
  poller.join();
  CHECK(p.calls == 1);
  CHECK(!p.ec);
  CHECK(got == content);
  CHECK(ctx.served_synchronously() == 1);
}

// A refusal can come before any transfer: the constructor arms the wake
// poll and submits it. The context must still build, and reads through it
// must complete.
TEST_CASE("io_uring: a context built while submits are refused still reads") {
  constexpr std::size_t len = 256 * 1024;
  const auto content = pattern(len, 44);
  const temp_file f(content);
  std::optional<io_impl::uring_context> ctx;
  {
    struct refusal {
      refusal() { io_impl::refuse_submit.store(true); }
      ~refusal() { io_impl::refuse_submit.store(false); }
    } const refused;
    CHECK_NOTHROW(ctx.emplace(io_impl::reader_context_options{}, 4u));
  }
  REQUIRE(ctx.has_value());
  io_impl::uring_context::file file(*ctx, f.path, io_impl::file_open_options{});
  std::vector<std::byte> got(len);
  probe p;
  io_impl::uring_context::transfer op{};
  op.done = &probe::on_done;
  op.owner = &p;
  ctx->submit_read(file, 0, std::span(got), op);
  ctx->flush();
  for (std::size_t polls = 0; p.calls == 0; ++polls) {
    REQUIRE(polls < 1000);
    ctx->poll(true);
  }
  CHECK(!p.ec);
  CHECK(got == content);
}

// A length that is not a multiple of the direct-I/O granule is the last
// read of almost every real file, and the whole read of a small one. It
// rides the ring in both directions: on the plain descriptor for a
// buffered file, and for a direct one as the whole granules on the direct
// descriptor followed by the rest on the plain one. None of it reaches
// the deferred path, which served_synchronously() counts.
TEST_CASE("io_uring: an unaligned length rides the ring, buffered or direct") {
  constexpr std::size_t len = 2 * 4096 + 100;
  const auto content = pattern(len, 43);
  for (const bool direct : {false, true}) {
    CAPTURE(direct);
    const temp_file f(content);
    io_impl::uring_context ctx({}, 4);
    {
      io_impl::uring_context::file file(
          ctx, f.path, io_impl::file_open_options{.direct_io = direct});
      if (!file.name().starts_with("io_uring")) {
        MESSAGE("no io_uring here (" << file.name() << "); nothing to check");
        return;
      }
      MESSAGE("read through " << file.name());
      // A direct transfer takes a buffer aligned as the engines' arena
      // is, so both directions hand the context the same kind of buffer.
      const auto got = io_impl::make_aligned_buffer(len);
      probe p;
      io_impl::uring_context::transfer op{};
      op.done = &probe::on_done;
      op.owner = &p;
      ctx.submit_read(file, 0, std::span<std::byte>{got.get(), len}, op);
      ctx.flush();
      while (p.calls == 0) {
        ctx.poll(true);
      }
      CHECK(!p.ec);
      CHECK(std::ranges::equal(std::span<const std::byte>{got.get(), len},
                               content));
      CHECK(ctx.served_synchronously() == 0);
    }
    // The write twin: the same shape lands through the same descriptors.
    const temp_file w({});
    {
      io_impl::uring_context::file file(
          ctx, w.path,
          io_impl::file_open_options{.write = true, .direct_io = direct});
      const auto buf = io_impl::make_aligned_buffer(len);
      std::ranges::copy(content, buf.get());
      probe p;
      io_impl::uring_context::transfer op{};
      op.done = &probe::on_done;
      op.owner = &p;
      ctx.submit_write(file, 0, std::span<const std::byte>{buf.get(), len}, op);
      ctx.flush();
      while (p.calls == 0) {
        ctx.poll(true);
      }
      CHECK(!p.ec);
      CHECK(ctx.served_synchronously() == 0);
      file.set_size(len);
    }
    std::ifstream in(w.path, std::ios::binary);
    std::vector<char> landed((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    REQUIRE(landed.size() == len);
    CHECK(std::memcmp(landed.data(), content.data(), len) == 0);
  }
}

TEST_CASE("io_uring: giving up the ring mid-run leaves wake() working") {
  io_impl::uring_context ctx({}, 4);
  // The armed wake rides the same queue as the reads; taking the queue
  // back drops it, and poll() has to notice and re-arm before it sleeps.
  ctx.defer_unsubmitted();

  std::atomic<bool> entered{false};
  std::size_t ran = 1;
  std::thread driver([&] {
    entered.store(true, std::memory_order_release);
    ran = ctx.poll(true);
  });
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  ctx.wake();
  driver.join();
  CHECK(ran == 0);
}
#endif

#if defined(__FreeBSD__)
// The FreeBSD twin of the io_uring case above. A transfer the engine has
// taken is counted in in_flight(), and the caller is owed its callback
// whatever happens next: lio_listio can refuse a list outright (the
// per-process AIO queue full, or closed), and the requests it refused
// have to reach their callback another way, or in_flight() never comes
// down and a blocking poll never returns. A working kernel cannot be
// asked to refuse, so the recovery step is driven directly.
TEST_CASE("aio: transfers the kernel would not take are served by poll()") {
  constexpr std::size_t len = 16 * 1024;
  const auto content = pattern(len, 41);
  const temp_file f(content);

  io_impl::aio_context ctx({}, 4);
  io_impl::aio_context::file file(ctx, f.path, io_impl::file_open_options{});

  std::vector<std::byte> got_a(len / 2);
  std::vector<std::byte> got_b(len / 2);
  probe pa;
  probe pb;
  io_impl::aio_context::transfer a{};
  io_impl::aio_context::transfer b{};
  a.done = &probe::on_done;
  a.owner = &pa;
  b.done = &probe::on_done;
  b.owner = &pb;
  ctx.submit_read(file, 0, std::span(got_a), a);
  ctx.submit_read(file, len / 2, std::span(got_b), b);
  CHECK(ctx.in_flight() == 2);

  // What a refused lio_listio leaves behind, without one.
  ctx.defer_unsubmitted();
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(ctx.in_flight() == 2);

  // The lanes perform the deferred transfers, as every transfer this
  // backend cannot queue; each callback still runs from poll(), on this
  // thread.
  std::size_t polls = 0;
  while (pa.calls + pb.calls < 2) {
    ctx.poll(true);
    ++polls;
    REQUIRE(polls < 8);
  }
  CHECK(!pa.ec);
  CHECK(!pb.ec);
  CHECK(ctx.in_flight() == 0);
  CHECK(ctx.served_synchronously() == 2);
  CHECK(std::equal(got_a.begin(), got_a.end(), content.begin()));
  CHECK(std::equal(got_b.begin(), got_b.end(), content.begin() + len / 2));
}

// lio_listio queues each entry on its own and answers EAGAIN when any
// entry was refused for room, whatever happened to the others. Here the
// per-process queue is filled with reads of an empty socket, which stay
// queued, and three slots are freed, so the context's batch of eight is
// taken in part. Every transfer must complete exactly once, with its
// bytes: an accepted entry kept pending is queued a second time.
TEST_CASE("aio: a batch the kernel takes in part completes each transfer once") {
  int sv[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  // Reads of the socket, held queued until bytes arrive. The limit is
  // whatever the kernel says it is; a queue that never fills is too
  // large to test this way.
  constexpr std::size_t cap = 8192;
  std::vector<struct aiocb> held(cap);
  std::vector<char> sink(cap);
  std::size_t queued = 0;
  for (; queued < cap; ++queued) {
    struct aiocb& cb = held[queued];
    std::memset(&cb, 0, sizeof cb);
    cb.aio_fildes = sv[0];
    cb.aio_buf = &sink[queued];
    cb.aio_nbytes = 1;
    cb.aio_sigevent.sigev_notify = SIGEV_NONE;
    if (::aio_read(&cb) != 0) {
      REQUIRE(errno == EAGAIN);
      break;
    }
  }
  // Reaps the oldest held reads after sending them a byte each, which
  // gives their slots back.
  std::size_t reaped = 0;
  const auto release_held = [&](std::size_t count) {
    const std::vector<char> bytes(count, 'x');
    REQUIRE(::write(sv[1], bytes.data(), count) ==
            static_cast<ssize_t>(count));
    for (std::size_t i = 0; i < count; ++i, ++reaped) {
      struct aiocb& cb = held[reaped];
      const struct aiocb* list[1] = {&cb};
      while (::aio_error(&cb) == EINPROGRESS) {
        (void)::aio_suspend(list, 1, nullptr);
      }
      CHECK(::aio_return(&cb) == 1);
    }
  };
  if (queued == cap || queued < 8) {
    release_held(queued);
    ::close(sv[0]);
    ::close(sv[1]);
    MESSAGE("skipped: the per-process AIO queue holds " << queued);
    return;
  }
  release_held(3);

  constexpr std::size_t n = 8;
  constexpr std::size_t piece = 4096;
  const auto content = pattern(n * piece, 43);
  const temp_file f(content);
  {
    io_impl::aio_context ctx({}, n);
    io_impl::aio_context::file file(ctx, f.path, io_impl::file_open_options{});
    std::vector<std::vector<std::byte>> got(n, std::vector<std::byte>(piece));
    std::array<probe, n> probes{};
    std::array<io_impl::aio_context::transfer, n> ops{};
    for (std::size_t i = 0; i < n; ++i) {
      ops[i].done = &probe::on_done;
      ops[i].owner = &probes[i];
      ctx.submit_read(file, i * piece, std::span(got[i]), ops[i]);
    }
    const auto calls = [&] {
      int sum = 0;
      for (const probe& p : probes) {
        sum += p.calls;
      }
      return sum;
    };
    std::size_t polls = 0;
    while (calls() < static_cast<int>(n)) {
      ctx.poll(true);
      ++polls;
      REQUIRE(polls < 1000);
    }
    // A transfer queued twice completes twice; give a second completion
    // the chance to arrive.
    for (int i = 0; i < 4; ++i) {
      ctx.poll(false);
    }
    CHECK(ctx.in_flight() == 0);
    for (std::size_t i = 0; i < n; ++i) {
      CAPTURE(i);
      CHECK(probes[i].calls == 1);
      CHECK(!probes[i].ec);
      CHECK(std::equal(got[i].begin(), got[i].end(),
                       content.begin() + static_cast<std::ptrdiff_t>(i * piece)));
    }
  }
  release_held(queued - reaped);
  ::close(sv[0]);
  ::close(sv[1]);
}

// A length that is not a multiple of the direct-I/O granule is the last
// read of almost every real file, and the whole read of a small one. It
// rides the AIO queue in both directions: on the plain descriptor for a
// buffered file, and for a direct one as the whole granules on the
// direct descriptor followed by the rest on the plain one. None of it
// reaches the deferred path, which served_synchronously() counts.
TEST_CASE("aio: an unaligned length rides the queue, buffered or direct") {
  constexpr std::size_t len = 2 * 4096 + 100;
  const auto content = pattern(len, 43);
  for (const bool direct : {false, true}) {
    CAPTURE(direct);
    const temp_file f(content);
    io_impl::aio_context ctx({}, 4);
    if (!ctx.engaged()) {
      MESSAGE("no kernel AIO here (" << ctx.engine().refusal()
                                     << "); nothing to check");
      return;
    }
    {
      io_impl::aio_context::file file(
          ctx, f.path, io_impl::file_open_options{.direct_io = direct});
      MESSAGE("read through " << file.name());
      std::vector<std::byte> got(len);
      probe p;
      io_impl::aio_context::transfer op{};
      op.done = &probe::on_done;
      op.owner = &p;
      ctx.submit_read(file, 0, std::span(got), op);
      ctx.flush();
      while (p.calls == 0) {
        ctx.poll(true);
      }
      CHECK(!p.ec);
      CHECK(got == content);
      CHECK(ctx.served_synchronously() == 0);
    }
    // The write twin: the same shape lands through the same descriptors.
    const temp_file w({});
    {
      io_impl::aio_context::file file(
          ctx, w.path,
          io_impl::file_open_options{.write = true, .direct_io = direct});
      probe p;
      io_impl::aio_context::transfer op{};
      op.done = &probe::on_done;
      op.owner = &p;
      ctx.submit_write(file, 0, std::span<const std::byte>{content}, op);
      ctx.flush();
      while (p.calls == 0) {
        ctx.poll(true);
      }
      CHECK(!p.ec);
      CHECK(ctx.served_synchronously() == 0);
      file.set_size(len);
    }
    std::ifstream in(w.path, std::ios::binary);
    std::vector<char> landed((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    REQUIRE(landed.size() == len);
    CHECK(std::memcmp(landed.data(), content.data(), len) == 0);
  }
}

// A file closed with transfers it could not hand to the kernel -- no AIO
// at all, or a request the queue refused -- has to take them with it:
// they name its descriptors and its share of the count.
TEST_CASE("an aio file closed with deferred transfers takes them with it") {
  const std::vector<std::byte> content(64 * 1024 + 100, std::byte{0x5e});
  const temp_file f(content);
  for (const bool async : {false, true}) {
    CAPTURE(async);
    io_impl::aio_context ctx({.async = async}, 4);
    std::array<std::byte, 100> buf{};
    io_impl::aio_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    {
      io_impl::aio_context::file file(
          ctx, f.path, io_impl::file_open_options{.direct_io = true});
      ctx.submit_read(file, 0, std::span{buf}, op);
      ctx.flush();
      CHECK(ctx.in_flight() == 1);
    }
    CHECK(ctx.in_flight() == 0);
    CHECK(ctx.poll(false) == 0);
    CHECK(p.calls == 0);
  }
}

// The per-process AIO queue bounds how many requests the kernel will
// hold, and the context cuts each lio_listio batch to what is still free
// rather than letting the kernel refuse one. A queue depth past the
// limit therefore still completes, just with fewer requests out at once
// -- which is the shape a host with vfs.aio.max_aio_queue_per_proc
// lowered under the engine's depth has.
TEST_CASE("aio: more transfers than the kernel queue holds still complete") {
  constexpr std::size_t len = 4096;
  constexpr unsigned count = 64;
  const auto content = pattern(len * count, 47);
  const temp_file f(content);
  io_impl::aio_context ctx({}, 4);
  io_impl::aio_context::file file(ctx, f.path, io_impl::file_open_options{});
  std::vector<std::byte> got(len * count);
  std::vector<probe> probes(count);
  std::vector<io_impl::aio_context::transfer> ops(count);
  for (unsigned i = 0; i < count; ++i) {
    ops[i].done = &probe::on_done;
    ops[i].owner = &probes[i];
    ctx.submit_read(file, std::uint64_t{i} * len,
                    std::span(got).subspan(std::size_t{i} * len, len), ops[i]);
  }
  CHECK(ctx.in_flight() == count);
  std::size_t done = 0;
  while (done < count) {
    ctx.poll(true);
    done = 0;
    for (const auto& p : probes) {
      done += static_cast<std::size_t>(p.calls);
    }
  }
  for (const auto& p : probes) {
    CHECK(p.calls == 1);
    CHECK(!p.ec);
  }
  CHECK(ctx.in_flight() == 0);
  CHECK(got == content);
}
#endif

TEST_CASE("wake() releases a blocked poll from another thread") {
  const auto check = [](auto& ctx) {
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::size_t ran = 1;
    std::thread driver([&] {
      entered.store(true, std::memory_order_release);
      ran = ctx.poll(true);
      returned.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // Nothing is in flight, so only wake() can end this.
    ctx.wake();
    driver.join();
    CHECK(returned.load(std::memory_order_acquire));
    CHECK(ran == 0);
  };
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__unix__) || defined(__APPLE__)
  {
    io_impl::pread_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    check(ctx);
  }
  {
    io_impl::ioring_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    check(ctx);
  }
#endif
}

// A wake with no poll blocked is remembered: the next blocking poll
// returns at once, with nothing run. The one after it blocks again and is
// released by a second wake, so the first wake's signal was consumed
// rather than left to end every later sleep.
TEST_CASE("a wake before a blocking poll is remembered, and only once") {
  const auto check = [](auto& ctx) {
    ctx.wake();
    CHECK(ctx.poll(true) == 0);
    std::atomic<bool> returned{false};
    std::thread driver([&] {
      (void)ctx.poll(true);
      returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(returned.load(std::memory_order_acquire));
    ctx.wake();
    driver.join();
    CHECK(returned.load(std::memory_order_acquire));
  };
  {
    io_impl::stdio_context ctx({}, 4);
    check(ctx);
  }
#if defined(__unix__) || defined(__APPLE__)
  {
    io_impl::pread_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    check(ctx);
  }
  {
    io_impl::ioring_context ctx({}, 4);
    check(ctx);
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    check(ctx);
  }
#endif
}

#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__) || \
    defined(__FreeBSD__)
// The destructor drains without completing anything into the caller's
// hands: the pool outlives the context here, so a callback running from
// teardown would be visible, and so would a kernel write into freed
// memory (which is what ASan is watching for).
TEST_CASE("a context destroyed with reads in flight runs no callback") {
#if defined(__linux__)
  using ctx_type = io_impl::uring_context;
#elif defined(_WIN32)
  using ctx_type = io_impl::iocp_context;
#elif defined(__FreeBSD__)
  using ctx_type = io_impl::aio_context;
#else
  using ctx_type = io_impl::gcd_context;
#endif
  const auto content = pattern(8 * 1024 * 1024, 3);
  const temp_file f(content);
  std::vector<std::byte> buf(content.size());
  probe p;
  // Declared before the context and the file: an operation must outlive
  // the drain that forgets it, since a backend's own threads may write
  // into it until that drain returns.
  ctx_type::transfer op{};
  op.done = &probe::on_done;
  op.owner = &p;
  {
    ctx_type ctx({}, 4);
    ctx_type::file file(ctx, f.path, io_impl::file_open_options{});
    ctx.submit_read(file, 0, std::span{buf}, op);
    ctx.flush();
    // Leave without polling: the read is owed and must be reaped by the
    // destructor, not delivered.
  }
  CHECK(p.calls == 0);
}
#endif

// drain() on a live context: the owed read is reaped and never
// reported, the counters say nothing is owed, and the context then
// serves a fresh read as if nothing had happened -- on io_uring that
// means the wake poll was reaped with the rest and re-armed.
TEST_CASE("drain() reaps owed reads without a callback and keeps the context usable") {
  const auto content = pattern(2 * 1024 * 1024, 5);
  const temp_file f(content);
  std::vector<std::byte> buf(content.size());
  const auto check = [&](auto& ctx, auto& file, std::string_view tag) {
    CAPTURE(tag);
    using ctx_type = std::remove_reference_t<decltype(ctx)>;
    probe p;
    typename ctx_type::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span{buf}, op);
    ctx.flush();
    ctx.drain();
    CHECK(p.calls == 0);
    CHECK(ctx.in_flight() == 0);

    // Still usable, and the callback discipline still holds.
    typename ctx_type::transfer again{};
    again.done = &probe::on_done;
    again.owner = &p;
    ctx.submit_read(file, 0, std::span{buf}, again);
    ctx.flush();
    CHECK(p.calls == 0);
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    CHECK(std::equal(content.begin(), content.end(), buf.begin()));
    ctx.drain();  // nothing owed: a no-op
    CHECK(ctx.in_flight() == 0);
  };
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    io_impl::uring_context::file file(ctx, f.path, io_impl::file_open_options{});
    check(ctx, file, file.name());
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    io_impl::iocp_context::file file(ctx, f.path, io_impl::file_open_options{});
    check(ctx, file, file.name());
  }
  {
    io_impl::ioring_context ctx({}, 4);
    io_impl::ioring_context::file file(ctx, f.path,
                                       io_impl::file_open_options{});
    check(ctx, file, file.name());
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    io_impl::gcd_context::file file(ctx, f.path, io_impl::file_open_options{});
    check(ctx, file, file.name());
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    io_impl::aio_context::file file(ctx, f.path, io_impl::file_open_options{});
    check(ctx, file, file.name());
  }
#endif
#if defined(__unix__) || defined(__APPLE__)
  {
    io_impl::pread_context ctx({}, 4);
    io_impl::pread_context::file file(ctx, f.path, io_impl::file_open_options{});
    check(ctx, file, "pread");
  }
#endif
}

// A wake nobody consumed is still queued when the context dies: an
// eventfd counter, a sentinel in the completion port, a signalled
// condition variable. Destruction must not wait for it and must not turn
// it into a callback.
TEST_CASE("a context destroyed with a wake pending tears down cleanly") {
  const auto check = [](auto&& make) {
    {
      auto ctx = make();
      ctx->wake();
    }
    // Twice, so a wake consumed by the destructor of one context cannot
    // hide a second that was never armed.
    {
      auto ctx = make();
      ctx->wake();
      ctx->wake();
    }
    CHECK(true);  // reaching here without a hang or a crash is the check
  };
#if defined(__linux__)
  check([] { return std::make_unique<io_impl::uring_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
#if defined(__FreeBSD__)
  check([] { return std::make_unique<io_impl::aio_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
#if defined(__unix__) || defined(__APPLE__)
  check([] { return std::make_unique<io_impl::pread_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
#if defined(_WIN32)
  check([] { return std::make_unique<io_impl::iocp_context>(
                 io_impl::reader_context_options{}, 4u); });
  check([] { return std::make_unique<io_impl::ioring_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
#if defined(__APPLE__)
  check([] { return std::make_unique<io_impl::gcd_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
}

TEST_CASE("the degradation ladder keeps its backend strings") {
  const auto content = pattern(256 * 1024, 5);
  const temp_file f(content);
  struct row {
    bool direct;
    bool async;
    bool offload;
    std::string name;
    // Windows only: which of the two completion mechanisms was asked for.
    bool ioring = true;
  };
  std::vector<row> rows;
#if defined(__linux__)
  // Whether a ring can be had is a property of the machine, not of the
  // platform: qemu-user and a seccomp policy both answer no, and the
  // fallback name carries the reason. Probe once and expect the ladder
  // that actually applies here.
  int setup_errno = 0;
  {
    io_impl::uring ring;
    if (!ring.init(4)) {
      setup_errno = ring.setup_errno;
    }
  }
  if (setup_errno == 0) {
    rows = {{true, true, true, "io_uring+direct"},
            {true, true, false, "io_uring+direct (inline submit)"},
            {true, false, true, "pread+direct"},
            {false, true, true, "io_uring"},
            {false, true, false, "io_uring (inline submit)"},
            {false, false, true, "pread"}};
  } else {
    const std::string why = io_impl::no_uring_suffix(setup_errno);
    MESSAGE("no io_uring here" << why << ": testing the fallback ladder");
    // async requested and refused carries the reason; not requested at
    // all says nothing, which is the distinction the suffix exists for.
    rows = {{true, true, true, "pread+direct" + why},
            {true, false, true, "pread+direct"},
            {false, true, true, "pread" + why},
            {false, false, true, "pread"}};
  }
#elif defined(__FreeBSD__)
  // Two independent questions, probed once each. Whether the machine has
  // AIO and a kqueue is a property of the host (the administrator can
  // close the per-process AIO queue), and the fallback name carries the
  // reason. Whether O_DIRECT is SERVED is a property of the filesystem
  // the temporary file landed on, which is what the backend's own
  // one-page probe answers at open: UFS takes it, a ZFS dataset whose
  // `direct` property forbids this shape answers EINVAL and the file
  // degrades to buffered with the suffix. Both spellings are the
  // ladder's; which one applies here is not the platform's to say.
  const io_impl::aio_context aio_probe(io_impl::reader_context_options{}, 4);
  if (aio_probe.engaged()) {
    std::string direct_rung;
    {
      io_impl::aio_context c(io_impl::reader_context_options{}, 2);
      const io_impl::aio_context::file probe_file(
          c, f.path, io_impl::file_open_options{.direct_io = true});
      direct_rung = std::string(probe_file.name());
    }
    CAPTURE(direct_rung);
    CHECK((direct_rung == "aio+direct" ||
           direct_rung == "aio [direct: EINVAL]"));
    rows = {{true, true, true, direct_rung},
            {false, true, true, "aio"},
            {true, false, true, "pread+direct"},
            {false, false, true, "pread"}};
  } else {
    const std::string why(aio_probe.engine().refusal());
    MESSAGE("no kernel AIO here" << why << ": testing the fallback ladder");
    // async requested and refused carries the reason; not requested at
    // all says nothing, which is the distinction the suffix exists for.
    rows = {{true, true, true, "pread+direct" + why},
            {true, false, true, "pread+direct"},
            {false, true, true, "pread" + why},
            {false, false, true, "pread"}};
  }
#elif defined(__APPLE__)
  // F_NOCACHE is per-fd and tolerates any alignment, so there is no
  // unaligned-tail rung: the ladder is GCD or not, cache or not.
  rows = {{true, true, true, "gcd+nocache"},
          {false, true, true, "gcd"},
          {true, false, true, "pread+nocache"},
          {false, false, true, "pread"}};
#elif defined(_WIN32)
  // NO_BUFFERING and the overlapped handle are per-open flags, so the
  // ladder is the same shape whichever mechanism carried it: what
  // engaged, and what it fell back to. Which of the two is available is a
  // property of the machine rather than of the platform -- Windows 10 and
  // some virtual images have no ring -- so it is probed once and the
  // ladder that actually applies here is the one expected.
  const io_impl::ioring_context probe(io_impl::reader_context_options{}, 4);
  if (probe.engaged()) {
    rows = {{true, true, true, "ioring+direct", true},
            {false, true, true, "ioring", true},
            {true, true, true, "iocp+direct", false},
            {false, true, true, "iocp", false},
            {true, false, true, "readfile+direct", true},
            {false, false, true, "readfile", true}};
  } else {
    // The port carries the reason the ring was refused, so a run on
    // such a machine reads as what it is rather than as one that never
    // asked.
    const std::string why =
        io_impl::no_ioring_suffix(probe.engine().refusal());
    MESSAGE("no I/O ring here (" << why << "): testing the port's ladder");
    rows = {{true, true, true, "iocp+direct" + why, true},
            {false, true, true, "iocp" + why, true},
            {true, false, true, "readfile+direct", true},
            {false, false, true, "readfile", true}};
  }
#else
  rows = {{false, false, true, ""}};
#endif
  for (const auto& r : rows) {
    if (r.name.empty()) {
      continue;
    }
    CAPTURE(r.name);
    blake3pp::detail::file_reader reader(
        f.path, {.window_bytes = 64 * 1024,
                 .queue_depth = 2,
                 .direct_io = r.direct,
                 .async = r.async,
                 .offload_submit = r.offload,
                 .ioring = r.ioring});
    CHECK(reader.backend() == r.name);
    // And the data is the same however it degraded.
    std::vector<std::byte> out;
    while (auto w = reader.next()) {
      out.insert(out.end(), w->bytes.begin(), w->bytes.end());
      reader.release(*w);
    }
    CHECK(out == content);
  }
}

// The callback must run on whichever thread called poll(), not on the
// one that submitted and not on a backend worker. Driving poll() from a
// second thread is what tells those three apart.
TEST_CASE("callbacks run on the thread that called poll, not the submitter") {
  const auto content = pattern(64 * 1024, 41);
  const temp_file f(content);
  const auto check = [&](auto& ctx, auto&& make_file) {
    auto file = make_file(ctx);
    std::vector<std::byte> buf(content.size());
    typename std::remove_reference_t<decltype(ctx)>::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span{buf}, op);
    ctx.flush();
    std::thread::id driver_id{};
    std::thread driver([&] {
      driver_id = std::this_thread::get_id();
      while (p.calls == 0) {
        ctx.poll(true);
      }
    });
    driver.join();
    CHECK(p.calls == 1);
    CHECK(!p.ec);
    CHECK(p.ran_on == driver_id);
    CHECK(p.ran_on != std::this_thread::get_id());
    CHECK(buf == content);
  };
#if defined(__linux__)
  {
    io_impl::uring_context ctx({}, 4);
    check(ctx, [&](io_impl::uring_context& c) {
      return io_impl::uring_context::file(c, f.path, io_impl::file_open_options{});
    });
  }
#endif
#if defined(__FreeBSD__)
  {
    io_impl::aio_context ctx({}, 4);
    check(ctx, [&](io_impl::aio_context& c) {
      return io_impl::aio_context::file(c, f.path,
                                        io_impl::file_open_options{});
    });
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    check(ctx, [&](io_impl::gcd_context& c) {
      return io_impl::gcd_context::file(c, f.path, io_impl::file_open_options{});
    });
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    check(ctx, [&](io_impl::iocp_context& c) {
      return io_impl::iocp_context::file(c, f.path, io_impl::file_open_options{});
    });
  }
  {
    io_impl::ioring_context ctx({}, 4);
    check(ctx, [&](io_impl::ioring_context& c) {
      return io_impl::ioring_context::file(c, f.path,
                                           io_impl::file_open_options{});
    });
  }
#endif
#if defined(__unix__) || defined(__APPLE__)
  {
    io_impl::pread_context ctx({}, 4);
    check(ctx, [&](io_impl::pread_context& c) {
      return io_impl::pread_context::file(c, f.path,
                                          io_impl::file_open_options{});
    });
  }
#endif
  {
    io_impl::stdio_context ctx({}, 4);
    check(ctx, [&](io_impl::stdio_context& c) {
      return io_impl::stdio_context::file(c, f.path,
                                          io_impl::file_open_options{});
    });
  }
}

// --- Lanes: the transfers happen off the polling thread --------------------
//
// A deferred transfer is performed on a lane, so the pipeline's own
// thread keeps admitting, reducing and retiring windows while a pread is
// in progress. A source that counts how many transfers are inside it at
// once shows the lanes overlap; the callback discipline above shows the
// callbacks still run from poll().

// A source whose read_at/write_at hold for `hold` and record the most
// transfers that were inside at once. Templated on positional so the
// serialized case is the same source with one word changed.
template <bool Positional>
class counting_source {
 public:
  static constexpr blake3pp::io_engine rung_engine =
      blake3pp::io_engine::posix_sync;
  static constexpr bool positional = Positional;

  // Shared rather than per-source: a context opens one file here, and the
  // counters have to outlive the source the context owns.
  struct shared {
    std::atomic<int> inside{0};
    std::atomic<int> most{0};
    std::atomic<int> transfers{0};
    std::chrono::nanoseconds hold{std::chrono::milliseconds(4)};
    // Set by whichever thread performed a transfer, and compared against
    // the polling thread's. Atomic because several lanes write it.
    std::atomic<bool> performed_off_the_poller{false};
    std::thread::id poller{};
    // What the source throws instead of transferring, for the result
    // encoding below. Empty transfers normally.
    std::error_code fail_with{};
    // While set, a lane that takes a transfer waits inside the source
    // before doing it. A test holds the lanes this way when what it
    // asserts depends on which transfers a lane has taken; the hold above
    // would only make the ordering likely.
    std::atomic<bool> gated{false};
    // When set, a transfer does not finish until a second lane has entered
    // the source (most >= 2), or until ten seconds pass, which sets
    // rendezvous_timed_out. Overlap is then a handshake; the hold above
    // only makes it likely.
    bool rendezvous = false;
    std::atomic<bool> rendezvous_timed_out{false};
  };
  static inline shared* live = nullptr;

  counting_source(const fs::path&, const io_impl::file_open_options&) {}

  [[nodiscard]] std::uint64_t size() const noexcept { return 1 << 20; }
  [[nodiscard]] std::string_view name() const noexcept { return "counting"; }
  void set_size(std::uint64_t) {}

  void read_at(std::uint64_t off, std::span<std::byte> buf) { work(off, buf); }
  void write_at(std::uint64_t off, std::span<const std::byte> buf) {
    work(off, {});
    (void)buf;
  }

 private:
  static void work(std::uint64_t off, std::span<std::byte> buf) {
    shared& s = *live;
    if (s.fail_with) {
      throw std::system_error(s.fail_with, "injected");
    }
    const int now = s.inside.fetch_add(1) + 1;
    int seen = s.most.load();
    while (seen < now && !s.most.compare_exchange_weak(seen, now)) {
    }
    if (std::this_thread::get_id() != s.poller) {
      s.performed_off_the_poller.store(true);
    }
    while (s.gated.load()) {
      std::this_thread::yield();
    }
    if (s.rendezvous) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (s.most.load() < 2) {
        if (std::chrono::steady_clock::now() > deadline) {
          s.rendezvous_timed_out.store(true);
          break;
        }
        std::this_thread::yield();
      }
    }
    std::this_thread::sleep_for(s.hold);
    for (std::size_t i = 0; i < buf.size(); ++i) {
      buf[i] = static_cast<std::byte>((off + i) & 0xff);
    }
    s.transfers.fetch_add(1);
    s.inside.fetch_sub(1);
  }
};

// Submits `n` transfers of `bytes` over a counting context and polls
// until every callback has run. Returns the context's own account of
// what the device and the lanes did.
template <bool Positional>
struct lane_run {
  blake3pp::detail::device_time device{};
  std::uint64_t lane_wait_ns = 0;
  unsigned device_depth = 0;
  int most_concurrent = 0;
  int completed = 0;
  std::thread::id poller{};
  bool every_callback_on_the_polling_thread = true;
  bool rendezvous_timed_out = false;
};

template <bool Positional>
lane_run<Positional> run_counting(unsigned depth, int n,
                                  std::size_t bytes = 4096,
                                  bool rendezvous = false) {
  using source = counting_source<Positional>;
  using context = io_impl::sync_context<source>;
  typename source::shared shared;
  shared.poller = std::this_thread::get_id();
  shared.rendezvous = rendezvous;
  source::live = &shared;

  context ctx({}, depth);
  typename context::file file(ctx, fs::path("ignored"), {});

  std::vector<typename context::transfer> ops(static_cast<std::size_t>(n));
  std::vector<std::vector<std::byte>> bufs(static_cast<std::size_t>(n));
  lane_run<Positional> out;
  const std::thread::id polling = std::this_thread::get_id();
  out.poller = polling;
  for (int i = 0; i < n; ++i) {
    bufs[static_cast<std::size_t>(i)].resize(bytes);
    auto& op = ops[static_cast<std::size_t>(i)];
    op.owner = &out;
    op.done = +[](io_impl::transfer_base* b, std::error_code ec) noexcept {
      auto* const r = static_cast<lane_run<Positional>*>(b->owner);
      r->completed++;
      if (std::this_thread::get_id() != r->poller) {
        r->every_callback_on_the_polling_thread = false;
      }
      if (ec) {
        r->every_callback_on_the_polling_thread = false;
      }
    };
    ctx.submit_read(file, static_cast<std::uint64_t>(i) * bytes,
                    bufs[static_cast<std::size_t>(i)], op);
  }
  ctx.flush();
  while (out.completed < n) {
    ctx.poll(true);
  }
  // Every callback ran inside one of those poll() calls, on this thread
  // (checked per callback above); the transfers themselves ran on the
  // lanes.
  out.device = ctx.device();
  out.lane_wait_ns = ctx.lane_wait_ns();
  out.device_depth = ctx.device_depth();
  out.most_concurrent = shared.most.load();
  out.rendezvous_timed_out = shared.rendezvous_timed_out.load();
  // The transfer did not happen on the thread that called poll().
  CHECK(shared.performed_off_the_poller.load());
  source::live = nullptr;
  return out;
}

TEST_CASE("a positional synchronous source serves several transfers at once") {
  // Every transfer waits for a second lane to be inside the source, so
  // reaching two is a handshake: the run either overlaps or times out.
  const lane_run<true> r = run_counting<true>(/*depth=*/8, /*n=*/12, 4096, /*rendezvous=*/true);
  CHECK(r.completed == 12);
  CHECK(r.every_callback_on_the_polling_thread);
  CHECK_FALSE(r.rendezvous_timed_out);
  // Four lanes is the ceiling, and more than one transfer was inside the
  // source at once.
  CHECK(r.most_concurrent > 1);
  CHECK(r.most_concurrent <= static_cast<int>(io_impl::max_io_lanes));
  CHECK(r.device_depth == io_impl::max_io_lanes);
}

TEST_CASE("a source that cannot take two at once gets one lane") {
  const lane_run<false> r = run_counting<false>(/*depth=*/8, /*n=*/8);
  CHECK(r.completed == 8);
  CHECK(r.every_callback_on_the_polling_thread);
  // One cursor, one lane: off the polling thread, but never overlapped.
  CHECK(r.most_concurrent == 1);
  CHECK(r.device_depth == 1);
}

TEST_CASE("the depth is the ceiling on lanes as much as the constant is") {
  const lane_run<true> r = run_counting<true>(/*depth=*/2, /*n=*/6);
  CHECK(r.completed == 6);
  CHECK(r.device_depth == 2);
  CHECK(r.most_concurrent <= 2);
}

TEST_CASE("the device's own time has no wait for a lane in it") {
  // Twelve transfers of 4 ms over four lanes: the device saw 12 x 4 ms
  // whatever the queueing, and two thirds of them waited for a lane
  // first. The two numbers tell "the device is the wall" from "there are
  // too few lanes", which is why the transfer is stamped on the lane and
  // not measured as a span of the caller's timeline.
  const lane_run<true> r = run_counting<true>(/*depth=*/8, /*n=*/12);
  CHECK(r.device.transfers == 12);
  // Generous bands: this is a sleep on a loaded test machine.
  CHECK(r.device.total_ns > 12 * 2'000'000ULL);
  CHECK(r.device.min_ns > 1'000'000ULL);
  // Twelve transfers over four lanes queue three deep, so the wait is
  // real and it is NOT in the device figure above.
  CHECK(r.lane_wait_ns > 0);
}

TEST_CASE("a failed transfer's error survives the lane's encoding") {
  // The result a lane reports is carried as a value and a category flag
  // rather than as a whole std::error_code, which is what keeps the
  // Windows union inside io_op's storage (detail/driver.hpp). So the
  // round trip is worth a case of its own, and over BOTH categories the
  // encoding distinguishes: errno, which is every POSIX source and every
  // cancel, and the system category, which is what the Windows backends
  // throw and what no POSIX run would otherwise exercise.
  using source = counting_source<true>;
  using context = io_impl::sync_context<source>;

  const std::error_code cases[] = {
      std::error_code(EIO, std::generic_category()),
      std::error_code(5, std::system_category()),  // ERROR_ACCESS_DENIED
  };
  for (const std::error_code& want : cases) {
    CAPTURE(want.value());
    CAPTURE(want.category().name());
    source::shared shared;
    shared.hold = std::chrono::milliseconds(0);
    shared.poller = std::this_thread::get_id();
    shared.fail_with = want;
    source::live = &shared;

    struct seen {
      std::error_code ec{};
      int calls = 0;
    } got;
    context ctx({}, 4);
    typename context::file file(ctx, fs::path("ignored"), {});
    std::vector<std::byte> buf(4096);
    typename context::transfer op{};
    op.owner = &got;
    op.done = +[](io_impl::transfer_base* b, std::error_code ec) noexcept {
      auto* const s = static_cast<seen*>(b->owner);
      s->ec = ec;
      s->calls++;
    };
    ctx.submit_read(file, 0, buf, op);
    ctx.flush();
    while (got.calls == 0) {
      ctx.poll(true);
    }
    CHECK(got.calls == 1);
    CHECK(got.ec == want);
    CHECK(got.ec.value() == want.value());
    CHECK(got.ec.category() == want.category());
    source::live = nullptr;
  }
}

TEST_CASE("io_lanes starts on first use and not before") {
  struct owner {
    std::atomic<int> wakes{0};
    std::atomic<int> bodies{0};
    void wake() noexcept { wakes.fetch_add(1); }
  } o;
  io_impl::io_lanes lanes(io_impl::io_lanes::waker<owner>(), &o, 3);
  // Nothing has asked it to do anything, so there is no thread yet --
  // which is what keeps a context whose engine took everything from
  // paying for one.
  CHECK_FALSE(lanes.running());
  CHECK(lanes.count() == 3);

  const bool started = lanes.start(
      +[](void* p) noexcept {
        auto* const self = static_cast<owner*>(p);
        self->bodies.fetch_add(1);
        self->wake();
      },
      nullptr, &o);
  CHECK(started);
  CHECK(lanes.running());
  CHECK(lanes.count() == 3);
  // Idempotent: a second ask does not start a second set.
  CHECK(lanes.start(+[](void*) noexcept {}, nullptr, &o));
  lanes.stop();
  CHECK(o.bodies.load() == 3);
  CHECK(o.wakes.load() == 3);
  // Idempotent the other way too, which is what the destructor relies on.
  lanes.stop();
}

TEST_CASE("a file closed with a transfer in a lane waits the lane out") {
  // The hazard is the descriptor: a lane inside pread on a file that has
  // closed would, once the number is recycled, read another file's bytes
  // into the engine's buffer and report success. forget() waits it out,
  // and reports nothing of that file's.
  using source = counting_source<true>;
  using context = io_impl::sync_context<source>;
  source::shared shared;
  shared.hold = std::chrono::milliseconds(30);
  shared.poller = std::this_thread::get_id();
  source::live = &shared;

  int completed = 0;
  {
    context ctx({}, 4);
    typename context::file file(ctx, fs::path("ignored"), {});
    std::array<context::transfer, 4> ops{};
    std::array<std::vector<std::byte>, 4> bufs;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      bufs[i].resize(4096);
      ops[i].owner = &completed;
      ops[i].done = +[](io_impl::transfer_base* b,
                        std::error_code) noexcept {
        ++*static_cast<int*>(b->owner);
      };
      ctx.submit_read(file, i * 4096, bufs[i], ops[i]);
    }
    // Let the lanes get inside the source, then reclaim the file.
    while (shared.inside.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ctx.forget(file);
    // Nothing of this file's was reported, and no lane is still inside
    // it: that is what forget() promises, and it is what makes the
    // buffers and the descriptor safe to release next.
    CHECK(completed == 0);
    CHECK(shared.inside.load() == 0);
    CHECK(ctx.in_flight() == 0);
  }
  source::live = nullptr;
}

// The operation lifetime rule from the other side: an operation must
// outlive the drain of its file, and once that drain returns no lane may
// touch it again. The operations here are heap objects freed right after
// the file closes, so a lane still finishing a transfer, or taking one
// that was queued, is a heap-use-after-free under ASan; without ASan the
// counts say the same. One lane and three transfers, so two are queued
// when the file closes.
TEST_CASE("after a file's drain no lane touches its operations") {
  using source = counting_source<true>;
  using context = io_impl::sync_context<source>;
  source::shared shared;
  shared.hold = std::chrono::milliseconds(40);
  shared.poller = std::this_thread::get_id();
  source::live = &shared;
  {
    context ctx({}, 1);
    std::array<std::vector<std::byte>, 3> bufs;
    std::vector<std::unique_ptr<context::transfer>> ops;
    int completed = 0;
    {
      typename context::file file(ctx, fs::path("ignored"), {});
      for (std::size_t i = 0; i < bufs.size(); ++i) {
        bufs[i].resize(4096);
        ops.push_back(std::make_unique<context::transfer>());
        ops[i]->owner = &completed;
        ops[i]->done = +[](io_impl::transfer_base* b,
                           std::error_code) noexcept {
          ++*static_cast<int*>(b->owner);
        };
        ctx.submit_read(file, i * 4096, bufs[i], *ops[i]);
      }
      while (shared.inside.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }  // ~file: transfers owed, so the file forgets them
    // The lane that was inside has finished, and nothing queued was taken.
    CHECK(shared.inside.load() == 0);
    CHECK(shared.transfers.load() == 1);
    CHECK(ctx.in_flight() == 0);
    ops.clear();
    // Time for a lane that had not been waited out to reach a freed
    // operation.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(shared.transfers.load() == 1);
    CHECK(ctx.poll(false) == 0);
    CHECK(completed == 0);
  }
  source::live = nullptr;
}

#if defined(__linux__)
// The same rule on a real backend's lanes: with async off, every
// transfer rides the pump and a pread into the caller's buffer is what a
// lane is doing when the file closes.
TEST_CASE("io_uring without a ring: after a file's drain no lane touches its operations") {
  constexpr std::size_t len = 32 * 1024 * 1024;
  const auto content = pattern(2 * len, 13);
  const temp_file f(content);
  std::vector<std::byte> buf(2 * len);
  io_impl::uring_context ctx({.async = false}, 1);
  std::array<std::unique_ptr<io_impl::uring_context::transfer>, 2> ops{
      std::make_unique<io_impl::uring_context::transfer>(),
      std::make_unique<io_impl::uring_context::transfer>()};
  probe p;
  {
    io_impl::uring_context::file file(ctx, f.path,
                                      io_impl::file_open_options{});
    for (std::size_t i = 0; i < ops.size(); ++i) {
      ops[i]->done = &probe::on_done;
      ops[i]->owner = &p;
      ctx.submit_read(file, i * len,
                      std::span{buf}.subspan(i * len, len), *ops[i]);
    }
    ctx.flush();
    // Usually puts the first read on a lane before the file closes, so the
    // drain waits for a transfer in progress. A drain that finds both
    // still queued takes them off the queue instead, and the checks below
    // hold either way.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(ctx.in_flight() == 0);
  for (auto& op : ops) {
    op.reset();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(ctx.poll(false) == 0);
  CHECK(p.calls == 0);
}
#endif

TEST_CASE("a cancel reaches a transfer no lane has taken yet") {
  using source = counting_source<true>;
  using context = io_impl::sync_context<source>;
  source::shared shared;
  // The lanes are held inside the source until the cancels are in, so
  // at most four of the ten transfers can have been taken when they
  // arrive (one per lane) and at least six are still waiting.
  shared.hold = std::chrono::milliseconds(0);
  shared.gated = true;
  shared.poller = std::this_thread::get_id();
  source::live = &shared;

  struct tally {
    int done = 0;
    int cancelled = 0;
  } t;
  {
    context ctx({}, 8);
    typename context::file file(ctx, fs::path("ignored"), {});
    // More transfers than lanes, so the tail of the queue is still
    // waiting when the cancels arrive.
    std::array<context::transfer, 10> ops{};
    std::array<std::vector<std::byte>, 10> bufs;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      bufs[i].resize(4096);
      ops[i].owner = &t;
      ops[i].done = +[](io_impl::transfer_base* b,
                        std::error_code ec) noexcept {
        auto* const sum = static_cast<tally*>(b->owner);
        sum->done++;
        if (ec == std::errc::operation_canceled) {
          sum->cancelled++;
        }
      };
      ctx.submit_read(file, i * 4096, bufs[i], ops[i]);
    }
    for (auto& op : ops) {
      ctx.cancel(op);
    }
    shared.gated = false;
    while (t.done < static_cast<int>(ops.size())) {
      ctx.poll(true);
    }
    // Every transfer reported exactly once, which is the contract; a
    // cancel that arrived before a lane took the transfer took effect,
    // and the ones a lane already had ran to completion, which the
    // contract's advisory cancel allows.
    CHECK(t.done == static_cast<int>(ops.size()));
    // At most one transfer per lane was taken before the cancels arrived;
    // the rest were cancelled before any lane came back for them.
    CHECK(t.cancelled >=
          static_cast<int>(ops.size() - io_impl::max_io_lanes));
  }
  source::live = nullptr;
}

}  // TEST_SUITE
