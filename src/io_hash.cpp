#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/io.hpp>

#include <span>

namespace blake3pp {

void update_file(hasher& h, const std::filesystem::path& path,
                 const file_io_options& opts) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
  while (auto w = reader.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    reader.release(*w);
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
