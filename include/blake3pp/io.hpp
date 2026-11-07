#pragma once

/// @file
/// File hashing at storage speed: the windowed pipeline that joins the
/// file_reader to the compute engine. The reader uses io_uring +
/// O_DIRECT, the I/O ring or IOCP + NO_BUFFERING, GCD + F_NOCACHE, or
/// POSIX AIO + O_DIRECT, whatever the platform allows.
///
/// While window i is being hashed, windows i+1..i+depth-1 are already
/// streaming in. The queue-depth buffer ring is the backpressure
/// mechanism, so the pipeline allocates nothing past setup.
///
/// The primitive is `update_file()`, which is `hasher::update()` with a file
/// as the source. It streams into a caller-owned hasher and returns.
/// Because the hasher belongs to the caller, its mode (plain, keyed,
/// derive_key) and every finalize form (digest, extended output, the
/// seekable reader) compose with file input without this header knowing
/// about them. `hash_file()` is the one-shot convenience on top: construct,
/// update_file, finalize.
///
/// This header is free of any execution-provider dependency: core.hpp,
/// dispatch.hpp and io.hpp compile against the standard library alone.
/// Reads still overlap hashing here (the reader is asynchronous); what is
/// sequential is the compute. The scheduler-taking overloads, which fan
/// each window out over cores, and update_stream() for pipes and standard
/// input live in parallel_io.hpp, the one public header that needs
/// stdexec/beman/std::execution.
///
/// std::filesystem::path is the path currency throughout (string literals
/// and std::string convert implicitly). Each entry point follows the
/// standard library's dual-overload idiom: the plain form throws
/// std::system_error on I/O failure, the std::error_code& form reports
/// through ec instead.

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/file_writer.hpp>
#include <blake3pp/detail/io_geometry.hpp>

namespace blake3pp {

class trace_buffer;

/// The pipeline's knobs: what update_file() takes. The hasher it streams
/// into already carries the SIMD variant and the mode.
struct file_io_options {
  /// Bytes per window, 8 MiB by default; rounded down to a power-of-2
  /// multiple of chunk_size, minimum 64 KiB.
  std::size_t window_bytes = detail::default_window_bytes;
  /// Reads in flight at once, four by default; clamped to [2, 256]. Each
  /// window costs window_bytes of arena plus its own tables, which is what
  /// the ceiling guards (and BLAKE3PP_MAX_QUEUE_DEPTH moves). The
  /// sequential forms allocate exactly this many windows.
  ///
  /// The forms that take a scheduler allocate more. The depth is what the
  /// device sees; the window objects behind it are what the hash pool draws
  /// work from while the device serves the next reads, and on a fast device
  /// they are worth more than the depth is. Their number follows the
  /// machine's thread count (detail::read_plan_budget_bytes), and
  /// docs/pipeline.md carries the measurement.
  unsigned queue_depth = detail::default_queue_depth;
  /// Bypass the page cache where the platform supports it; degrades to
  /// buffered reads where it does not. A file below 512 KiB
  /// (detail::min_pipeline_bytes) is read through the page cache either
  /// way: it runs without the pipeline, and buffered is the faster read
  /// there by 5-10%.
  bool direct_io = true;
  /// Issue each read on the kernel's I/O worker threads instead of inline
  /// in the submitting thread (Linux: io_uring's IOSQE_ASYNC). Issuing a
  /// large direct read costs real CPU time, and inline it is paid by the
  /// thread that also drives the hash; kernels since 6.x issue inline
  /// whenever they can, so this asks for the hand-off explicitly. Ignored
  /// where the platform has no such notion.
  bool offload_submit = true;
  /// Queue reads asynchronously where the platform offers it. Off performs
  /// them synchronously on a few I/O threads of the library's own, which
  /// is what a platform with no async engine does anyway. The engines this
  /// gives up are the ones is_async() marks; for a given build,
  /// compiled_io_engines() filtered by is_async() names them. Which rung
  /// was engaged is reported after the fact: read_backend(trace.driver())
  /// for a read, write_report::backend for a write.
  bool async = true;
  /// Windows: prefer the I/O ring (Windows 11, `ioringapi.h`) over the
  /// completion port where the machine has one. Off selects the port,
  /// which is also what a machine without a ring gets; the engaged rung
  /// is reported after the fact and names which one ran.
  bool ioring = true;
  /// Where to record per-window timing (<blake3pp/trace.hpp>); nullptr
  /// records nothing, at the cost of one branch per window.
  ///
  /// Not owned and not copied: the buffer has to outlive the call. The
  /// senders in <blake3pp/async_io.hpp> do not read this field: their
  /// operation state is a type fixed at connect, so they take the tracing
  /// policy as an argument instead (blake3pp::buffer_tracer).
  trace_buffer* trace = nullptr;
};

/// Which BLAKE3 mode a hasher built from options runs in: plain, keyed
/// (MAC/PRF), or key derivation.
///
/// One value rather than one field per mode: a hash is keyed or derived or
/// neither, and asking for two of those does not compile. The mode is named
/// the way hasher spells the same choice, so the two read alike:
///
/// @code
/// blake3pp::hash_file_options plain{};
/// blake3pp::hash_file_options mac{
///     .mode = blake3pp::hash_mode::keyed(my_key)};
/// blake3pp::hash_file_options kdf{
///     .mode = blake3pp::hash_mode::derive_key("fixture v3 2026-09")};
/// @endcode
class hash_mode {
 public:
  /// Which of the three a mode value holds.
  enum class kind { plain, keyed, derive_key };

  /// Plain hashing.
  constexpr hash_mode() noexcept = default;
  /// Plain hashing, named: what a default-constructed mode is.
  [[nodiscard]] static constexpr hash_mode plain() noexcept { return {}; }

  /// Keyed (MAC/PRF) mode: BLAKE3's replacement for HMAC, as
  /// hasher::keyed().
  /// @param key  Exactly key_size bytes, enforced by the span extent.
  [[nodiscard]] static constexpr hash_mode keyed(
      std::span<const std::byte, key_size> key) noexcept {
    hash_mode m;
    m.kind_ = kind::keyed;
    std::ranges::copy(key, m.key_.begin());
    return m;
  }

  /// Key-derivation mode, as hasher::derive_key(): the input is key material
  /// and the output a subkey bound to the context, which should be
  /// hardcoded, globally unique and application-specific (see the BLAKE3
  /// spec); it is not a secret.
  ///
  /// The context is copied, like the key: a mode outlives the expression
  /// that built it -- an options object holds one, and the senders in
  /// <blake3pp/async_io.hpp> hold a copy of the options for the whole
  /// operation -- so a borrowed string would have to outlive all of that,
  /// and nothing at the call site says it does.
  /// @param context  The domain-separation string.
  [[nodiscard]] static hash_mode derive_key(std::string_view context) {
    hash_mode m;
    m.kind_ = kind::derive_key;
    m.context_.assign(context);
    return m;
  }

  /// Which mode this is.
  [[nodiscard]] constexpr kind which() const noexcept { return kind_; }
  /// The key, when which() is keyed.
  [[nodiscard]] constexpr std::span<const std::byte, key_size> key()
      const noexcept {
    return key_;
  }
  /// The context string, when which() is derive_key. A view of this mode's
  /// own copy, valid as long as the mode is.
  [[nodiscard]] std::string_view context() const noexcept { return context_; }

 private:
  std::array<std::byte, key_size> key_{};
  std::string context_;
  kind kind_ = kind::plain;
};

/// hash_file()'s knobs: the pipeline's, plus what shapes the hasher it
/// constructs internally.
///
/// Both structs spell the shared fields the same way so designated
/// initializers read alike, and this one converts to file_io_options so a
/// single options object can drive both entry points (a tool's
/// --window/--qd/--no-direct flags land in one place).
struct hash_file_options {
  /// The SIMD variant of the hasher.
  arch a = arch::auto_detect;
  /// Bytes per window, 8 MiB by default; rounded down to a power-of-2
  /// multiple of chunk_size, minimum 64 KiB.
  std::size_t window_bytes = detail::default_window_bytes;
  /// Windows in flight at once, four by default; clamped to [2, 256]; see
  /// file_io_options.
  unsigned queue_depth = detail::default_queue_depth;
  /// Bypass the page cache where the platform supports it; see
  /// file_io_options for the small-file exception.
  bool direct_io = true;
  /// Issue reads on the kernel's I/O worker threads; see file_io_options.
  bool offload_submit = true;
  /// Queue reads asynchronously where the platform offers it; see
  /// file_io_options.
  bool async = true;
  /// Windows: prefer the I/O ring over the completion port; see
  /// file_io_options.
  bool ioring = true;
  /// Plain, keyed or key-derivation mode: `hash_mode::keyed(key)` or
  /// `hash_mode::derive_key(context)`, named as hasher spells them.
  ///
  /// Extended output has no shortcut here, since hash_file() returns a
  /// 32-byte digest: take the hasher these options describe (make_hasher()),
  /// stream it with update_file() and finalize it to any length.
  hash_mode mode{};
  /// Where to record per-window timing (<blake3pp/trace.hpp>); nullptr
  /// records nothing.
  trace_buffer* trace = nullptr;

  /// The pipeline knobs alone, so one options object drives update_file()
  /// too.
  constexpr operator file_io_options() const noexcept {
    return {.window_bytes = window_bytes,
            .queue_depth = queue_depth,
            .direct_io = direct_io,
            .offload_submit = offload_submit,
            .async = async,
            .ioring = ioring,
            .trace = trace};
  }
};

namespace detail {

inline hasher make_hasher(const hash_file_options& opts,
                          const kern::kernel_ops* ops) noexcept {
  switch (opts.mode.which()) {
    case hash_mode::kind::keyed:
      return hasher::keyed(opts.mode.key(), ops);
    case hash_mode::kind::derive_key:
      return hasher::derive_key(opts.mode.context(), ops);
    case hash_mode::kind::plain:
      break;
  }
  return hasher{ops};
}

// The body of every std::error_code overload: ec is cleared, then set
// from the std::system_error the throwing form raises. Anything else
// escaping the pipeline reads as not_enough_memory: a setup allocation (the
// buffer ring, the queue), whatever an execution provider raises, or a write
// generator's own exception, none of which this seam can tell apart. A
// caller that needs to know takes the throwing form. Value-returning callers
// get a default-constructed result on failure.
template <class F>
auto with_error_code(std::error_code& ec, F&& fn) noexcept
    -> std::invoke_result_t<F> {
  using result = std::invoke_result_t<F>;
  try {
    ec.clear();
    return std::forward<F>(fn)();
  } catch (const std::system_error& e) {
    ec = e.code();
  } catch (...) {
    ec = std::make_error_code(std::errc::not_enough_memory);
  }
  if constexpr (!std::is_void_v<result>) {
    return result{};
  }
}

}  // namespace detail

/// The hasher these options describe: its SIMD variant and its mode.
///
/// hash_file() builds this internally; it is public so a caller that needs
/// a finalize form hash_file() does not have -- extended output, the
/// seekable reader -- can stream the same options through update_file()
/// without respelling the mode selection.
/// @param opts  The variant and mode; the pipeline knobs are ignored here.
///
/// @code
/// blake3pp::hash_file_options opts{
///     .mode = blake3pp::hash_mode::derive_key("fixture v3 2026-09")};
/// blake3pp::hasher h = blake3pp::make_hasher(opts);
/// blake3pp::update_file(h, "seed.bin", opts);
/// auto stream = h.finalize_xof();
/// @endcode
[[nodiscard]] inline hasher make_hasher(
    const hash_file_options& opts) noexcept {
  return detail::make_hasher(opts, detail::resolve(opts.a));
}

/// Streams a file's bytes into a hasher, as `h.update()` would, and returns
/// with h open for more input or any finalize form.
///
/// Files hash in sequence: after update_file(h, a); update_file(h, b);
/// h holds the hash of a's bytes followed by b's. The hasher's mode
/// (plain, keyed, derive_key) applies unchanged.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
/// @throws std::system_error on I/O failure; h is then in an unspecified
///         but valid state (reset() or discard it).
///
/// @code
/// blake3pp::hasher h = blake3pp::hasher::derive_key("fixture v3 2026-09");
/// blake3pp::update_file(h, "seed.bin");
/// auto stream = h.finalize_xof();
/// @endcode
void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts = {});
/// Streams a file's bytes into a hasher, reporting failure through ec
/// instead of throwing.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the I/O error otherwise (allocation
///              failure at setup reads as not_enough_memory).
/// @param opts  The pipeline knobs.
void update_file(hasher& h, const std::filesystem::path& path,
                 std::error_code& ec,
                 const file_io_options& opts = {}) noexcept;

/// What update_file() would engage for a file: the geometry its plan
/// decides from the options and the file's size, and the rung the open
/// lands on. See probe_file_io().
struct file_io_probe {
  /// The file's size as the filesystem reports it.
  std::uint64_t file_bytes = 0;
  /// Bytes per window, as the pipeline rounds the request.
  std::size_t window_bytes = 0;
  /// Reads the device sees at once.
  unsigned reads_in_flight = 0;
  /// Windows in flight at most, reading or being hashed; never more than
  /// the file has.
  unsigned windows = 0;
  /// Whether the open asks for the cache bypass: the caller's direct_io,
  /// granted only to a file of at least 512 KiB (detail::min_pipeline_bytes)
  /// and to a size the filesystem would not give.
  bool direct_io = false;
  /// The rung the open landed on, spelled as write_report::backend is:
  /// an engine token as to_string(io_engine) gives it ("io_uring",
  /// "ioring", "iocp", "gcd", "aio", "pread", "readfile", "stdio"),
  /// then "+direct" or "+nocache" where the cache bypass engaged, then
  /// " (inline submit)" where offload_submit was off and the reason
  /// appended where the async engine was asked for and refused.
  /// Where the bypass was asked for and the filesystem declined it, this
  /// says so by naming the buffered rung while direct_io above is true.
  ///
  /// The leading token is an engine name: to_string(io_engine) spells the
  /// same words from the same table, so `backend` and best_io_engine()
  /// agree for a file that got what a fresh open engages.
  std::string backend;
};

/// Opens the file the way update_file() opens it for a fresh hasher,
/// reports what that open engaged, and closes it; nothing is read. The
/// answer is this filesystem's and this kernel's for this path and these
/// options, which is why it takes an open to give it: a hashing call with
/// the same options gets the same rung. A hasher part-way through a
/// message may lose the bypass to an unaligned window grid, which this
/// probe, asked for a fresh hasher, does not see.
/// @param path  The file to probe.
/// @param opts  The pipeline knobs update_file() would be given.
/// @throws std::system_error when the file cannot be opened.
[[nodiscard]] file_io_probe probe_file_io(const std::filesystem::path& path,
                                          const file_io_options& opts = {});

/// probe_file_io() reporting failure through ec instead of throwing.
/// @param path  The file to probe.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The pipeline knobs update_file() would be given.
/// @return The probe, or a default one when ec is set.
[[nodiscard]] file_io_probe probe_file_io(const std::filesystem::path& path,
                                          std::error_code& ec,
                                          const file_io_options& opts = {}) noexcept;

/// An I/O engine: the mechanism that moves a window of bytes between
/// the device and memory.
///
/// A build carries one native engine per platform plus the synchronous
/// rung it degrades to, except on Windows, which carries both the I/O
/// ring and the completion port and chooses when a context opens.
/// compiled_io_engines() lists what this binary has,
/// available_io_engines() those the running kernel grants, and
/// best_io_engine() the one a fresh open uses. None of the three needs
/// a path; probe_file_io() is for what does.
///
/// The enumerators, their two name spellings and the preference order
/// come from detail/io_engine.def, which documents each entry. Its line
/// order is the ABI order and is append-only.
enum class io_engine : std::uint8_t {
#define BLAKE3PP_IO_ENGINE(enumerator, read_name, write_name, rank, async) \
  enumerator,
#include <blake3pp/detail/io_engine.def>
#undef BLAKE3PP_IO_ENGINE
};

/// Which half of an engine a name is being asked for.
///
/// Three engines spell the two directions differently: "pread" against
/// "pwrite", "readfile" against "writefile", and "gcd" against
/// "gcd-write", because reads and writes take different paths through
/// libdispatch. Pass it to to_string() where the bytes went the other
/// way; the default is the read spelling.
enum class io_direction : std::uint8_t { read, write };

/// True if the engine is in this binary and the running kernel grants
/// it, which is the same as saying available_io_engines() names it.
///
/// There is no os_supports() beside this, as there is a cpu_supports()
/// beside the dispatch twin: the code that asks the kernel about an
/// engine is the backend itself, so a build without that backend has
/// nothing to ask with.
/// @param e  Any engine.
[[nodiscard]] bool is_available(io_engine e) noexcept;

/// The engine a fresh open uses on this machine, which is
/// available_io_engines().front().
///
/// One FILE may still land lower: a filesystem can refuse the cache
/// bypass, and a length the engine will not take asynchronously goes
/// down a rung. probe_file_io() reports that, for a path.
[[nodiscard]] io_engine best_io_engine() noexcept;

/// The engines in this binary, best-first, ending with the synchronous
/// rung the others degrade to.
///
/// The list is the degradation ladder itself, read off the backend
/// types the platform selected: each declares the engine it is and the
/// rung it defers to. An engine appears only if its code is really
/// here, so Windows reports the ring and the port where it has both,
/// and the port alone where <ioringapi.h> was not in the SDK.
[[nodiscard]] std::span<const io_engine> compiled_io_engines() noexcept;

/// The engines this kernel grants, best-first; never empty, and its
/// front() is best_io_engine().
///
/// Answered by constructing the platform's own context with the
/// smallest resources it will take and reading back which rung it
/// engaged. The answer is kept only where the backend says it cannot
/// change: a kernel without io_uring will not grow one, but a setup
/// refused because the descriptor table was full is true only of that
/// moment, and the next call probes again.
[[nodiscard]] std::span<const io_engine> available_io_engines() noexcept;

/// The engine's canonical lowercase name, the word a `backend` string
/// starts with: "io_uring", "ioring", "iocp", "gcd", "aio", "pread",
/// "readfile", "stdio", and the write spellings beside them.
/// @param e  Any engine.
/// @param d  Which half; reads by default.
[[nodiscard]] const char* to_string(
    io_engine e, io_direction d = io_direction::read) noexcept;

/// Parses a name from to_string(), in either direction. Takes both
/// spellings because the engine token of a `backend` string does not
/// say which way the bytes went.
/// @param name  An engine name, as to_string() spells it.
/// @return The engine, or std::nullopt for any other string.
[[nodiscard]] std::optional<io_engine> io_engine_from_string(
    std::string_view name) noexcept;

/// True where the engine hands a transfer to the OS and collects a
/// completion later, false for the rungs that transfer on the calling
/// thread.
///
/// These are the engines file_io_options::async and
/// file_write_options::async give up. Filter compiled_io_engines() by
/// this to name them for a build, as the tools do for their --no-async
/// help text.
/// @param e  Any engine.
[[nodiscard]] bool is_async(io_engine e) noexcept;

/// Knobs for write_file(): the geometry of a generated file.
///
/// On the write side the windows in flight are the whole parallelism,
/// because one window's fill is sequential, so the queue depth is the
/// lever and the window size moves little. The default depth is
/// therefore automatic: over a scheduler, one window per hardware thread
/// as long as depth x window stays within 256 MiB in flight (32 windows
/// of the default 8 MiB), past which a striped volume slows down; in the
/// sequential forms, two, one filling while the other drains.
struct file_write_options {
  /// Bytes per window. The sequential forms round up to a multiple of
  /// 4 KiB (minimum 64 KiB, maximum 1 GiB); the forms that take a
  /// scheduler round down to a power-of-2 multiple of chunk_size, as
  /// reading does, so one options object asking for an odd size can mean
  /// two geometries.
  std::size_t window_bytes = detail::default_write_window_bytes;
  /// Windows in flight, clamped to [2, 256]; zero means automatic, as
  /// described above. A caller that knows its pool is smaller than the
  /// machine, or its device prefers less in flight, sets it.
  unsigned queue_depth = detail::default_write_queue_depth;
  /// Bypass the page cache where the platform can.
  bool direct_io = true;
  /// Queue writes asynchronously where the platform offers it. Off
  /// writes synchronously on the library's own I/O threads, as a platform
  /// with no async engine does anyway. The engines this gives up are the ones is_async() marks;
  /// for a given build, compiled_io_engines() filtered by is_async()
  /// names them.
  bool async = true;
  /// io_uring: issue on io-wq rather than inline in the submit call.
  bool offload_submit = true;
  /// Windows: prefer the I/O ring (Windows 11, `ioringapi.h`) over the
  /// completion port where the machine has one. Off selects the port,
  /// which is also what a machine without a ring gets; the engaged rung
  /// is reported after the fact and names which one ran.
  bool ioring = true;
  /// Windows only: set the valid data length up front instead of making
  /// the file sparse.
  ///
  /// NTFS zeroes every byte between a file's valid data length and a
  /// write that lands beyond it, so that no reader sees the blocks a
  /// deleted file left there; with several writes in flight that zeroing
  /// is most of the write's cost. By default the writer makes the file
  /// sparse for the duration of the run, which turns those gaps into holes
  /// (read as zeros, nothing exposed), and clears the flag once the file is
  /// complete. A run that fails leaves a sparse file with holes.
  ///
  /// This flag sets the valid data length to the preallocation up front
  /// (SetFileValidData), which is faster still. It needs
  /// SeManageVolumePrivilege, which the request enables on the PROCESS
  /// token, and until the writes land the file exposes whatever its disk
  /// blocks held before. Where the privilege is not held, the sparse route
  /// is taken as if the flag were off.
  ///
  /// write_report::backend says which route ran: "+validdata", "+sparse",
  /// or neither where the file system refused sparse files too. Unix
  /// preallocates unwritten extents that read as zeros, and ignores the
  /// flag.
  bool waive_zero_fill = false;
  /// Where to record per-window timing (<blake3pp/trace.hpp>) in the
  /// forms that take a scheduler; nullptr records nothing, and the
  /// sequential forms record nothing either. The stamps carry the write
  /// side's stages: admitted, fill begun on an agent, fill done, write
  /// landed, released. Not owned: the buffer outlives the call.
  trace_buffer* trace = nullptr;
};

/// What a write_file() run did.
struct write_report {
  /// Bytes written: the size that was asked for, or what the generator
  /// produced before it stopped.
  std::uint64_t bytes = 0;
  /// Which mechanism carried them, as the writer names it, in order:
  ///
  /// - the engine, in the write spelling to_string(e, io_direction::write)
  ///   gives ("io_uring", "ioring", "iocp", "gcd-write", "aio", or the
  ///   synchronous "pwrite", "writefile", "stdio");
  /// - "+direct" or "+nocache" where the cache bypass engaged;
  /// - on Windows, "+sparse" or "+validdata" for how the preallocation was
  ///   kept from zeroing ahead of the writes (see
  ///   file_write_options::waive_zero_fill);
  /// - " (inline submit)" where offload_submit was off;
  /// - the reason, where an asynchronous rung was asked for and refused.
  ///
  /// So "io_uring+direct", "iocp+direct+sparse",
  /// "pwrite+direct [io_uring: ENOSYS, kernel too old]", and so on. Owned
  /// by the report, because the writer that composed it is gone by then.
  std::string backend;
};

namespace detail {

// Both sequential write_file() forms: fill one buffer at a time, submit
// it and move on, so the producer fills the next while the device drains
// this one. take(buffer) says how many bytes the generator put in it;
// fewer than the buffer holds ends the file, which is also the only
// position where a direct-I/O write may be unaligned.
template <class Take>
write_report write_sequential(const std::filesystem::path& path,
                              std::uint64_t preallocate, Take take,
                              const file_write_options& opts) {
  file_writer writer(path,
                     {.buffer_bytes = opts.window_bytes,
                      .queue_depth = opts.queue_depth != 0
                                         ? opts.queue_depth
                                         : sequential_write_queue_depth,
                      .direct_io = opts.direct_io,
                      .async = opts.async,
                      .offload_submit = opts.offload_submit,
                      .ioring = opts.ioring,
                      .preallocate_bytes = preallocate,
                      .waive_zero_fill = opts.waive_zero_fill});
  std::uint64_t done = 0;
  for (;;) {
    file_writer::buffer b = writer.acquire();
    const std::size_t n = take(b, done);
    if (n > 0) {
      writer.submit(b, n);
      done += n;
    }
    if (n < b.capacity) {
      break;
    }
  }
  writer.finish();
  return {done, std::string(writer.backend())};
}

}  // namespace detail

/// Generates a file of exactly `size` bytes on the calling thread, writing
/// it through the platform's fastest path.
///
/// `fill` is called as `fill(std::span<std::byte> out, std::uint64_t
/// offset)`, once per window and in file order, and must fill every byte
/// of `out`. It may throw, and the file is then left as far as it got; the
/// fill runs on this thread, so the exception propagates out of this call.
/// The parallel_io.hpp overload runs the windows over a scheduler instead,
/// where a throwing generator is carried by the chain rather than by the
/// stack.
///
/// On success the file is exactly `size` bytes. On failure the file is
/// left as far as it got: what lies past the last landed window is
/// unspecified, and it is NOT truncated, because a length invented for
/// bytes that were never produced would hide the failure.
/// @param path  The file to create or truncate.
/// @param size  How many bytes to generate.
/// @param fill  The generator, called per window.
/// @param opts  The geometry.
/// @return The bytes written and the mechanism that carried them.
/// @throws std::system_error on I/O failure.
///
/// @code
/// blake3pp::write_file("seed.bin", 1 << 30,
///                      [&](std::span<std::byte> out, std::uint64_t off) {
///                        auto r = stream;
///                        r.seek(off);
///                        blake3pp::fill(r, out);
///                      });
/// @endcode
template <class Fill>
  requires std::invocable<Fill&, std::span<std::byte>, std::uint64_t>
write_report write_file(const std::filesystem::path& path, std::uint64_t size,
                        Fill&& fill, const file_write_options& opts = {}) {
  return detail::write_sequential(
      path, size,
      [&](const detail::file_writer::buffer& b, std::uint64_t off) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(b.capacity, size - off));
        // At off == size the writer asks once more, which is how it learns
        // the file ended; the generator owes nothing for that window.
        if (n != 0) {
          fill(std::span{b.data, n}, off);
        }
        return n;
      },
      opts);
}

/// Generates a file whose length the generator decides, on the calling
/// thread.
///
/// `fill` is called as `std::size_t fill(std::span<std::byte> out,
/// std::uint64_t offset)`, once per window and in file order, and returns
/// how many bytes of `out` it produced. Anything short of the whole
/// buffer, zero included, ends the file. It may throw, as in the
/// length-taking form above.
/// @param path  The file to create or truncate.
/// @param fill  The generator; its return value is the file's length.
/// @param opts  The geometry.
/// @return The bytes written and the mechanism that carried them.
/// @throws std::system_error on I/O failure.
template <class Fill>
  requires std::invocable<Fill&, std::span<std::byte>, std::uint64_t> &&
           std::convertible_to<
               std::invoke_result_t<Fill&, std::span<std::byte>, std::uint64_t>,
               std::size_t>
write_report write_file(const std::filesystem::path& path, Fill&& fill,
                        const file_write_options& opts = {}) {
  return detail::write_sequential(
      path, 0,
      [&](const detail::file_writer::buffer& b, std::uint64_t off) {
        return static_cast<std::size_t>(
            fill(std::span{b.data, b.capacity}, off));
      },
      opts);
}

/// write_file() of a known size, reporting failure through ec.
/// @param path  The file to create or truncate.
/// @param size  How many bytes to generate.
/// @param fill  The generator, called per window.
/// @param ec    Cleared on success; the I/O error otherwise.
/// @param opts  The geometry.
/// @return What was written; `bytes` is 0 and `backend` empty when ec is
///         set at setup, otherwise what got out before the failure.
template <class Fill>
  requires std::invocable<Fill&, std::span<std::byte>, std::uint64_t>
write_report write_file(const std::filesystem::path& path, std::uint64_t size,
                        Fill&& fill, std::error_code& ec,
                        const file_write_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return write_file(path, size, std::forward<Fill>(fill), opts);
  });
}

/// write_file() of a generator-decided length, reporting failure through ec.
/// @param path  The file to create or truncate.
/// @param fill  The generator; its return value is the file's length.
/// @param ec    Cleared on success; the I/O error otherwise.
/// @param opts  The geometry.
/// @return What was written; `bytes` is 0 and `backend` empty when ec is
///         set at setup, otherwise what got out before the failure.
template <class Fill>
  requires std::invocable<Fill&, std::span<std::byte>, std::uint64_t> &&
           std::convertible_to<
               std::invoke_result_t<Fill&, std::span<std::byte>, std::uint64_t>,
               std::size_t>
write_report write_file(const std::filesystem::path& path, Fill&& fill,
                        std::error_code& ec,
                        const file_write_options& opts = {}) noexcept {
  return detail::with_error_code(
      ec, [&] { return write_file(path, std::forward<Fill>(fill), opts); });
}

/// One-shot digest of a file: a hasher shaped by opts (SIMD variant, and
/// plain, keyed or key-derivation mode), the file streamed through it,
/// finalized.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and mode, and the pipeline knobs.
/// @throws std::system_error on I/O failure.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               const hash_file_options& opts = {});
/// One-shot digest of a file, reporting failure through ec instead of
/// throwing.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the I/O error otherwise.
/// @param opts  The hasher's variant and mode, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
[[nodiscard]] digest hash_file(const std::filesystem::path& path,
                               std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept;

namespace detail {

// Path types from other filesystem libraries (boost::filesystem::path is
// the motivating case): anything exposing a native() character sequence
// that std::filesystem::path accepts as a Source. Bridging through
// native() preserves the platform encoding exactly (no lossy transcoding,
// unlike .string() on Windows). Structural, so no third-party dependency
// or naming enters this library.
template <class P>
concept foreign_path =
    !std::same_as<std::remove_cvref_t<P>, std::filesystem::path> &&
    requires(const P& p) { std::filesystem::path(p.native()); };

}  // namespace detail

/// update_file() for a path type from another filesystem library (e.g.
/// boost::filesystem::path): anything with a `native()` the standard path
/// accepts, bridged without transcoding.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, const file_io_options& opts = {}) {
  update_file(h, std::filesystem::path(path.native()), opts);
}

/// update_file() for a foreign path type, reporting through ec.
/// @param h     The hasher to stream into.
/// @param path  The file to read.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The pipeline knobs.
template <detail::foreign_path P>
void update_file(hasher& h, const P& path, std::error_code& ec,
                 const file_io_options& opts = {}) noexcept {
  // The path conversion allocates, so it belongs inside the guard too.
  detail::with_error_code(ec, [&] {
    update_file(h, std::filesystem::path(path.native()), opts);
  });
}

/// hash_file() for a foreign path type.
/// @param path  The file to hash.
/// @param opts  The hasher's variant and mode, and the pipeline knobs.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path,
                               const hash_file_options& opts = {}) {
  return hash_file(std::filesystem::path(path.native()), opts);
}

/// hash_file() for a foreign path type, reporting through ec.
/// @param path  The file to hash.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The hasher's variant and mode, and the pipeline knobs.
/// @return The digest, or an all-zero digest when ec is set.
template <detail::foreign_path P>
[[nodiscard]] digest hash_file(const P& path, std::error_code& ec,
                               const hash_file_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return hash_file(std::filesystem::path(path.native()), opts);
  });
}

/// probe_file_io() for a foreign path type.
/// @param path  The file to probe.
/// @param opts  The pipeline knobs update_file() would be given.
template <detail::foreign_path P>
[[nodiscard]] file_io_probe probe_file_io(const P& path,
                                          const file_io_options& opts = {}) {
  return probe_file_io(std::filesystem::path(path.native()), opts);
}

/// probe_file_io() for a foreign path type, reporting through ec.
/// @param path  The file to probe.
/// @param ec    Cleared on success; the error otherwise.
/// @param opts  The pipeline knobs update_file() would be given.
template <detail::foreign_path P>
[[nodiscard]] file_io_probe probe_file_io(
    const P& path, std::error_code& ec,
    const file_io_options& opts = {}) noexcept {
  return detail::with_error_code(ec, [&] {
    return probe_file_io(std::filesystem::path(path.native()), opts);
  });
}

}  // namespace blake3pp
