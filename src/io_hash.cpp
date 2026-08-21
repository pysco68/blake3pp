#include <blake3pp/detail/file_reader.hpp>
#include <blake3pp/io.hpp>

#include <span>

namespace blake3pp {

digest hash_file(const std::filesystem::path& path,
                 const hash_file_options& opts) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
  hasher h = detail::make_hasher(opts, detail::resolve(opts.a));
  while (auto w = reader.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    reader.release(*w);
  }
  return h.finalize();
}

digest hash_file(const std::filesystem::path& path, std::error_code& ec,
                 const hash_file_options& opts) noexcept {
  try {
    ec.clear();
    return hash_file(path, opts);
  } catch (const std::system_error& e) {
    ec = e.code();
  } catch (...) {
    ec = std::make_error_code(std::errc::not_enough_memory);
  }
  return digest{};
}

}  // namespace blake3pp
