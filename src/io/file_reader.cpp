// file_reader implementation: a pimpl shell around the portable engine.
// This TU contains no platform code and no engine logic: reader_engine
// (io/engine.hpp) is the window/slot machine, written once against the
// reader_backend concept, and io/backend_select.hpp decides which OS
// backend it is instantiated with here: io_uring (Linux), IOCP
// (Windows), GCD (macOS), plain pread (other POSIX), stdio (everything
// else). Runtime degradation (O_DIRECT refused -> buffered, async engine
// refused -> sync) happens inside the backends.

#include <blake3pp/detail/file_reader.hpp>

#include "io/backend_select.hpp"
#include "io/engine.hpp"

namespace blake3pp::detail {

// Conformance is checked in the backend headers themselves (each ends
// with definition-site static_asserts) and again by the reader_engine
// constraint at this instantiation.
struct file_reader::impl : io_impl::reader_engine<io_impl::native_reader> {
  using reader_engine::reader_engine;
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
