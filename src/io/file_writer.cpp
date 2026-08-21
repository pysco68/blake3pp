// file_writer implementation: the write-side mirror of file_reader.cpp,
// and like it just a pimpl shell: writer_engine (io/engine.hpp) is the
// slot machine, written once against the writer_backend concept, and
// io/backend_select.hpp decides which OS backend it is instantiated
// with here.

#include <blake3pp/detail/file_writer.hpp>

#include "io/backend_select.hpp"
#include "io/engine.hpp"

namespace blake3pp::detail {

// Conformance is checked in the backend headers themselves (each ends
// with definition-site static_asserts) and again by the writer_engine
// constraint at this instantiation.
struct file_writer::impl : io_impl::writer_engine<io_impl::native_writer> {
  using writer_engine::writer_engine;
};

file_writer::file_writer(const std::filesystem::path& path,
                         const file_writer_options& opts)
    : impl_(new impl(path, opts)) {}

file_writer::~file_writer() {
  // Best-effort drain: buffers must outlive in-flight writes. Errors here
  // are unreportable; that is why finish() exists. Anything a failed
  // finish leaves in flight is handled by the backend's own destructor.
  try {
    finish();
  } catch (...) {
  }
  delete impl_;
}

std::uint64_t file_writer::bytes_written() const noexcept {
  return impl_->bytes_written();
}

std::string_view file_writer::backend() const noexcept {
  return impl_->backend_name();
}

file_writer::buffer file_writer::acquire() { return impl_->acquire(); }

void file_writer::submit(const buffer& b, std::size_t bytes) {
  impl_->submit(b, bytes);
}

void file_writer::finish() { impl_->finish(); }

}  // namespace blake3pp::detail
