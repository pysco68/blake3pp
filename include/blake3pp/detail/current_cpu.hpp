#pragma once

// The CPU the calling thread runs on, for agent_record::cpu. Compiled in
// src/trace.cpp, so the headers that record it name no platform header.
//
// Internal to the library.

#include <cstdint>

namespace blake3pp::detail {

/// The calling thread's CPU: sched_getcpu() on Linux and FreeBSD,
/// processor group x 64 + number on Windows. UINT32_MAX where the
/// platform cannot say.
[[nodiscard]] std::uint32_t current_cpu() noexcept;

}  // namespace blake3pp::detail
