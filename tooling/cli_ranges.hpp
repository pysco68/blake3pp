#pragma once

// Range checks for the tools' numeric options: a value an option accepts
// converts to the bytes or the clock ticks it names without wrapping.
// The standard library and CLI11 only, so the pipe tools, which link no
// blake3pp library, use them too.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <CLI/CLI.hpp>

namespace b3tool {

// A count of 2^shift-byte units (KiB: 10, MiB: 20) whose byte total fits
// in size_t: from `least` to SIZE_MAX >> shift.
[[nodiscard]] inline CLI::Range units_fitting(unsigned shift,
                                              std::size_t least = 1) {
  return CLI::Range(least, std::numeric_limits<std::size_t>::max() >> shift);
}

// --window transform: typed in MiB, stored in bytes (bind the option
// straight to window_bytes and give it default_str(window_mib)).
inline const CLI::Validator mib_to_bytes(
    [](std::string& value) {
      unsigned long long mib = 0;
      if (!CLI::detail::lexical_cast(value, mib) || mib == 0 ||
          mib > (std::numeric_limits<std::size_t>::max() >> 20)) {
        return std::string{"must be a positive number of MiB"};
      }
      value = std::to_string(mib << 20);
      return std::string{};
    },
    "MiB");

// A size typed in GiB, fractions allowed: finite, at least one byte, and
// below 2^64 bytes. Convert it with gib_bytes().
inline const CLI::Validator gib_size(
    [](std::string& value) {
      double gib = 0;
      // 2^34 GiB is 2^64 bytes, exactly representable as a double.
      constexpr double past = 17179869184.0;
      if (!CLI::detail::lexical_cast(value, gib) || !std::isfinite(gib) ||
          gib * 1073741824.0 < 1.0 || gib >= past) {
        return std::string{"must be a positive number of GiB below 2^34"};
      }
      return std::string{};
    },
    "GiB");

// The bytes a gib_size() value names.
[[nodiscard]] constexpr std::uint64_t gib_bytes(double gib) noexcept {
  return static_cast<std::uint64_t>(gib * 1073741824.0);
}

// The longest delay an option takes. A longer one is a typo, and the
// bound keeps a deadline built from it, in the clock's nanoseconds, far
// inside the clock's range.
inline constexpr std::chrono::hours max_delay{24};

// A delay option counted in Duration's units: from `least` to max_delay.
template <class Duration>
[[nodiscard]] CLI::Range delay_range(typename Duration::rep least = 0) {
  return CLI::Range(least,
                    std::chrono::duration_cast<Duration>(max_delay).count());
}

}  // namespace b3tool
