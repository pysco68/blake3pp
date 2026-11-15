// detail::current_cpu() for agent_record::cpu: the one place a trace asks
// the platform which CPU a thread is on.

#include <blake3pp/detail/current_cpu.hpp>

#include <cstdint>

#if defined(__linux__) || defined(__FreeBSD__)
#include <sched.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace blake3pp::detail {

std::uint32_t current_cpu() noexcept {
#if defined(__linux__) || defined(__FreeBSD__)
  const int c = ::sched_getcpu();
  return c < 0 ? ~std::uint32_t{0} : static_cast<std::uint32_t>(c);
#elif defined(_WIN32)
  PROCESSOR_NUMBER n{};
  ::GetCurrentProcessorNumberEx(&n);
  return std::uint32_t{n.Group} * 64 + n.Number;
#else
  return ~std::uint32_t{0};
#endif
}

}  // namespace blake3pp::detail
