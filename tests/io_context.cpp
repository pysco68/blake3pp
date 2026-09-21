// The completion-callback reader contract (src/io/backend.hpp): what a
// context promises the engine, tested on the contexts themselves rather
// than through file_reader.
//
// The invariant every later phase rests on is where a callback runs:
// inside poll(), on the thread that called it, and nowhere else. Most of
// what follows is that one sentence, checked from several directions --
// after submit, after flush, from a second thread, and from a context
// that is being destroyed with reads still owed.

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
#include "io/null_backend.hpp"
#include "io/stdio_backend.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include "io/pread_backend.hpp"
#endif
#if defined(__linux__)
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
  };

  class file {
   public:
    file(fake_context&, std::uint64_t size) noexcept : size_(size) {}
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] std::string_view name() const noexcept { return "fake"; }

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
    queue_.push_back(&op);
  }
  void flush() {}

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

  // The engine deliberately gives no way to reach its context, so the
  // window to fail is named before the engine is built. A static on a
  // type that exists only in this file is the smallest thing that can
  // say it.
  static inline std::uint64_t fail_offset = ~std::uint64_t{0};

 private:
  std::vector<read_op*> queue_;
  io_impl::poll_waiter waiter_;
};

static_assert(io_impl::reader_context<fake_context>);

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

}  // namespace

TEST_SUITE("io_context") {

TEST_CASE("callbacks run only inside poll") {
  const auto content = pattern(64 * 1024, 7);
  const temp_file f(content);
#if defined(__linux__)
  check_callback_discipline<io_impl::uring_context>(
      "uring", [&](io_impl::uring_context& c) {
        return io_impl::uring_context::file(c, f.path, /*direct_io=*/false);
      });
#endif
#if defined(__unix__) || defined(__APPLE__)
  check_callback_discipline<io_impl::pread_context>(
      "pread", [&](io_impl::pread_context& c) {
        return io_impl::pread_context::file(c, f.path, /*direct_io=*/false);
      });
#endif
#if defined(_WIN32)
  check_callback_discipline<io_impl::iocp_context>(
      "iocp", [&](io_impl::iocp_context& c) {
        return io_impl::iocp_context::file(c, f.path, /*direct_io=*/false);
      });
#endif
#if defined(__APPLE__)
  check_callback_discipline<io_impl::gcd_context>(
      "gcd", [&](io_impl::gcd_context& c) {
        return io_impl::gcd_context::file(c, f.path, /*direct_io=*/false);
      });
#endif
  check_callback_discipline<io_impl::stdio_context>(
      "stdio", [&](io_impl::stdio_context& c) {
        return io_impl::stdio_context::file(c, f.path, /*direct_io=*/false);
      });
  check_callback_discipline<io_impl::null_context>(
      "null", [](io_impl::null_context& c) {
        return io_impl::null_context::file(c, 16384);
      });
}

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
    typename C::file file_a(ctx, fa.path, false);
    typename C::file file_b(ctx, fb.path, false);
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
  {
    io_impl::null_context ctx({}, 4);
    check(ctx);
  }
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
    ctx_type::file file(ctx, f.path, false);
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
  check([] { return std::make_unique<io_impl::null_context>(
                 io_impl::reader_context_options{}, 4u); });
}

TEST_CASE("the null source hashes the pattern it was filled with") {
  // A size that is not a window multiple: the last window is short, which
  // is where a reader that mis-sizes the tail shows up as a wrong digest.
  constexpr std::uint64_t size = 5 * 64 * 1024 + 4321;
  constexpr std::size_t window = 64 * 1024;
  io_impl::polled_reader_engine<io_impl::null_context> r(
      size, {.window_bytes = window, .queue_depth = 3});
  CHECK(r.backend_name() == "null");
  const auto pat = pattern(window, 97);
  const auto pool = r.pool();
  for (std::size_t off = 0; off < pool.size(); off += window) {
    std::memcpy(pool.data() + off, pat.data(),
                std::min(window, pool.size() - off));
  }
  blake3pp::hasher h;
  while (auto w = r.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    r.release(*w);
  }
  blake3pp::hasher expect;
  for (std::uint64_t done = 0; done < size;) {
    const auto n =
        static_cast<std::size_t>(std::min<std::uint64_t>(window, size - done));
    expect.update(std::span<const std::byte>{pat.data(), n});
    done += n;
  }
  CHECK(h.finalize() == expect.finalize());
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
    // Captured from the slot-based engine before the contract changed.
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
      return io_impl::uring_context::file(c, f.path, false);
    });
  }
#endif
#if defined(__APPLE__)
  {
    io_impl::gcd_context ctx({}, 4);
    check(ctx, [&](io_impl::gcd_context& c) {
      return io_impl::gcd_context::file(c, f.path, false);
    });
  }
#endif
#if defined(_WIN32)
  {
    io_impl::iocp_context ctx({}, 4);
    check(ctx, [&](io_impl::iocp_context& c) {
      return io_impl::iocp_context::file(c, f.path, false);
    });
  }
#endif
}

}  // TEST_SUITE
