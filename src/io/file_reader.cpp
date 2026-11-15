// file_reader implementation: a pimpl shell around polled_reader. This
// TU contains no platform code and no window logic: io/native.hpp names
// the context the OS gets -- io_uring (Linux), the I/O ring or the
// completion port (Windows), libdispatch (macOS), AIO (FreeBSD), plain
// pread (other POSIX), stdio (everything else) -- and io/polled_io.hpp is
// the window/slot machine over it. Runtime degradation (O_DIRECT refused
// -> buffered, async engine refused -> sync) happens inside the contexts.

#include <blake3pp/detail/file_reader.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "io/native.hpp"
#include "io/polled_io.hpp"

namespace blake3pp::detail {

// Conformance is checked in the backend headers themselves (each ends
// with definition-site static_asserts) and again by the
// polled_reader constraint at this instantiation.
struct file_reader::impl : io_impl::native_reader {
  using base = io_impl::native_reader;
  using base::base;
};

file_reader::file_reader(const std::filesystem::path& path,
                         const file_reader_options& opts)
    : impl_(std::make_unique<impl>(path, opts)) {}

file_reader::~file_reader() = default;

std::uint64_t file_reader::file_size() const noexcept {
  return impl_->file_size();
}

std::string_view file_reader::backend() const noexcept {
  return impl_->backend_name();
}

std::optional<file_reader::window> file_reader::next() {
  return impl_->next();
}

void file_reader::release(const window& w) noexcept { impl_->release(w); }

}  // namespace blake3pp::detail
