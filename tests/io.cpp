#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <ostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <blake3pp/io.hpp>
#include <blake3pp/parallel.hpp>
#include <blake3pp/parallel_io.hpp>
#include <doctest/doctest.h>

// The portable engines and the backends the platform selector never
// picks here: instantiating reader_engine/writer_engine with them below
// keeps the off-platform code compiled AND executed on every platform
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
    static const unsigned run_id = std::random_device{}();
    path = fs::temp_directory_path() /
           ("blake3pp_io_test_" + std::to_string(run_id) + "_" +
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

TEST_CASE("keyed hash_file matches keyed in-memory hashing") {
  std::array<std::byte, 32> key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(200 - i);
  }
  const auto content = make_input(2 * 1024 * 1024 + 999);
  const temp_file f(content);
  const auto expected =
      blake3pp::keyed_hash(std::span<const std::byte, 32>{key}, content);

  CHECK(blake3pp::hash_file(f.path, {.key = key}) == expected);
  CHECK(blake3pp::hash_file(f.path, blake3pp::get_parallel_scheduler(),
                            {.window_bytes = 1024 * 1024, .key = key}) ==
        expected);
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

#if defined(__linux__)
// Closing the ring fd does not wait for the reads it still owes, so a
// ring torn down with a read in flight lets the kernel complete into
// memory its owner has already freed. destroy() has to reap them first:
// after it returns, the buffer holds the file. The read is large enough
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
  aligned_pool buf(content.size());
  std::memset(buf.data, 0, content.size());
  ring.submit_rw(IORING_OP_READ, file.fd, buf.data,
                 static_cast<unsigned>(content.size()), 0, 0, true);
  ring.destroy();
  CHECK(ring.outstanding == 0);
  CHECK(std::equal(content.begin(), content.end(), buf.data));
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
}

// A submit larger than the buffer it names would hand the kernel a span
// running into the next slot; it is refused instead.
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
      w.finish();
    }
    CHECK(blake3pp::hash_file(f.path) == expected);
  }
}

// Writes `content` through writer_engine<W>, reads it back through
// reader_engine<R>, and checks byte identity. Small buffers and low queue
// depth force slot recycling; the odd length forces an unaligned tail
// through the sync path.
template <class R, class W>
void roundtrip_engines(std::string_view tag) {
  namespace io_impl = blake3pp::detail::io_impl;
  const auto content = make_input(300 * 1024 + 77);
  const temp_file f({});  // placeholder path; the writer recreates it

  {
    io_impl::writer_engine<W> w(
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

  io_impl::reader_engine<R> r(f.path,
                              {.window_bytes = 64 * 1024, .queue_depth = 2});
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
  roundtrip_engines<io_impl::stdio_reader, io_impl::stdio_writer>("stdio");
#if defined(__unix__) || defined(__APPLE__)
  roundtrip_engines<io_impl::pread_reader, io_impl::pread_writer>("pread");
#endif
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
