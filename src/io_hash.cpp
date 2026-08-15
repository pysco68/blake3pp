#include <blake3pp/io.hpp>

#include <span>

namespace blake3pp {

digest hash_file(const char* path, const hash_file_options& opts) {
  detail::file_reader reader(
      path, {opts.window_bytes, opts.queue_depth, opts.direct_io, true});
  hasher h{opts.a};
  while (auto w = reader.next()) {
    h.update(std::span<const std::byte>{w->data, w->bytes});
    reader.release(*w);
  }
  return h.finalize();
}

}  // namespace blake3pp
