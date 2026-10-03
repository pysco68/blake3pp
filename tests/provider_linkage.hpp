#pragma once

// The seam between the two translation units of the provider-linkage
// case (RULE 6 in detail/ex_compat.hpp).
//
// The receiver below is at NAMESPACE scope on purpose. A receiver in an
// anonymous namespace would give every operation state built over it
// internal linkage by accident, and the case would then pass whether or
// not the defect is present -- which is what the first version of it
// did.

#include <exception>
#include <filesystem>
#include <system_error>

#include <blake3pp/core.hpp>
#include <blake3pp/detail/ex_compat.hpp>

namespace b3test {

// Where a completion lands. Shared by both translation units so the
// operation states they build have identical template arguments.
struct linkage_result {
  unsigned completions = 0;
  blake3pp::digest value{};
};

// Externally linked, which is the point: it is what makes the operation
// state's own template arguments externally linked too.
struct linkage_receiver {
  using receiver_concept = blake3pp::detail::ex_compat::receiver_tag;
  explicit linkage_receiver(linkage_result* r) noexcept : out(r) {}
  linkage_result* out;

  void set_value(blake3pp::digest d) && noexcept {
    out->value = d;
    ++out->completions;
  }
  void set_error(std::error_code) && noexcept { ++out->completions; }
  void set_error(std::exception_ptr) && noexcept { ++out->completions; }
  void set_stopped() && noexcept { ++out->completions; }
};

// Hashes the file through the BLOCKING pipeline, from a translation unit
// that never sees the async senders.
[[nodiscard]] blake3pp::digest hash_file_from_other_tu(
    const std::filesystem::path& path);

// Hashes it through the ASYNC senders, with the same receiver type and
// the same scheduler as the case's own translation unit -- so both units
// instantiate one operation state with identical arguments.
[[nodiscard]] blake3pp::digest hash_file_async_from_other_tu(
    const std::filesystem::path& path);

}  // namespace b3test
