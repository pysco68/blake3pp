// file_reader implementation: a pimpl shell around the portable engine.
// This TU contains no platform code and no engine logic:
// io/backend_select.hpp names the engine the OS gets, already bound to
// its backend -- io_uring (Linux), IOCP (Windows), GCD (macOS), plain
// pread (other POSIX), stdio (everything else) -- and io/engine.hpp is
// the window/slot machine behind both of them. Runtime degradation
// (O_DIRECT refused -> buffered, async engine refused -> sync) happens
// inside the backends.

#include <blake3pp/detail/file_reader.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "io/backend_select.hpp"
#include "io/engine.hpp"

namespace blake3pp::detail {

// Conformance is checked in the backend headers themselves (each ends
// with definition-site static_asserts) and again by the reader_engine
// constraint at this instantiation.
struct file_reader::impl : io_impl::native_reader_engine {
  using base = io_impl::native_reader_engine;
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
