#include <blake3pp/detail/pipeline_plan.hpp>
#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/detail/io_driver.hpp>
#include <blake3pp/io.hpp>
#include <blake3pp/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <thread>

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
                                    .offload_submit = opts.offload_submit,
                                    .ioring = opts.ioring});
  trace_buffer* const trace = opts.trace;
  if (trace != nullptr) {
    set_read_backend(trace->driver(), reader.backend());
  }
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

file_io_probe probe_file_io(const std::filesystem::path& path,
                            const file_io_options& opts) {
  // The plan update_file() makes over a scheduler for a fresh hasher,
  // and the open it makes from that plan; the sequential form opens the
  // same way and differs only in the geometry it reads with.
  std::error_code size_ec;
  const std::uintmax_t size = std::filesystem::file_size(path, size_ec);
  const detail::pipeline_plan plan = detail::plan_pipeline(
      0, opts.window_bytes, opts.queue_depth, opts.direct_io,
      size_ec ? 0 : static_cast<std::uint64_t>(size),
      detail::read_plan_budget_bytes(std::thread::hardware_concurrency(),
                                     opts.window_bytes));
  detail::io_driver drv({.async = opts.async,
                         .offload_submit = opts.offload_submit,
                         .ioring = opts.ioring},
                        plan.reads_in_flight);
  detail::io_driver::file file(drv, path, plan.direct_io);
  return {.file_bytes = file.size(),
          .window_bytes = plan.window_bytes,
          .reads_in_flight = plan.reads_in_flight,
          .windows = plan.windows,
          .direct_io = plan.direct_io,
          .backend = std::string(file.name())};
}

file_io_probe probe_file_io(const std::filesystem::path& path,
                            std::error_code& ec,
                            const file_io_options& opts) noexcept {
  return detail::with_error_code(ec, [&] { return probe_file_io(path, opts); });
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
