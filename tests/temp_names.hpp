#pragma once

// The per-process part of a test's temporary file names, so suites that
// ctest runs in parallel never share a file.
//
// The pid keeps concurrent processes apart even where random_device
// returns the same value every time, as libstdc++'s does on a CPU whose
// RDSEED reports success with a zero (AMD-SB-7055). random_device keeps
// them apart where the pid is fixed, as it is under Emscripten.

#include <random>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace b3test {

[[nodiscard]] inline const std::string& process_salt() {
  static const std::string salt = [] {
#if defined(_WIN32)
    const auto pid = ::_getpid();
#else
    const auto pid = ::getpid();
#endif
    return std::to_string(pid) + "_" + std::to_string(std::random_device{}());
  }();
  return salt;
}

}  // namespace b3test
