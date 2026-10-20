#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <system_error>

namespace blake3pp {

void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts) {
  // The same open-mode rule the pipeline's plan applies: a small file is
  // read through the page cache whatever was asked, and a size the
  // filesystem will not give counts as large.
  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
  const bool direct_io =
      opts.direct_io &&
      detail::direct_io_worthwhile(size_ec ? 0 : static_cast<std::uint64_t>(size));
  detail::file_reader reader(path, {.window_bytes = opts.window_bytes,
                                    .queue_depth = opts.queue_depth,
                                    .direct_io = direct_io,
                                    .async = opts.async,
                                    .offload_submit = opts.offload_submit});
  trace_buffer* const trace = opts.trace;
  std::uint64_t index = 0;
  for (;;) {
    // The wait is timed before the record is claimed: the last next()
    // returns nothing, and that iteration gets no record.
    const std::int64_t t_wait_begin = trace ? trace->now() : 0;
    auto w = reader.next();
    if (!w) {
      break;
    }
    window_record* const rec = trace ? trace->claim_window() : nullptr;
    if (rec) {
      rec->index = index;
      rec->bytes = w->bytes;
      rec->flags = w->last ? window_record::flag_last : 0;
      rec->t_wait_begin = t_wait_begin;
      rec->t_ready = trace->now();
    }
    h.update(std::span<const std::byte>{w->data, w->bytes});
    if (rec) {
      rec->t_joined = rec->t_absorbed = trace->now();
    }
    reader.release(*w);
    if (rec) {
      rec->t_released = trace->now();
    }
    ++index;
  }
}

void update_file(hasher& h, const std::filesystem::path& path,
                 std::error_code& ec, const file_io_options& opts) noexcept {
  detail::with_error_code(ec, [&] { update_file(h, path, opts); });
}

digest hash_file(const std::filesystem::path& path,
                 const hash_file_options& opts) {
  hasher h = detail::make_hasher(opts, detail::resolve(opts.a));
  update_file(h, path, opts);
  return h.finalize();
}

digest hash_file(const std::filesystem::path& path, std::error_code& ec,
                 const hash_file_options& opts) noexcept {
  return detail::with_error_code(ec, [&] { return hash_file(path, opts); });
}

}  // namespace blake3pp
