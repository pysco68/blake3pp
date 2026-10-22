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
#if defined(__FreeBSD__)
#include "io/aio_backend.hpp"
#endif
#if defined(_WIN32)
#include "io/iocp_backend.hpp"
#include "io/ioring_backend.hpp"
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
                   transfer& op) {
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
                    transfer& op) {
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

  // The engine gives no way to reach its context, so the
  // window to fail is named before the engine is built. A static on a
  // type that exists only in this file is the smallest thing that can
  // say it.
  static inline std::uint64_t fail_offset = ~std::uint64_t{0};

 private:
  std::vector<transfer*> queue_;
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

// A file closed with transfers it could not hand to the port (an
// unaligned length under NO_BUFFERING, or no port at all) has to take
// them with it: they name its handles and its share of the count. Left
// queued, they would be performed through a closed handle, and a file
// with a port would wait on it for completions that never arrive.
TEST_CASE("an IOCP file closed with deferred transfers takes them with it") {
  namespace io_impl = blake3pp::detail::io_impl;
  const std::vector<std::byte> content(64 * 1024 + 100, std::byte{0x5c});
  const temp_file f(content);
  for (const bool async : {false, true}) {
    CAPTURE(async);
    io_impl::iocp_context ctx({.async = async}, 4);
    std::array<std::byte, 100> buf{};  // below one NO_BUFFERING granule
    io_impl::iocp_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    {
      io_impl::iocp_context::file file(
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

// Registered buffers are an option, not a rung the caller can be given
// without asking, and a ring that refuses the registration has to keep
// reading. All three answers deliver the same bytes: registration asked
// for and taken, not asked for at all, and asked for with a region no
// ring can accept.
TEST_CASE("IoRing: registration on, off and refused deliver the same bytes") {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = pattern(300 * 1024 + 77, 46);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);
  for (const bool registered : {false, true}) {
    CAPTURE(registered);
    io_impl::polled_reader_engine<io_impl::ioring_context> r(
        f.path, {.window_bytes = 64 * 1024,
                 .queue_depth = 2,
                 .registered_buffers = registered});
    MESSAGE("reader backend: " << r.backend_name());
    blake3pp::hasher h;
    while (auto w = r.next()) {
      h.update(std::span<const std::byte>{w->data, w->bytes});
      r.release(*w);
    }
    CHECK(h.finalize() == expected);
  }
  // A registration the ring cannot take: an empty region describes
  // nothing. The transfers then name pointers, as every transfer outside
  // a registered arena does, and the bytes are the same.
  {
    io_impl::ioring_context ctx({.registered_buffers = true}, 4);
    ctx.register_arena({});
    CHECK(!ctx.arena_registered());
    io_impl::ioring_context::file file(ctx, f.path,
                                       io_impl::file_open_options{});
    std::vector<std::byte> got(content.size());
    probe p;
    io_impl::ioring_context::transfer op{};
    op.done = &probe::on_done;
    op.owner = &p;
    ctx.submit_read(file, 0, std::span{got}, op);
    ctx.flush();
    while (p.calls == 0) {
      ctx.poll(true);
    }
    CHECK(!p.ec);
    CHECK(got == content);
  }
}

// The reasons a ring could not be had are part of the ladder a tool
// prints, and the machine that runs this suite is usually one that HAS a
// ring, so the spellings are asserted on the composer rather than waited
// for. A Windows 10 host reaches them through name() instead.
TEST_CASE("IoRing: a refused ring names the reason") {
  namespace io_impl = blake3pp::detail::io_impl;
  using refusal = io_impl::ioring_refusal;
  CHECK(io_impl::no_ioring_suffix(refusal{}).empty());
  CHECK(io_impl::no_ioring_suffix({refusal::no_api, 0, 0}) ==
        " [ioring: no api]");
  CHECK(io_impl::no_ioring_suffix({refusal::old_version, 1, 0}) ==
        " [ioring: version 1]");
  CHECK(io_impl::no_ioring_suffix({refusal::create_failed, 0, 0x80070057}) ==
        " [ioring: CreateIoRing failed 0x80070057]");
}

// The ring's twin of the case above it: a file closed with transfers it
// could not hand the ring must take them with it, because they name its
// handles and its share of the count.
TEST_CASE("an IoRing file closed with deferred transfers takes them with it") {
  namespace io_impl = blake3pp::detail::io_impl;
  const std::vector<std::byte> content(64 * 1024 + 100, std::byte{0x5d});
  const temp_file f(content);
  for (const bool async : {false, true}) {
    CAPTURE(async);
    io_impl::ioring_context ctx({.async = async}, 4);
    std::array<std::byte, 100> buf{};  // below one NO_BUFFERING granule
    io_impl::ioring_context::transfer op{};
    probe p;
    op.done = &probe::on_done;
    op.owner = &p;
    {
      io_impl::ioring_context::file file(
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
  CHECK(op.filled == content.size());
  CHECK(op.error == 0);
  CHECK(buf == content);
  CHECK(ctx.in_flight() == 0);
  CHECK(ctx.poll(false) == 0);
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

// A length that is not a multiple of the direct-I/O granule is the last
// read of almost every real file, and the whole read of a small one. It
// rides the ring in both directions: on the plain descriptor for a
// buffered file, and for a direct one as the whole granules on the direct
// descriptor followed by the rest on the plain one. None of it reaches
// the synchronous path, which costs one device round trip per poll().
// Before this held, a directory of 65000-byte files read at a quarter of
// the rate of 65536-byte ones, buffered or direct.
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
  ctx.defer_pending();
  CHECK(pa.calls == 0);
  CHECK(pb.calls == 0);
  CHECK(ctx.in_flight() == 2);

  // One deferred transfer per poll, on the caller's thread, same as
  // every other transfer this backend cannot queue.
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

// A length that is not a multiple of the direct-I/O granule is the last
// read of almost every real file, and the whole read of a small one. It
// rides the AIO queue in both directions: on the plain descriptor for a
// buffered file, and for a direct one as the whole granules on the
// direct descriptor followed by the rest on the plain one. None of it
// reaches the synchronous path, which costs one device round trip per
// poll().
TEST_CASE("aio: an unaligned length rides the queue, buffered or direct") {
  constexpr std::size_t len = 2 * 4096 + 100;
  const auto content = pattern(len, 43);
  for (const bool direct : {false, true}) {
    CAPTURE(direct);
    const temp_file f(content);
    io_impl::aio_context ctx({}, 4);
    if (!ctx.using_aio()) {
      MESSAGE("no kernel AIO here (" << ctx.refusal() << "); nothing to check");
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
  {
    ctx_type ctx({}, 4);
    ctx_type::file file(ctx, f.path, io_impl::file_open_options{});
    ctx_type::transfer op{};
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
  if (aio_probe.using_aio()) {
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
    const std::string why(aio_probe.refusal());
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
  if (probe.using_ring()) {
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
    const std::string why = io_impl::no_ioring_suffix(probe.refusal());
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
}

}  // TEST_SUITE
