#pragma once

// What the parallel engines may spend on a part table, and how an input
// is cut into parts: the budget type, its default, and the part size it
// implies. Names no execution provider, so the window planner and the
// plan can use it without pulling one in. The public names (stack_budget
// and default_stack_budget) reach users through <blake3pp/parallel.hpp>.

#include <algorithm>
#include <bit>
#include <cstddef>

namespace blake3pp {

namespace detail {
// Deliberately not constexpr and never defined: a stack_budget constructor
// that reaches one fails its constant evaluation, and the diagnostic names
// the violated rule.
void stack_budget_not_a_multiple_of_32_bytes();
void stack_budget_below_two_parts();
}  // namespace detail

/// What the multi-core functions may spend on their table of part chaining
/// values, passed as their first template argument:
///
/// @code
/// blake3pp::hash<blake3pp::stack_budget{1024}>(input, sched);   // 32 parts
/// @endcode
///
/// The input is split into at most parts() parts whatever its size, and
/// each part's 32-byte chaining value sits in that table. The default is
/// 32 KiB, which is 1024 parts. A budget that is not a multiple of 32
/// bytes, or that holds fewer than two parts, does not compile.
///
/// There is no upper limit, because what a target can spare is not known
/// where this header is compiled. Code that knows its own budget checks
/// against it:
///
/// @code
/// constexpr blake3pp::stack_budget budget{1024};
/// static_assert(budget.bytes <= CONFIG_MAIN_STACK_SIZE / 4);   // e.g. on Zephyr
/// @endcode
///
/// @warning The budget covers this table alone, and the table lives with
/// the window that owns it rather than on a frame -- on the memory and
/// file paths that is the heap, one per window in flight. An agent's own
/// frame is bounded by BLAKE3PP_MAX_SIMD_DEGREE and
/// BLAKE3PP_SUBTREE_FOLD, not by this, and nothing checks an agent
/// thread's stack at run time.
///
/// @note A full window's grouped reduction keeps a second table of
/// parts() x simd_degree chaining values, 512 KiB at most for the default
/// budget on a 16-lane kernel; it lives on the heap with the window that
/// owns it, allocated when the window first needs it and kept.
///
/// @see docs/api.md for what the knob trades away, and
/// docs/freestanding.md for the measured frames and a worked case.
struct stack_budget {
  /// The stack one part's chaining value takes.
  static constexpr std::size_t bytes_per_part = 32;

  /// The budget in bytes.
  std::size_t bytes;

  /// @param n  A multiple of bytes_per_part, at least two parts' worth.
  consteval explicit stack_budget(std::size_t n) : bytes{n} {
    if (n % bytes_per_part != 0) {
      detail::stack_budget_not_a_multiple_of_32_bytes();
    }
    if (n / bytes_per_part < 2) {
      detail::stack_budget_below_two_parts();
    }
  }

  /// The most parts the input is split into.
  [[nodiscard]] constexpr std::size_t parts() const noexcept {
    return bytes / bytes_per_part;
  }
};

/// The default budget: 32 KiB of stack, 1024 parts.
inline constexpr stack_budget default_stack_budget{32 * 1024};

namespace detail {

// A part is at least 16 chunks (16 KiB), which keeps small inputs in few
// tasks.
inline constexpr std::size_t min_part_chunks = 16;

// The part size in chunks for num_chunks: a power of two, so every part
// starting at a multiple of it is subtree-aligned, and large enough that
// num_chunks / part <= Budget.parts().
template <stack_budget Budget>
[[nodiscard]] constexpr std::size_t part_chunks(std::size_t num_chunks) noexcept {
  return std::bit_ceil(std::max(
      (num_chunks + Budget.parts() - 1) / Budget.parts(), min_part_chunks));
}

}  // namespace detail

}  // namespace blake3pp
