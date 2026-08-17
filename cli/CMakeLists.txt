# The CLI tools use std::format/std::print-style output, a C++20 library
# feature that trails the language: libstdc++ ships <format> from GCC 13.
# On older standard libraries the tools are skipped with a notice; the
# LIBRARY keeps its C++20 floor; only the tools carry this extra floor.
include(CheckIncludeFileCXX)
check_include_file_cxx("format" BLAKE3PP_CLI_HAS_FORMAT)
if(NOT BLAKE3PP_CLI_HAS_FORMAT)
  message(STATUS
    "blake3pp: <format> unavailable on this stdlib - CLI tools skipped")
  return()
endif()

# CLI11 for argument parsing: robust, pinned.
include(FetchContent)
FetchContent_Declare(cli11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
  URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(CLI11_PRECOMPILED ON CACHE BOOL "" FORCE) # Oo measured 2.4x faster recompiles of main.cpp (32.7s -> 13.4s).
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cli11)

add_executable(blake3ppsum sum.cpp)
blake3pp_tool_allocator(blake3ppsum)
target_link_libraries(blake3ppsum PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  CLI11::CLI11)

add_executable(blake3ppgen gen.cpp)
blake3pp_tool_allocator(blake3ppgen)
target_link_libraries(blake3ppgen PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  CLI11::CLI11)

# Smoke tests, driven by CMake script mode so they run identically on
# every platform (no shell): known-answer for the empty input via stdin,
# and the --check round trip including corruption detection.
foreach(case version empty_stdin check_roundtrip length_prefix
    keyed_roundtrip derive_key gen_deterministic gen_seek gen_threads
    gen_output)
  add_test(NAME cli_${case}
    COMMAND ${CMAKE_COMMAND}
      "-DSUM=$<TARGET_FILE:blake3ppsum>"
      "-DGEN=$<TARGET_FILE:blake3ppgen>"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/smoke-${case}"
      "-DCASE=${case}"
      # ctest only wraps add_test(target) commands in the emulator; a
      # script-mode driver must carry it explicitly for cross builds.
      "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/smoketest.cmake")
endforeach()
