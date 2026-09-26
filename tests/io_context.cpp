// The completion-callback reader contract (src/io/backend.hpp): what a
// context promises the engine, tested on the contexts themselves rather
// than through file_reader.
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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <random>
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

#include "io/backend.hpp"
#include "io/engine.hpp"
#include "io/stdio_backend.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include "io/pread_backend.hpp"
#endif
#if defined(__linux__)
#include <unistd.h>  // getpid, for a temporary name under /dev/shm

#include "io/uring_backend.hpp"
#endif
#if defined(_WIN32)
#include "io/iocp_backend.hpp"
#endif
#if defined(__APPLE__)
#include "io/gcd_backend.hpp"
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
    static const unsigned run_id = std::random_device{}();
    path = fs::temp_directory_path() /
           ("blake3pp_ctx_test_" + std::to_string(run_id) + "_" +
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
// whose completions arrive on its own workers (GCD) has to hand them over
// rather than call from there, and nothing else in the suite would
// notice if it did not.
struct probe {
  int calls = 0;
  std::error_code ec{};
  std::thread::id ran_on{};
  static void on_done(io_impl::read_op_base* base,
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
  struct read_op : io_impl::read_op_base {
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
                   read_op& op) {
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
                    read_op& op) {
    op.buf = {};
    op.off = off;
    op.filled = buf.size();
    op.done_once = false;
    op.cancelled = false;
    queue_.push_back(&op);
  }

  void cancel(read_op& op) noexcept { op.cancelled = true; }

  std::size_t poll(bool) {
    if (queue_.empty()) {
      return 0;
    }
    // Out of order: the newest submission finishes first.
    read_op& op = *queue_.back();
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
    op.done(&op, {});
    return 1;
  }

  void wake() noexcept { waiter_.wake(); }
  [[nodiscard]] std::size_t in_flight() const noexcept {
    return queue_.size();
  }
  void drain() noexcept { queue_.clear(); }

  // The engine deliberately gives no way to reach its context, so the
  // window to fail is named before the engine is built. A static on a
  // type that exists only in this file is the smallest thing that can
  // say it.
  static inline std::uint64_t fail_offset = ~std::uint64_t{0};

 private:
  std::vector<read_op*> queue_;
  io_impl::poll_waiter waiter_;
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
  typename C::read_op op_a{};
  typename C::read_op op_b{};
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
  typename C::read_op op_a{};
  typename C::read_op op_b{};
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
  typename C::read_op op_c{};
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
// an unaligned one takes the deferred path inside the backend.
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
    typename C::read_op op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;

    ctx.submit_write(f, 0, std::span<const std::byte>{content}, op);
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
#endif
#if defined(__APPLE__)
  // No cancel mechanism at all: the read runs to completion and reports
  // its data, which is the contract's other legal answer.
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
#if defined(__unix__) || defined(__APPLE__)
      check_write_discipline<io_impl::pread_context>(
          "pread", out.path, direct, len,
          [&](io_impl::pread_context& c) {
            return io_impl::pread_context::file(
                c, out.path,
                io_impl::file_open_options{.write = true, .direct_io = direct});
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
    io_impl::uring_context::read_op op{};
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
    io_impl::stdio_context::read_op op{};
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
// usable file behind, not a failed open: the writes simply extend the
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
    io_impl::uring_context::read_op op{};
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
  io_impl::polled_reader_engine<fake_context> r(
      size, {.window_bytes = 64 * 1024, .queue_depth = 3});
  CHECK(r.file_size() == size);
  CHECK(r.backend_name() == "fake");
  std::vector<std::byte> out;
  while (auto w = r.next()) {
    out.insert(out.end(), w->data, w->data + w->bytes);
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
  io_impl::polled_reader_engine<fake_context> r(
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
    typename C::read_op op_a{};
    typename C::read_op op_b{};
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
  io_impl::uring_context::read_op a{};
  io_impl::uring_context::read_op b{};
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

  // One deferred read per poll, on the caller's thread, same as every
  // other read this backend cannot put on the ring.
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
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    check(ctx);
  }
#endif
}

#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
// The destructor drains without completing anything into the caller's
// hands: the pool outlives the context here, so a callback running from
// teardown would be visible, and so would a kernel write into freed
// memory (which is what ASan is watching for).
TEST_CASE("a context destroyed with reads in flight runs no callback") {
#if defined(__linux__)
  using ctx_type = io_impl::uring_context;
#elif defined(_WIN32)
  using ctx_type = io_impl::iocp_context;
#else
  using ctx_type = io_impl::gcd_context;
#endif
  const auto content = pattern(8 * 1024 * 1024, 3);
  const temp_file f(content);
  std::vector<std::byte> buf(content.size());
  probe p;
  {
    ctx_type ctx({}, 4);
    ctx_type::file file(ctx, f.path, io_impl::file_open_options{});
    ctx_type::read_op op{};
    op.done = &probe::on_done;
    op.owner = &p;
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
    typename ctx_type::read_op op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span{buf}, op);
    ctx.flush();
    ctx.drain();
    CHECK(p.calls == 0);
    CHECK(ctx.in_flight() == 0);

    // Still usable, and the callback discipline still holds.
    typename ctx_type::read_op again{};
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
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    io_impl::gcd_context::file file(ctx, f.path, io_impl::file_open_options{});
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
#if defined(__unix__) || defined(__APPLE__)
  check([] { return std::make_unique<io_impl::pread_context>(
                 io_impl::reader_context_options{}, 4u); });
#endif
#if defined(_WIN32)
  check([] { return std::make_unique<io_impl::iocp_context>(
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
#elif defined(__APPLE__)
  // F_NOCACHE is per-fd and tolerates any alignment, so there is no
  // unaligned-tail rung: the ladder is GCD or not, cache or not.
  rows = {{true, true, true, "gcd+nocache"},
          {false, true, true, "gcd"},
          {true, false, true, "pread+nocache"},
          {false, false, true, "pread"}};
#elif defined(_WIN32)
  // NO_BUFFERING and the port are per-open flags, so the ladder is the
  // same shape: what engaged, and what it fell back to.
  rows = {{true, true, true, "iocp+direct"},
          {false, true, true, "iocp"},
          {true, false, true, "readfile+direct"},
          {false, false, true, "readfile"}};
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
                 .offload_submit = r.offload});
    CHECK(reader.backend() == r.name);
    // And the data is the same however it degraded.
    std::vector<std::byte> out;
    while (auto w = reader.next()) {
      out.insert(out.end(), w->data, w->data + w->bytes);
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
    typename std::remove_reference_t<decltype(ctx)>::read_op op{};
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
#endif
}

}  // TEST_SUITE
