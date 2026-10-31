#include <algorithm>
#include <array>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>
#include <doctest/doctest.h>

// The portable engines and the I/O contexts the platform selector never
// picks here: instantiating the engines with them below keeps the
// off-platform code compiled AND executed on every platform
// (on Linux the selector only ever compiles the io_uring backend).
#include "io/engine.hpp"
#include "io/stdio_backend.hpp"
#if defined(__unix__) || defined(__APPLE__)
#include "io/pread_backend.hpp"
#endif
#if defined(__linux__)
#include <fcntl.h>
#include "io/backend.hpp"
#include "io/uring_backend.hpp"
#endif
#if defined(__FreeBSD__)
#include "io/aio_backend.hpp"
#endif
#if defined(__APPLE__)
#include "io/gcd_backend.hpp"
#endif
#if defined(_WIN32)
#include "io/iocp_backend.hpp"
#include "io/ioring_backend.hpp"
#endif
// The platform's own choice, which is what the engine list is read off:
// the tests below ask the selected context what rung it engaged and
// hold the public list to the answer.
#include "io/backend_select.hpp"
#include "temp_names.hpp"

namespace {

namespace fs = std::filesystem;

std::vector<std::byte> make_input(std::size_t len) {
  std::vector<std::byte> v(len);
  for (std::size_t i = 0; i < len; ++i) {
    v[i] = static_cast<std::byte>(i % 251);
  }
  return v;
}

struct temp_file {
  fs::path path;
  explicit temp_file(const std::vector<std::byte>& content) {
    path = fs::temp_directory_path() /
           ("blake3pp_io_test_" + b3test::process_salt() + "_" +
            std::to_string(counter++));
    // ostream::write wants char, so the bytes are converted rather than
    // reinterpreted: one transform and one bulk write.
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

// Every io_engine enumerator, expanded from the canonical table rather
// than written out. Nothing public hands this list out -- an engine no
// build here can reach is not a fact about this machine, and the API
// reports only what is true of it -- but the table's own rows still
// have to be checked, and a test that re-typed the enum would be the
// very drift the table exists to stop.
constexpr blake3pp::io_engine every_io_engine[] = {
#define BLAKE3PP_IO_ENGINE(enumerator, read_name, write_name, rank, async) \
  blake3pp::io_engine::enumerator,
#include <blake3pp/detail/io_engine.def>
#undef BLAKE3PP_IO_ENGINE
};

// The leading engine name of a `backend` string, with every decoration
// the backends append cut off: "+direct", "+nocache", "+regbuf",
// " (inline submit)", " [io_uring: ENOSYS, kernel too old]". What is
// left is one io_engine's own spelling, which is what to_string() gives.
std::string engine_token(std::string_view backend) {
  return std::string(backend.substr(0, backend.find_first_of("+ ")));
}

TEST_SUITE("io") {

// Every size class: empty, sub-chunk, exact windows, unaligned tails,
// multi-window. Small windows + low queue depth force buffer recycling.
TEST_CASE("hash_file matches in-memory hash across size classes") {
  const blake3pp::hash_file_options small_windows{
      .window_bytes = 64 * 1024, .queue_depth = 3};

  for (const std::size_t len :
       {std::size_t{0}, std::size_t{1}, std::size_t{1024},
        std::size_t{4096}, std::size_t{64 * 1024},
        std::size_t{64 * 1024 + 1}, std::size_t{192 * 1024},
        std::size_t{1024 * 1024 + 4097}, std::size_t{3 * 1024 * 1024 + 17}}) {
    CAPTURE(len);
    const auto content = make_input(len);
    const temp_file f(content);
    const auto expected = blake3pp::hash(content);
    CHECK(blake3pp::hash_file(f.path) == expected);  // fs::path overload
    CHECK(blake3pp::hash_file(f.path, small_windows) == expected);
    CHECK(blake3pp::hash_file(f.path, {.direct_io = false}) == expected);
  }
}

TEST_CASE("error_code overload reports instead of throwing") {
  std::error_code ec;
  const auto d =
      blake3pp::hash_file("/nonexistent/blake3pp/no/such/file", ec);
  CHECK(ec);
  CHECK(ec == std::errc::no_such_file_or_directory);
  CHECK(d == blake3pp::digest{});

  const auto content = make_input(4096);
  const temp_file f(content);
  const auto ok = blake3pp::hash_file(f.path, ec);
  CHECK(!ec);
  CHECK(ok == blake3pp::hash(content));
}

TEST_CASE("parallel hash_file matches, across window boundaries") {
  auto sched = blake3pp::get_parallel_scheduler();

  for (const std::size_t len :
       {std::size_t{1024 * 1024}, std::size_t{4 * 1024 * 1024},
        std::size_t{9 * 1024 * 1024 + 12345}}) {
    CAPTURE(len);
    const auto content = make_input(len);
    const temp_file f(content);
    const auto expected = blake3pp::hash(content);
    CHECK(blake3pp::hash_file(f.path, sched) == expected);
    CHECK(blake3pp::hash_file(f.path, sched,
                              {.window_bytes = 1024 * 1024,
                               .queue_depth = 3}) == expected);
    std::error_code ec;
    CHECK(blake3pp::hash_file(f.path, sched, ec) == expected);
    CHECK(!ec);
  }
}

// The stack budget decides how many parts a window is split into, and so
// how many chaining values the window fold joins. 64 bytes is two parts,
// the fewest the fold can take; the default is 512 for an 8 MiB window.
// The window here is the default 8 MiB and the file is not a multiple of
// it, so the last window takes the sequential path.
TEST_CASE("a stack budget changes a window's split, not the digest") {
  using blake3pp::stack_budget;
  auto sched = blake3pp::get_parallel_scheduler();
  const auto content = make_input(9 * 1024 * 1024 + 12345);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);
  CHECK(blake3pp::hash_file<stack_budget{64}>(f.path, sched) == expected);
  CHECK(blake3pp::hash_file<stack_budget{1024}>(f.path, sched) == expected);
  CHECK(blake3pp::hash_file(f.path, sched) == expected);

  blake3pp::hasher h;
  blake3pp::update_file<stack_budget{64}>(h, f.path, sched);
  CHECK(h.finalize() == expected);
}

TEST_CASE("keyed hash_file matches keyed in-memory hashing") {
  std::array<std::byte, 32> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(200 - i);
  }
  const auto content = make_input(2 * 1024 * 1024 + 999);
  const temp_file f(content);
  const auto expected =
      blake3pp::keyed_hash(std::span<const std::byte, 32>{key}, content);

  const blake3pp::hash_mode mode = blake3pp::hash_mode::keyed(key);
  CHECK(blake3pp::hash_file(f.path, {.mode = mode}) == expected);
  CHECK(blake3pp::hash_file(f.path, blake3pp::get_parallel_scheduler(),
                            {.window_bytes = 1024 * 1024, .mode = mode}) ==
        expected);
}

// One mode per options object, by construction: there is one member, named
// the way hasher names the same choice, so no call can ask for two modes.
// These pin the shape that makes it true -- a field per mode would put the
// contradiction back.
static_assert(std::is_same_v<decltype(blake3pp::hash_file_options{}.mode),
                             blake3pp::hash_mode>);
// Through concepts, because a member access on a concrete type is a hard
// error rather than a substitution failure.
template <class T>
concept has_key_field = requires(T o) { o.key; };
template <class T>
concept has_context_field = requires(T o) { o.derive_context; };
static_assert(!has_key_field<blake3pp::hash_file_options>);
static_assert(!has_context_field<blake3pp::hash_file_options>);

// And no mode arrives by conversion: a key or a string reaches it through
// the named functions, never by being assigned to the member.
template <class T>
concept converts_to_mode = std::convertible_to<T, blake3pp::hash_mode>;
static_assert(!converts_to_mode<std::string_view>);
static_assert(!converts_to_mode<const char*>);
static_assert(!converts_to_mode<std::array<std::byte, blake3pp::key_size>>);
static_assert(!converts_to_mode<std::span<const std::byte, blake3pp::key_size>>);

// The mode owns its context, like its key, so every spelling of the string
// is accepted: the mode is what has to outlive the operation, and it does
// that on its own.
template <class S>
concept names_context = requires(S s) {
  blake3pp::hash_mode::derive_key(std::move(s));
};
static_assert(names_context<std::string_view>);
static_assert(names_context<std::string>);
static_assert(names_context<const char*>);

static_assert(blake3pp::hash_mode::plain().which() ==
              blake3pp::hash_mode::kind::plain);
static_assert(blake3pp::hash_file_options{}.mode.which() ==
              blake3pp::hash_mode::kind::plain);
static_assert(
    blake3pp::hash_file_options{
        .mode = blake3pp::hash_mode::keyed(std::array<std::byte, 32>{})}
        .mode.which() == blake3pp::hash_mode::kind::keyed);

// A context borrowed from the string that named it would dangle: the
// string is gone by the time the options are used, which the mode cannot
// notice and the digest cannot survive. Every way of producing one is
// here, because a named lvalue dangles exactly as a temporary does once
// the options outlive its scope.
TEST_CASE("a mode's context outlives the string it was built from") {
  const auto content = make_input(4096);
  const temp_file f(content);
  constexpr std::string_view context = "blake3pp 2026-10-03 lifetime";

  blake3pp::hasher expect_h = blake3pp::hasher::derive_key(context);
  expect_h.update(content);
  const blake3pp::digest expect = expect_h.finalize();

  const auto from_local_string = [&] {
    const std::string owned(context);
    return blake3pp::hash_file_options{
        .mode = blake3pp::hash_mode::derive_key(owned)};
  };
  const auto from_temporary = [&] {
    return blake3pp::hash_file_options{
        .mode = blake3pp::hash_mode::derive_key(std::string(context))};
  };
  const auto from_scratch_buffer = [&] {
    std::vector<char> buf(context.begin(), context.end());
    blake3pp::hash_file_options opts{
        .mode = blake3pp::hash_mode::derive_key(
            std::string_view(buf.data(), buf.size()))};
    std::fill(buf.begin(), buf.end(), '!');  // the view's bytes, overwritten
    return opts;
  };

  for (const auto& opts :
       {from_local_string(), from_temporary(), from_scratch_buffer()}) {
    CHECK(opts.mode.which() == blake3pp::hash_mode::kind::derive_key);
    CHECK(opts.mode.context() == context);
    CHECK(blake3pp::hash_file(f.path, opts) == expect);
  }
}

// The options object carries the mode, so hash_file() reaches all three
// and make_hasher() hands the same hasher to a caller that needs a
// finalize form hash_file() does not have.
TEST_CASE("hash_file_options carries the mode") {
  std::array<std::byte, 32> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(7 * i + 1);
  }
  constexpr std::string_view context = "blake3pp 2026-10-02 options test";
  const auto content = make_input(1024 * 1024 + 7);
  const temp_file f(content);

  blake3pp::hasher derived = blake3pp::hasher::derive_key(context);
  derived.update(content);

  const blake3pp::hash_file_options opts{.window_bytes = 256 * 1024,
                                         .mode = blake3pp::hash_mode::derive_key(context)};
  CHECK(blake3pp::hash_file(f.path, opts) == derived.finalize());
  CHECK(blake3pp::hash_file(f.path, blake3pp::get_parallel_scheduler(),
                            opts) == derived.finalize());

  // make_hasher() is the same hasher hash_file() builds internally, which
  // is what gets extended output out of an options object.
  blake3pp::hasher h = blake3pp::make_hasher(opts);
  blake3pp::update_file(h, f.path, opts);
  std::vector<std::byte> got(96);
  std::vector<std::byte> expected(96);
  h.finalize(got);
  derived.finalize(expected);
  CHECK(got == expected);

  // The other two modes, through the same entry point.
  CHECK(blake3pp::make_hasher({.mode = blake3pp::hash_mode::keyed(key)}).finalize() ==
        blake3pp::hasher::keyed(std::span<const std::byte, 32>{key})
            .finalize());
  CHECK(blake3pp::make_hasher({}).finalize() == blake3pp::hasher{}.finalize());
}

// update_file() is hasher::update() with a file as the source, so every
// mode and every finalize form must compose with it: here derive_key
// with extended output, sequential and over a scheduler, against the
// in-memory hasher fed the same bytes.
TEST_CASE("update_file composes with derive_key and extended output") {
  const auto content = make_input(3 * 1024 * 1024 + 4321);
  const temp_file f(content);
  constexpr std::string_view context = "blake3pp 2026-09-07 io test";

  blake3pp::hasher reference = blake3pp::hasher::derive_key(context);
  reference.update(content);
  std::vector<std::byte> expected(100);
  reference.finalize(expected);

  blake3pp::hasher seq = blake3pp::hasher::derive_key(context);
  blake3pp::update_file(seq, f.path, {.window_bytes = 1024 * 1024});
  std::vector<std::byte> got(100);
  seq.finalize(got);
  CHECK(got == expected);
  CHECK(seq.finalize() == reference.finalize());

  blake3pp::hasher par = blake3pp::hasher::derive_key(context);
  blake3pp::update_file(par, f.path, blake3pp::get_parallel_scheduler(),
                        {.window_bytes = 1024 * 1024});
  par.finalize(got);
  CHECK(got == expected);

  // The seekable reader too: a slice deep into the stream.
  auto stream = par.finalize_xof();
  stream.seek(1 << 20);
  std::vector<std::byte> slice(64);
  stream.fill(slice);
  auto ref_stream = reference.finalize_xof();
  ref_stream.seek(1 << 20);
  std::vector<std::byte> ref_slice(64);
  ref_stream.fill(ref_slice);
  CHECK(slice == ref_slice);
}

// Files hash in sequence. The parallel form can only offload a window as
// a subtree when the hasher sits on a window-aligned boundary, so the
// first file's size steers it: a window multiple keeps the second file
// on the fast path, anything else forces the fallback through update().
// Both must agree with the in-memory hash of the concatenation.
TEST_CASE("update_file hashes files in sequence, aligned and not") {
  constexpr std::size_t window = 1024 * 1024;
  const blake3pp::file_io_options opts{.window_bytes = window};
  auto sched = blake3pp::get_parallel_scheduler();

  for (const std::size_t first_len :
       {std::size_t{2 * window}, std::size_t{2 * window + 1},
        std::size_t{100 * 1024 + 3}, std::size_t{0}}) {
    CAPTURE(first_len);
    const auto a = make_input(first_len);
    const auto b = make_input(3 * window + 555);
    const temp_file fa(a);
    const temp_file fb(b);

    std::vector<std::byte> joined(a);
    joined.insert(joined.end(), b.begin(), b.end());
    const auto expected = blake3pp::hash(joined);

    blake3pp::hasher seq;
    blake3pp::update_file(seq, fa.path, opts);
    blake3pp::update_file(seq, fb.path, opts);
    CHECK(seq.finalize() == expected);

    blake3pp::hasher par;
    blake3pp::update_file(par, fa.path, sched, opts);
    blake3pp::update_file(par, fb.path, sched, opts);
    CHECK(par.finalize() == expected);
    CHECK(par.count() == joined.size());

    // Mixed: a sequential file followed by a parallel one.
    blake3pp::hasher mixed;
    blake3pp::update_file(mixed, fa.path, opts);
    blake3pp::update_file(mixed, fb.path, sched, opts);
    CHECK(mixed.finalize() == expected);
  }
}

TEST_CASE("update_file error_code form leaves the hasher usable") {
  const auto content = make_input(4096);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);

  blake3pp::hasher h;
  std::error_code ec;
  blake3pp::update_file(h, "/nonexistent/blake3pp/no/such/file", ec);
  CHECK(ec == std::errc::no_such_file_or_directory);
  blake3pp::update_file(h, "/nonexistent/blake3pp/no/such/file",
                        blake3pp::get_parallel_scheduler(), ec);
  CHECK(ec == std::errc::no_such_file_or_directory);

  h.reset();
  blake3pp::update_file(h, f.path, ec);
  CHECK(!ec);
  CHECK(h.finalize() == expected);

  h.reset();
  blake3pp::update_file(h, f.path, blake3pp::get_parallel_scheduler(), ec);
  CHECK(!ec);
  CHECK(h.finalize() == expected);

  blake3pp::hasher thrower;
  CHECK_THROWS_AS(
      blake3pp::update_file(thrower, "/nonexistent/blake3pp/no/such/file"),
      std::system_error);
}

// One options object drives both entry points: hash_file_options converts
// to the pipeline's file_io_options.
TEST_CASE("hash_file_options drives update_file") {
  const auto content = make_input(2 * 1024 * 1024 + 1);
  const temp_file f(content);
  const blake3pp::hash_file_options opts{.window_bytes = 1024 * 1024,
                                         .queue_depth = 2};
  blake3pp::hasher h;
  blake3pp::update_file(h, f.path, opts);
  CHECK(h.finalize() == blake3pp::hash_file(f.path, opts));
}

// Tracing: a trace_buffer in the options records one window_record per
// window (and, with agent storage, one agent_record per bulk invocation
// that took parts) without touching the digest.

// The window count the reader will produce for len bytes at window_bytes
// (a power of two here, so no rounding).
std::size_t window_count(std::size_t len, std::size_t window_bytes) {
  return (len + window_bytes - 1) / window_bytes;
}

TEST_CASE("tracing leaves the digest unchanged") {
  auto sched = blake3pp::get_parallel_scheduler();
  const std::size_t len = 3 * 1024 * 1024 + 17;  // not a window multiple
  const auto content = make_input(len);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);

  for (const std::size_t window : {std::size_t{64 * 1024},
                                   std::size_t{1024 * 1024}}) {
    CAPTURE(window);
    std::vector<blake3pp::window_record> windows(window_count(len, window));
    std::vector<blake3pp::agent_record> agents(windows.size() * 64);
    for (const bool with_agents : {false, true}) {
      CAPTURE(with_agents);
      blake3pp::trace_buffer trace(
          windows, with_agents ? std::span{agents}
                               : std::span<blake3pp::agent_record>{});
      const blake3pp::file_io_options opts{.window_bytes = window,
                                           .trace = &trace};

      blake3pp::hasher seq;
      blake3pp::update_file(seq, f.path, opts);
      CHECK(seq.finalize() == expected);
      CHECK(trace.windows().size() == windows.size());

      trace.clear();
      blake3pp::hasher par;
      blake3pp::update_file(par, f.path, sched, opts);
      CHECK(par.finalize() == expected);
      CHECK(trace.windows().size() == windows.size());

      // hash_file_options forwards the buffer too.
      trace.clear();
      CHECK(blake3pp::hash_file(f.path, sched,
                                {.window_bytes = window, .trace = &trace}) ==
            expected);
      CHECK(trace.windows().size() == windows.size());
    }
  }
}

TEST_CASE("window records are complete, in order and consistent") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t window = 64 * 1024;
  const std::size_t len = 5 * window + 4097;
  const auto content = make_input(len);
  const temp_file f(content);
  const std::size_t n = window_count(len, window);
  std::vector<blake3pp::window_record> windows(n);
  blake3pp::trace_buffer trace(windows);
  const blake3pp::file_io_options opts{.window_bytes = window,
                                       .trace = &trace};

  for (const bool parallel : {false, true}) {
    CAPTURE(parallel);
    trace.clear();
    blake3pp::hasher h;
    if (parallel) {
      blake3pp::update_file(h, f.path, sched, opts);
    } else {
      blake3pp::update_file(h, f.path, opts);
    }
    CHECK(h.finalize() == blake3pp::hash(content));

    const auto recs = trace.windows();
    REQUIRE(recs.size() == n);
    CHECK(trace.dropped_windows() == 0);
    std::size_t last_flags = 0;
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
      const auto& r = recs[i];
      CAPTURE(i);
      CHECK(r.index == i);
      bytes += r.bytes;
      // Ordering holds within a record; nothing is asserted between
      // records, which a driver with several windows in flight may
      // complete in any order.
      CHECK(r.t_wait_begin <= r.t_ready);
      CHECK(r.t_ready <= r.t_joined);
      CHECK(r.t_joined <= r.t_absorbed);
      CHECK(r.t_absorbed <= r.t_released);
      if (r.flags & blake3pp::window_record::flag_last) {
        ++last_flags;
        CHECK(i == recs.size() - 1);
      }
      if (parallel && !(r.flags & blake3pp::window_record::flag_last)) {
        // 64 chunks in 16-chunk parts: fanned out over the scheduler.
        CHECK((r.flags & blake3pp::window_record::flag_parallel) != 0);
        CHECK(r.agents_active >= 1);
        CHECK(r.agent_busy_ns > 0);
      } else {
        CHECK((r.flags & blake3pp::window_record::flag_parallel) == 0);
        CHECK(r.agents_active == 0);
        CHECK(r.agent_busy_ns == 0);
        CHECK(r.t_joined == r.t_absorbed);
      }
    }
    CHECK(last_flags == 1);
    CHECK(bytes == len);
  }
}

TEST_CASE("the read counters are kept whether or not a window is recorded") {
  // driver_stats reports these whether or not the caller asked for
  // per-window records: a buffer with no window storage records nothing
  // per window and still has to come back with the device's time in
  // it.
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t window = 64 * 1024;
  const std::size_t len = 9 * window;
  const auto content = make_input(len);
  const temp_file f(content);

  blake3pp::trace_buffer trace({});
  const blake3pp::file_io_options opts{.window_bytes = window,
                                       .queue_depth = 3,
                                       .trace = &trace};
  blake3pp::hasher h;
  blake3pp::update_file(h, f.path, sched, opts);
  CHECK(h.finalize() == blake3pp::hash(content));

  const auto& d = trace.driver();
  CHECK(trace.windows().empty());
  CHECK(trace.dropped_windows() == window_count(len, window));
  CHECK(d.transfers_landed > 0);
  CHECK(d.transfer_bytes == len);
  CHECK(d.device_wait_ns > 0);
  // The depth is the request, start to finish.
  CHECK(d.queue_depth_final == 3);
  CHECK(d.queue_depth_peak == 3);
}

TEST_CASE("agent records account for every part of their window") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t window = 256 * 1024;  // 256 chunks, 16 parts
  const std::size_t len = 4 * window + 1;
  const auto content = make_input(len);
  const temp_file f(content);
  const std::size_t n = window_count(len, window);
  std::vector<blake3pp::window_record> windows(n);
  std::vector<blake3pp::agent_record> agents(n * 64);
  blake3pp::trace_buffer trace(windows, agents);
  CHECK(trace.wants_agents());

  blake3pp::hasher h;
  blake3pp::update_file(h, f.path, sched,
                        {.window_bytes = window, .trace = &trace});
  CHECK(h.finalize() == blake3pp::hash(content));
  CHECK(trace.dropped_agents() == 0);

  const auto recs = trace.windows();
  REQUIRE(recs.size() == n);
  for (const auto& w : recs) {
    CAPTURE(w.index);
    std::uint64_t parts = 0;
    std::uint64_t busy = 0;
    std::uint32_t active = 0;
    std::uint32_t folds = 0;
    for (const auto& a : trace.agents()) {
      if (a.window != w.index) {
        continue;
      }
      CHECK(a.file == 0);
      CHECK(a.part_chunks == 16);
      CHECK(a.t_begin >= w.t_ready);
      CHECK(a.t_begin + static_cast<std::int64_t>(a.busy_ns) <= w.t_joined);
      if (a.parts == 0) {
        // The fold, on the agent that finished the bulk last: one per
        // fanned-out window, after every bulk record's work.
        ++folds;
        CHECK(a.min_part_ns == 0);
        CHECK(a.max_part_ns == 0);
        continue;
      }
      ++active;
      parts += a.parts;
      busy += a.busy_ns;
      CHECK(a.min_part_ns <= a.max_part_ns);
      const std::uint64_t floor_ns = a.min_part_ns * a.parts;
      CHECK(floor_ns <= a.busy_ns);
    }
    if (w.flags & blake3pp::window_record::flag_parallel) {
      CHECK(parts == w.bytes / blake3pp::chunk_size / 16);
      CHECK(busy == w.agent_busy_ns);
      CHECK(active == w.agents_active);
      CHECK(folds == 1);
    } else {
      CHECK(active == 0);
      CHECK(folds == 0);
    }
  }
}

TEST_CASE("full trace storage drops records, never bytes") {
  auto sched = blake3pp::get_parallel_scheduler();
  constexpr std::size_t window = 64 * 1024;
  const std::size_t len = 6 * window + 100;
  const auto content = make_input(len);
  const temp_file f(content);
  const std::size_t n = window_count(len, window);
  REQUIRE(n >= 5);
  const auto expected = blake3pp::hash(content);

  std::vector<blake3pp::window_record> windows(2);
  std::vector<blake3pp::agent_record> agents(1);
  blake3pp::trace_buffer trace(windows, agents);
  const blake3pp::hash_file_options opts{.window_bytes = window,
                                       .trace = &trace};

  CHECK(blake3pp::hash_file(f.path, opts) == expected);
  CHECK(trace.windows().size() == 2);
  CHECK(trace.dropped_windows() == n - 2);
  CHECK(trace.agents().empty());  // sequential: no agents
  CHECK(trace.dropped_agents() == 0);

  trace.clear();
  CHECK(blake3pp::hash_file(f.path, sched, opts) == expected);
  CHECK(trace.windows().size() == 2);
  CHECK(trace.dropped_windows() == n - 2);
  CHECK(trace.agents().size() == 1);
  CHECK(trace.dropped_agents() > 0);
  CHECK(trace.agents()[0].parts >= 1);
}

TEST_CASE("trace_buffer::clear keeps the epoch") {
  constexpr std::size_t window = 64 * 1024;
  const auto content = make_input(3 * window);
  const temp_file f(content);
  std::vector<blake3pp::window_record> windows(3);
  std::vector<blake3pp::agent_record> agents(8);
  blake3pp::trace_buffer trace(windows, agents);
  const blake3pp::hash_file_options opts{.window_bytes = window,
                                       .trace = &trace};
  CHECK(trace.epoch_ns() > 0);
  CHECK(trace.now() >= 0);

  (void)blake3pp::hash_file(f.path, opts);
  (void)blake3pp::hash_file(f.path, opts);  // a second call overflows
  REQUIRE(trace.windows().size() == 3);
  CHECK(trace.dropped_windows() == 3);
  const std::int64_t before = trace.windows().back().t_released;

  trace.clear();
  CHECK(trace.windows().empty());
  CHECK(trace.agents().empty());
  CHECK(trace.dropped_windows() == 0);
  CHECK(trace.dropped_agents() == 0);

  (void)blake3pp::hash_file(f.path, opts);
  REQUIRE(trace.windows().size() == 3);
  // Same epoch: the new records come after the old ones on the same axis.
  CHECK(trace.windows().front().t_wait_begin >= before);
}

TEST_CASE("missing file throws system_error") {
  CHECK_THROWS_AS(
      (void)blake3pp::hash_file("/nonexistent/blake3pp/no/such/file"),
      std::system_error);
}

TEST_CASE("reader reports a backend") {
  const auto content = make_input(256 * 1024);
  const temp_file f(content);
  blake3pp::detail::file_reader r(f.path, {});
  CHECK(r.file_size() == content.size());
  MESSAGE("file_reader backend: " << r.backend());
  CHECK(!r.backend().empty());
}

// What a file would get, answered without reading it: the plan's
// geometry and open mode from the size, and the rung the open lands on.
// The size rule shows here as it shows in strace: a small file is never
// opened for the cache bypass, a large one is when asked and the
// filesystem allows, and the name agrees with the mode either way.
TEST_CASE("probe_file_io reports the open mode and the rung without reading") {
  const auto small = make_input(100 * 1000);
  const auto large = make_input(1024 * 1024);
  const temp_file fs(small);
  const temp_file fl(large);

  const auto ps = blake3pp::probe_file_io(fs.path);
  MESSAGE("small: " << ps.backend);
  CHECK(ps.file_bytes == small.size());
  CHECK(!ps.direct_io);
  CHECK(ps.windows == 1);
  CHECK(ps.backend.find("direct") == std::string::npos);
  CHECK(ps.backend.find("nocache") == std::string::npos);

  const auto pl = blake3pp::probe_file_io(fl.path);
  MESSAGE("large: " << pl.backend);
  CHECK(pl.file_bytes == large.size());
  CHECK(pl.direct_io);
  CHECK(pl.window_bytes == blake3pp::detail::rounded_window_bytes(
                               blake3pp::file_io_options{}.window_bytes));
  CHECK(pl.reads_in_flight == blake3pp::detail::clamp_queue_depth(
                                  blake3pp::file_io_options{}.queue_depth));
  CHECK(pl.windows == 1);
  CHECK(!pl.backend.empty());

  // The caller's refusal is final, whatever the size.
  const auto pb = blake3pp::probe_file_io(fl.path, {.direct_io = false});
  CHECK(!pb.direct_io);
  CHECK(pb.backend.find("direct") == std::string::npos);

  // The geometry is the plan's: a request is rounded as the pipeline
  // rounds it, and a file gets no more windows than it has.
  const auto pg = blake3pp::probe_file_io(
      fl.path, {.window_bytes = 100 * 1024, .queue_depth = 8});
  CHECK(pg.window_bytes == 64 * 1024);
  CHECK(pg.reads_in_flight == 8);
  CHECK(pg.windows == 16);

  // A missing file is reported both ways.
  const std::filesystem::path missing = fs.path.string() + ".missing";
  CHECK_THROWS_AS((void)blake3pp::probe_file_io(missing), std::system_error);
  std::error_code ec;
  const auto pm = blake3pp::probe_file_io(missing, ec);
  CHECK(ec);
  CHECK(pm.backend.empty());
  CHECK(pm.file_bytes == 0);
}

// The engine table itself: every enumerator has two real names and both
// of them come back. A .def row with a copy-pasted name, or a to_string()
// that forgot a case, fails here rather than in a user's log.
TEST_CASE("every io_engine round-trips through both of its names") {
  const std::span<const blake3pp::io_engine> all{every_io_engine};
  REQUIRE(!all.empty());
  constexpr auto wr = blake3pp::io_direction::write;

  for (const auto e : all) {
    const std::string_view r = blake3pp::to_string(e);
    const std::string_view w = blake3pp::to_string(e, wr);
    CAPTURE(r);
    CAPTURE(w);
    CHECK(!r.empty());
    CHECK(!w.empty());
    CHECK(r != "unknown");
    CHECK(w != "unknown");
    const auto from_read = blake3pp::io_engine_from_string(r);
    const auto from_write = blake3pp::io_engine_from_string(w);
    REQUIRE(from_read.has_value());
    REQUIRE(from_write.has_value());
    CHECK(static_cast<int>(*from_read) == static_cast<int>(e));
    CHECK(static_cast<int>(*from_write) == static_cast<int>(e));
  }

  // from_string() can only be an inverse if no two engines spell
  // themselves alike -- and not just column by column. It parses EITHER
  // column, because the engine token of a `backend` string does not say
  // which way the bytes went, so one engine's write spelling colliding
  // with another's read spelling would resolve a real name to the wrong
  // enumerator. "pwrite" meaning posix_sync and "gcd-write" meaning gcd
  // is what keeps that from happening; a .def row that spelled the GCD
  // write side "pwrite" would be caught here and nowhere else.
  for (const auto a : all) {
    for (const auto b : all) {
      if (a == b) {
        continue;
      }
      CAPTURE(blake3pp::to_string(a));
      CAPTURE(blake3pp::to_string(b));
      CHECK(std::string_view(blake3pp::to_string(a)) !=
            std::string_view(blake3pp::to_string(b)));
      CHECK(std::string_view(blake3pp::to_string(a, wr)) !=
            std::string_view(blake3pp::to_string(b, wr)));
      CHECK(std::string_view(blake3pp::to_string(a, wr)) !=
            std::string_view(blake3pp::to_string(b)));
    }
  }

  CHECK(!blake3pp::io_engine_from_string("").has_value());
  CHECK(!blake3pp::io_engine_from_string("no_such_engine").has_value());
}

// The preference rank of each engine, from the same table the library
// reads: the ladder's order is a claim about these, and the claim is
// worth checking from outside the translation unit that makes it.
constexpr unsigned engine_rank(blake3pp::io_engine e) {
  switch (e) {
#define BLAKE3PP_IO_ENGINE(enumerator, read_name, write_name, rank, async) \
  case blake3pp::io_engine::enumerator:                                    \
    return (rank);
#include <blake3pp/detail/io_engine.def>
#undef BLAKE3PP_IO_ENGINE
  }
  return 0;
}

// Whether a list contains an engine, spelled once.
bool carries(std::span<const blake3pp::io_engine> list,
             blake3pp::io_engine e) {
  return std::ranges::find(list, e) != list.end();
}

// The same invariants the dispatch side has, on the I/O enum, plus the
// two the ladder adds: the compiled list descends the preference order
// because it IS the chain of what-defers-to-what, and the available
// list is a SUFFIX of it rather than any old subset -- a context
// engaged the rung it did because everything above was refused and
// everything below is what it would have degraded to in turn. A
// filtered-but-not-suffix answer would mean the ladder and the kernel
// disagree about which way is down.
TEST_CASE("io engine introspection: the ladder, what it grants, the best") {
  const auto compiled = blake3pp::compiled_io_engines();
  const auto available = blake3pp::available_io_engines();
  REQUIRE(!compiled.empty());
  REQUIRE(!available.empty());

  std::string names;
  for (const auto e : compiled) {
    names += std::format(" {}{}", blake3pp::to_string(e),
                         blake3pp::is_available(e) ? "" : "[no os support]");
  }
  MESSAGE("compiled io engines:"
          << names << " (auto -> "
          << std::string(blake3pp::to_string(blake3pp::best_io_engine()))
          << ")");

  // A ladder descends: each rung is ranked strictly below the one above
  // it, which is both "best-first" and "no engine named twice".
  for (std::size_t i = 1; i < compiled.size(); ++i) {
    CAPTURE(blake3pp::to_string(compiled[i - 1]));
    CAPTURE(blake3pp::to_string(compiled[i]));
    CHECK(engine_rank(compiled[i]) > engine_rank(compiled[i - 1]));
  }

  // Nothing asynchronous hangs below a synchronous rung, so a refusal
  // anywhere on the ladder lands on a synchronous rung.
  bool seen_sync = false;
  for (const auto e : compiled) {
    CAPTURE(blake3pp::to_string(e));
    CHECK(!(seen_sync && blake3pp::is_async(e)));
    seen_sync = seen_sync || !blake3pp::is_async(e);
  }

  // available is the ladder from the engaged rung down -- position and
  // contents, not merely membership.
  const auto at = std::ranges::find(compiled, blake3pp::best_io_engine());
  REQUIRE(at != compiled.end());
  CHECK(std::ranges::equal(
      available, compiled.subspan(
                     static_cast<std::size_t>(at - compiled.begin()))));
  CHECK(static_cast<int>(blake3pp::best_io_engine()) ==
        static_cast<int>(available.front()));

  // is_available() answers for the whole enum, not just the compiled
  // part: an engine this build does not carry is not available however
  // much the OS would like it.
  for (const auto e : every_io_engine) {
    CAPTURE(blake3pp::to_string(e));
    CHECK(blake3pp::is_available(e) == carries(available, e));
    if (!carries(compiled, e)) {
      CHECK(!blake3pp::is_available(e));
    }
  }

  // The floor: the worst compiled engine is a synchronous one, and no
  // kernel refuses it, which is why available_io_engines() is never
  // empty.
  CHECK(!blake3pp::is_async(compiled.back()));
  CHECK(blake3pp::is_available(compiled.back()));

  // The verdict does not move under the library's own I/O. Real opens
  // in between are the interesting part: the probe constructs a context
  // of exactly the kind a hash does, so a verdict that flapped would
  // make two --version runs of the same process disagree.
  const auto before = blake3pp::best_io_engine();
  {
    const auto content = make_input(256 * 1024);
    const temp_file f(content);
    (void)blake3pp::hash_file(f.path);
  }
  CHECK(static_cast<int>(blake3pp::best_io_engine()) ==
        static_cast<int>(before));
  CHECK(std::ranges::equal(blake3pp::available_io_engines(), available));
}

// is_async() is the column --no-async is built from, so both answers are
// asserted: the five engines that hand a transfer to the OS, and the
// three synchronous ones. Flipping a column in the .def fails this
// test.
TEST_CASE("is_async marks the engines that hand the transfer to the OS") {
  CHECK(blake3pp::is_async(blake3pp::io_engine::io_uring));
  CHECK(blake3pp::is_async(blake3pp::io_engine::ioring));
  CHECK(blake3pp::is_async(blake3pp::io_engine::iocp));
  CHECK(blake3pp::is_async(blake3pp::io_engine::gcd));
  CHECK(blake3pp::is_async(blake3pp::io_engine::aio));
  CHECK(!blake3pp::is_async(blake3pp::io_engine::posix_sync));
  CHECK(!blake3pp::is_async(blake3pp::io_engine::win_sync));
  CHECK(!blake3pp::is_async(blake3pp::io_engine::stdio));

  // The half that ties the column to behaviour: a reader told not to be
  // asynchronous lands on a rung is_async() calls synchronous. Without
  // this the column could be a fiction that only the help text reads.
  const auto content = make_input(256 * 1024);
  const temp_file f(content);
  blake3pp::detail::file_reader r(f.path,
                                  {.direct_io = false, .async = false});
  const auto e = blake3pp::io_engine_from_string(engine_token(r.backend()));
  CAPTURE(r.backend());
  REQUIRE(e.has_value());
  CHECK(!blake3pp::is_async(*e));
}

// The engine token of a real open is exactly the name best_io_engine()
// gives it. This ties the enumeration to the strings the tools print, and
// fails if a backend is added without a row in io_engine.def.
//
// The case names no engine to expect. Whatever this kernel grants (the
// ring, the port, libdispatch, AIO, or the synchronous rung because the
// async one was refused), the two answers are produced by different code,
// one by a backend's name_for() and the other by the .def table and a
// throwaway context, and have to agree.
TEST_CASE("a probe's engine token is the name best_io_engine() gives") {
  const auto expected =
      std::string(blake3pp::to_string(blake3pp::best_io_engine()));
  CAPTURE(expected);

  const auto large = make_input(1024 * 1024);
  const temp_file fl(large);
  const auto pl = blake3pp::probe_file_io(fl.path);
  CAPTURE(pl.backend);
  REQUIRE(!pl.backend.empty());
  CHECK(engine_token(pl.backend) == expected);

  const auto parsed = blake3pp::io_engine_from_string(engine_token(pl.backend));
  REQUIRE(parsed.has_value());
  CHECK(static_cast<int>(*parsed) ==
        static_cast<int>(blake3pp::best_io_engine()));

  // The decorations move with the open; the engine does not. A file too
  // small for the bypass and a caller who refused it land on the same
  // engine as the direct open above.
  const auto small = make_input(100 * 1000);
  const temp_file fsm(small);
  CHECK(engine_token(blake3pp::probe_file_io(fsm.path).backend) == expected);
  CHECK(engine_token(blake3pp::probe_file_io(fl.path, {.direct_io = false})
                         .backend) == expected);

  // The write side names itself from the same table. It is held to the
  // weaker claim -- an engine this build carries -- because a write can
  // lose the engine for reasons a read cannot (a share mode, a length
  // the engine will not take), and that is the per-file half.
  const temp_file out({});
  const blake3pp::write_report wrote = blake3pp::write_file(
      out.path, 256 * 1024,
      [](std::span<std::byte> buf, std::uint64_t) {
        std::ranges::fill(buf, std::byte{0x5a});
      });
  CAPTURE(wrote.backend);
  const auto w = blake3pp::io_engine_from_string(engine_token(wrote.backend));
  REQUIRE(w.has_value());
  CHECK(std::ranges::find(blake3pp::compiled_io_engines(), *w) !=
        blake3pp::compiled_io_engines().end());
}

// The oracle that ties the published list to the backends actually
// linked, which is the thing a hand-written list could never have.
//
// Every context in src/io/ declares two compile-time facts -- the
// engine it IS and the rung it defers to -- and
// blake3pp::compiled_io_engines() is nothing but the chain of those
// from the platform's own context. A declaration is still a sentence
// someone wrote, so this checks it against the OTHER place the same
// backend spells its engine: the name() it writes into a file's
// `backend` string, composed by its own name_for() and never by this
// table. The two are produced by different code from different inputs
// and have to agree, for every backend this platform links and not
// only the one it selects.
//
// Asking twice is what makes it cover the whole ladder: with asynchrony
// requested, a context lands on its top rung or on the one below
// depending on the kernel; with --no-async it lands on the rung below
// by construction, which is how the floor gets checked on a machine
// that has the async engine.
template <class Ctx>
void check_rung_declaration(const char* what, const fs::path& path) {
  namespace io_impl = blake3pp::detail::io_impl;
  CAPTURE(what);
  for (const bool async : {true, false}) {
    CAPTURE(async);
    const io_impl::reader_context_options opts{.async = async};
    Ctx ctx(opts, 4);
    const typename Ctx::file f(ctx, path, io_impl::file_open_options{});
    const blake3pp::io_engine engaged = ctx.engaged_rung().engine;
    const std::string name(f.name());
    CAPTURE(name);
    CHECK(engine_token(name) == std::string(blake3pp::to_string(engaged)));
    const auto parsed = blake3pp::io_engine_from_string(engine_token(name));
    REQUIRE(parsed.has_value());
    CHECK(static_cast<int>(*parsed) == static_cast<int>(engaged));
    if (!async) {
      CHECK(!blake3pp::is_async(engaged));
    }
  }
}

TEST_CASE("every linked backend names the rung it declares") {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = make_input(256 * 1024);
  const temp_file f(content);

  check_rung_declaration<io_impl::stdio_context>("stdio", f.path);
#if defined(__unix__) || defined(__APPLE__)
  check_rung_declaration<io_impl::pread_context>("pread", f.path);
#endif
#if defined(__linux__)
  check_rung_declaration<io_impl::uring_context>("uring", f.path);
#endif
#if defined(__FreeBSD__)
  check_rung_declaration<io_impl::aio_context>("aio", f.path);
#endif
#if defined(__APPLE__)
  check_rung_declaration<io_impl::gcd_context>("gcd", f.path);
#endif
#if defined(_WIN32)
  check_rung_declaration<io_impl::iocp_context>("iocp", f.path);
  check_rung_declaration<io_impl::ioring_context>("ioring", f.path);
#endif
  // The one the list is actually read off. On most platforms this is
  // one of the above again; on Windows it is the composition of two of
  // them, which is the case the list has to get right.
  check_rung_declaration<io_impl::native_io_context>("native", f.path);

  // And the list against that context: what a fresh context engages is
  // what best_io_engine() reports, with the public call told the answer
  // by nothing but its own probe. A probe that reported the floor
  // unconditionally fails this on every machine that has an async
  // engine.
  {
    const io_impl::native_io_context ctx(io_impl::reader_context_options{}, 4);
    CAPTURE(blake3pp::to_string(ctx.engaged_rung().engine));
    CHECK(carries(blake3pp::compiled_io_engines(),
                  ctx.engaged_rung().engine));
    CHECK(static_cast<int>(ctx.engaged_rung().engine) ==
          static_cast<int>(blake3pp::best_io_engine()));
  }
  // The bottom of the list is the rung the native context falls to when
  // nothing is asked of the OS -- the claim compiled_io_engines() makes
  // by ending where it does.
  {
    const io_impl::native_io_context ctx(
        io_impl::reader_context_options{.async = false}, 4);
    CAPTURE(blake3pp::to_string(ctx.engaged_rung().engine));
    CHECK(static_cast<int>(ctx.engaged_rung().engine) ==
          static_cast<int>(blake3pp::compiled_io_engines().back()));
  }
}

// The engaged rung, asserted.
//
// backend() exists so a degradation is visible. An async path that can
// never engage passes every test on the bytes: a Windows writer whose
// two handles' share modes collide sends every write down the
// synchronous rung and still writes the right file.
//
// With async asked for and direct I/O out of the picture, the name must
// either name the engine this kernel grants or say why it could not be
// had. A silent fallback fails here; a fallback that explains itself
// passes.
//
// Which engine that is comes from blake3pp::best_io_engine(), so the
// case needs no list of platform engines of its own.
TEST_CASE("an engaged backend is the engine this kernel grants, or says why not") {
  const std::string engine =
      blake3pp::to_string(blake3pp::best_io_engine());
  const std::string engine_write = blake3pp::to_string(
      blake3pp::best_io_engine(), blake3pp::io_direction::write);
  CAPTURE(engine);

  // A degradation the backend could explain carries the reason in
  // brackets, as uring_backend.hpp's no_uring_suffix() does.
  const auto engaged_or_explained = [](std::string_view name,
                                       std::string_view expect) {
    return name.find(expect) != std::string_view::npos ||
           name.find('[') != std::string_view::npos;
  };

  const auto content = make_input(256 * 1024);
  {
    const temp_file f(content);
    blake3pp::detail::file_reader r(
        f.path, {.direct_io = false, .async = true});
    const std::string_view name = r.backend();
    CAPTURE(name);
    CHECK(engaged_or_explained(name, engine));
  }
  {
    const temp_file f({});  // placeholder path; the writer recreates it
    blake3pp::detail::file_writer w(
        f.path, {.buffer_bytes = 64 * 1024,
                 .queue_depth = 2,
                 .direct_io = false,
                 .async = true});
    const std::string_view name = w.backend();
    CAPTURE(name);
    CHECK(engaged_or_explained(name, engine_write));
    auto b = w.acquire();
    std::memcpy(b.data, content.data(), 4096);
    w.submit(b, 4096);
    w.finish();
  }
}

#if defined(__linux__)
// Closing the ring fd does not wait for the reads it still owes, so a
// ring torn down with a read in flight lets the kernel complete into
// memory its owner has already freed. destroy() has to reap them first,
// so after it returns the buffer holds the file. The read is large enough
// that it cannot have finished in the microseconds destroy() itself
// takes, so a missing drain fails here rather than by luck.
TEST_CASE("io_uring destroy reaps in-flight reads before the ring goes") {
  using namespace blake3pp::detail::io_impl;
  const auto content = make_input(32 * 1024 * 1024);
  const temp_file f(content);
  uring ring;
  if (!ring.init(4)) {
    MESSAGE("io_uring unavailable (" << no_uring_suffix(ring.setup_errno)
                                     << "): skipped");
    return;
  }
  posix_file file;
  file.open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
  file.try_odirect(f.path.c_str(), O_RDONLY | O_CLOEXEC);
  const aligned_buffer buf = make_aligned_buffer(content.size());
  std::memset(buf.get(), 0, content.size());
  ring.submit_rw(IORING_OP_READ, file.fd(), buf.get(),
                 static_cast<unsigned>(content.size()), 0, 0, true);
  ring.destroy();
  CHECK(ring.outstanding == 0);
  CHECK(std::equal(content.begin(), content.end(), buf.get()));
}
#endif

// The degradation ladder below io_uring: synchronous backends must produce
// identical windows. They run rarely in the wild, on kernels without
// io_uring and in seccomp sandboxes, which is why these tests reach for
// them explicitly.
TEST_CASE("reader fallback backends deliver identical data") {
  const auto content = make_input(300 * 1024 + 77);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);

  for (const bool direct : {true, false}) {
    CAPTURE(direct);
    blake3pp::detail::file_reader r(
        f.path, {.window_bytes = 64 * 1024, .queue_depth = 2,
                 .direct_io = direct, .async = false});
    MESSAGE("sync backend: " << r.backend());
    blake3pp::hasher h;
    while (auto w = r.next()) {
      h.update(std::span<const std::byte>{w->data, w->bytes});
      r.release(*w);
    }
    CHECK(h.finalize() == expected);
  }
  // The async path with and without the io-wq hand-off (a no-op where the
  // backend has no such notion) must deliver the same bytes.
  for (const bool offload : {true, false}) {
    CAPTURE(offload);
    blake3pp::detail::file_reader r(
        f.path, {.window_bytes = 64 * 1024, .queue_depth = 2,
                 .direct_io = false, .async = true,
                 .offload_submit = offload});
    MESSAGE("async backend: " << r.backend());
    blake3pp::hasher h;
    while (auto w = r.next()) {
      h.update(std::span<const std::byte>{w->data, w->bytes});
      r.release(*w);
    }
    CHECK(h.finalize() == expected);
  }
}

// At most queue_depth windows may be held at once. Asking for one more
// is refused rather than indexing past the slot table, and the reader
// carries on once a window comes back.
TEST_CASE("reader refuses a window beyond queue_depth and recovers") {
  const auto content = make_input(256 * 1024 + 5);
  const temp_file f(content);
  const auto expected = blake3pp::hash(content);
  blake3pp::detail::file_reader r(
      f.path, {.window_bytes = 64 * 1024, .queue_depth = 2});
  auto w0 = r.next();
  auto w1 = r.next();
  REQUIRE(w0);
  REQUIRE(w1);
  CHECK_THROWS_AS(r.next(), std::system_error);
  blake3pp::hasher h;
  h.update(std::span<const std::byte>{w0->data, w0->bytes});
  r.release(*w0);
  h.update(std::span<const std::byte>{w1->data, w1->bytes});
  r.release(*w1);
  while (auto w = r.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    r.release(*w);
  }
  CHECK(h.finalize() == expected);
}

// release() is noexcept, so a window the reader never handed out (or one
// handed back twice) is latched and reported by the next next().
TEST_CASE("reader latches a release it did not hand out") {
  const auto content = make_input(256 * 1024);
  const temp_file f(content);
  SUBCASE("foreign slot") {
    blake3pp::detail::file_reader r(
        f.path, {.window_bytes = 64 * 1024, .queue_depth = 2});
    auto w = r.next();
    REQUIRE(w);
    auto bogus = *w;
    bogus.slot = 7;
    r.release(bogus);
    CHECK_THROWS_AS(r.next(), std::system_error);
  }
  SUBCASE("released twice") {
    blake3pp::detail::file_reader r(
        f.path, {.window_bytes = 64 * 1024, .queue_depth = 2});
    auto w = r.next();
    REQUIRE(w);
    r.release(*w);
    r.release(*w);
    CHECK_THROWS_AS(r.next(), std::system_error);
  }
  // The one the slot index alone cannot catch: a stale window naming a slot
  // that has since been reassigned. Accepting it recycles the buffer the
  // caller is reading from -- a wrong digest, no error anywhere -- so the
  // window's offset has to match what the slot holds.
  SUBCASE("stale window whose slot was reassigned") {
    blake3pp::detail::file_reader r(
        f.path, {.window_bytes = 64 * 1024, .queue_depth = 2});
    const auto w0 = r.next();
    REQUIRE(w0);
    r.release(*w0);          // slot now carries a later window
    const auto w1 = r.next();
    REQUIRE(w1);
    const auto w2 = r.next();
    REQUIRE(w2);
    REQUIRE(w2->slot == w0->slot);  // the same slot, a different window
    r.release(*w0);                 // the stale one
    CHECK_THROWS_AS(r.next(), std::system_error);
  }
}

// A submit larger than the buffer it names would hand the kernel a span
// running into the next slot. It is refused instead.
TEST_CASE("writer refuses a submit larger than its buffer") {
  const temp_file f({});
  blake3pp::detail::file_writer w(
      f.path, {.buffer_bytes = 64 * 1024, .queue_depth = 2});
  auto b = w.acquire();
  CHECK_THROWS_AS(w.submit(b, b.capacity + 1), std::system_error);
  auto foreign = b;
  foreign.slot = 9;
  CHECK_THROWS_AS(w.submit(foreign, 4096), std::system_error);
  w.finish();
}

// Two buffers in hand are two slots, and a buffer may be submitted once.
// A slot claimed only at submit would let a second acquire() hand out the
// same buffer again: both writes would ride one busy flag, the first
// completion would release it, and the producer would refill bytes the
// device was still reading, a wrong file reported as success.
TEST_CASE("writer hands out one buffer per slot and takes each back once") {
  const auto content = make_input(128 * 1024);
  const temp_file f({});
  // The writer owns the file while it lives, and Windows will not open a
  // second handle to it, so the read-back is outside the scope.
  {
    blake3pp::detail::file_writer w(
        f.path, {.buffer_bytes = 64 * 1024, .queue_depth = 2});
    const auto a = w.acquire();
    const auto b = w.acquire();
    CHECK(a.slot != b.slot);
    CHECK(a.data != b.data);
    // Every slot is held; the third ask says so rather than reusing one.
    CHECK_THROWS_AS(w.acquire(), std::system_error);

    std::copy_n(content.data(), 64 * 1024, a.data);
    std::copy_n(content.data() + 64 * 1024, 64 * 1024, b.data);
    w.submit(a, 64 * 1024);
    CHECK_THROWS_AS(w.submit(a, 64 * 1024), std::system_error);  // taken back
    w.submit(b, 64 * 1024);
    w.finish();
  }
  CHECK(std::filesystem::file_size(f.path) == content.size());
  CHECK(blake3pp::hash_file(f.path) == blake3pp::hash(content));
}

// The writer's async path with and without the io-wq hand-off must land
// the same bytes on disk.
TEST_CASE("writer submit modes deliver identical data") {
  const auto content = make_input(300 * 1024 + 77);
  const auto expected = blake3pp::hash(content);
  for (const bool offload : {true, false}) {
    CAPTURE(offload);
    const temp_file f({});  // placeholder path; the writer recreates it
    {
      blake3pp::detail::file_writer w(
          f.path, {.buffer_bytes = 64 * 1024, .queue_depth = 2,
                   .direct_io = false, .async = true,
                   .offload_submit = offload});
      MESSAGE("writer backend: " << w.backend());
      std::size_t done = 0;
      while (done < content.size()) {
        auto b = w.acquire();
        const std::size_t n = std::min(b.capacity, content.size() - done);
        std::memcpy(b.data, content.data() + done, n);
        w.submit(b, n);
        done += n;
      }
      CHECK(w.bytes_written() == content.size());
      w.finish();
      CHECK(w.bytes_written() == content.size());
    }
    CHECK(blake3pp::hash_file(f.path) == expected);
  }
}

// Writes `content` through polled_writer_engine<C>, reads it back
// through polled_reader_engine<C>, and checks byte identity. Small
// buffers and low queue depth force slot recycling; the odd length
// forces an unaligned tail through the deferred path. One context type
// serves both directions.
template <class C>
void roundtrip_engines(std::string_view tag) {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = make_input(300 * 1024 + 77);
  const temp_file f({});  // placeholder path; the writer recreates it

  {
    io_impl::polled_writer_engine<C> w(
        f.path, {.buffer_bytes = 64 * 1024, .queue_depth = 2,
                 .preallocate_bytes = content.size()});
    MESSAGE(tag << " writer backend: " << w.backend_name());
    std::size_t pos = 0;
    while (pos < content.size()) {
      const auto b = w.acquire();
      const std::size_t n = std::min(b.capacity, content.size() - pos);
      std::memcpy(b.data, content.data() + pos, n);
      w.submit(b, n);
      pos += n;
    }
    w.finish();
    CHECK(w.bytes_written() == content.size());
  }

  io_impl::polled_reader_engine<C> r(
      f.path, {.window_bytes = 64 * 1024, .queue_depth = 2});
  MESSAGE(tag << " reader backend: " << r.backend_name());
  CHECK(r.file_size() == content.size());
  std::vector<std::byte> out;
  while (auto win = r.next()) {
    out.insert(out.end(), win->data, win->data + win->bytes);
    r.release(*win);
  }
  CHECK(out == content);
}

// The engines are templates over the backend concepts, so the backends
// the selector ignores on this platform still get concept-checked (the
// definition-site static_asserts in their headers fire on #include) and
// driven through the full engine machinery: compiled AND executed
// everywhere, not just where the selector picks them.
TEST_CASE("engine templates drive the off-platform backends") {
  namespace io_impl = blake3pp::detail::io_impl;
  roundtrip_engines<io_impl::stdio_context>("stdio");
#if defined(__unix__) || defined(__APPLE__)
  roundtrip_engines<io_impl::pread_context>("pread");
#endif
#if defined(_WIN32)
  // The ring is the one backend the Windows selector reaches only at
  // runtime, so the engines are driven over it here whatever this machine
  // answered: a host without a ring runs the same round trip through the
  // context's synchronous rung.
  roundtrip_engines<io_impl::ioring_context>("ioring");
#endif
}

// finish() promises that every accepted byte has landed, and a backend
// that buffers in user space (stdio) can only keep that promise by
// flushing inside it. The round trip above cannot tell: the writer is
// destroyed, and fclose flushes, before anything reads the file. Here
// the writer stays alive and the file is read by another handle, with
// no preallocation, the configuration in which a skipped flush shows.
TEST_CASE("finish() lands every byte on the stdio backend") {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = make_input(100 * 1024 + 13);
  const temp_file f({});  // placeholder path; the writer recreates it
  io_impl::polled_writer_engine<io_impl::stdio_context> w(
      f.path, {.buffer_bytes = 64 * 1024, .queue_depth = 2});
  std::size_t pos = 0;
  while (pos < content.size()) {
    const auto b = w.acquire();
    const std::size_t n = std::min(b.capacity, content.size() - pos);
    std::memcpy(b.data, content.data() + pos, n);
    w.submit(b, n);
    pos += n;
  }
  w.finish();
  // The writer is still open; the bytes must be visible regardless.
  CHECK(std::filesystem::file_size(f.path) == content.size());
  std::ifstream in(f.path, std::ios::binary);
  std::vector<char> raw(content.size());
  in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
  CHECK(static_cast<std::size_t>(in.gcount()) == content.size());
  CHECK(std::memcmp(raw.data(), content.data(), content.size()) == 0);
}

// Stand-in for boost::filesystem::path: satisfies the foreign_path concept
// structurally, which is exactly how a real boost path would enter.
struct fake_boost_path {
  std::string s;
  const std::string& native() const { return s; }
};

TEST_CASE("foreign path-like types (boost::filesystem shape) forward") {
  const auto content = make_input(128 * 1024 + 7);
  const temp_file f(content);
  const fake_boost_path bp{f.path.string()};
  const auto expected = blake3pp::hash(content);
  CHECK(blake3pp::hash_file(bp) == expected);
  std::error_code ec;
  CHECK(blake3pp::hash_file(bp, ec) == expected);
  CHECK(!ec);
  CHECK(blake3pp::hash_file(bp, blake3pp::get_parallel_scheduler()) ==
        expected);
  std::error_code fec;
  CHECK(blake3pp::hash_file(bp, blake3pp::get_parallel_scheduler(), fec) ==
        expected);
  CHECK(!fec);

  blake3pp::hasher h;
  blake3pp::update_file(h, bp);
  CHECK(h.finalize() == expected);
  h.reset();
  blake3pp::update_file(h, bp, ec);
  CHECK(!ec);
  CHECK(h.finalize() == expected);
  h.reset();
  blake3pp::update_file(h, bp, blake3pp::get_parallel_scheduler());
  CHECK(h.finalize() == expected);
  h.reset();
  blake3pp::update_file(h, bp, blake3pp::get_parallel_scheduler(), fec);
  CHECK(!fec);
  CHECK(h.finalize() == expected);
}

TEST_CASE("digest hex round trip and std::format") {
  const auto d = blake3pp::hash(std::string_view{"round trip"});
  const auto chars = d.to_hex_chars();  // allocation-free variant
  CHECK(std::string_view{chars.data()} == d.to_hex());
  CHECK(chars[64] == '\0');
  const auto parsed = blake3pp::digest::from_hex(d.to_hex());
  REQUIRE(parsed.has_value());
  CHECK(*parsed == d);

  std::string upper = d.to_hex();
  for (char& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  CHECK(blake3pp::digest::from_hex(upper) == d);

  CHECK(!blake3pp::digest::from_hex("abc"));
  CHECK(!blake3pp::digest::from_hex(std::string(64, 'g')));

#if defined(__cpp_lib_format)
  CHECK(std::format("{}", d) == d.to_hex());
#endif
}

TEST_CASE("digest::matches verifies hex in one step") {
  const auto d = blake3pp::hash(std::string_view{"verify me"});
  CHECK(d.matches(d.to_hex()));
  std::string upper = d.to_hex();
  for (char& c : upper) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  CHECK(d.matches(upper));                       // case-insensitive
  CHECK(!d.matches("abc"));                      // malformed: no match
  CHECK(!d.matches(std::string(64, 'g')));       // non-hex: no match
  auto other = d.to_hex();
  other[0] = other[0] == '0' ? '1' : '0';
  CHECK(!d.matches(other));                      // wrong digest

  // The optional<digest> == digest spelling needs no library support:
  // std::optional's heterogeneous comparison handles it (false on nullopt).
  CHECK(blake3pp::digest::from_hex(d.to_hex()) == d);
  CHECK(blake3pp::digest::from_hex("nope") != d);
}

}  // TEST_SUITE

}  // namespace
