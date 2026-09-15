# The two checksum/generator tools. Everything they share with the
# benchmarks (CLI11, tooling/tool_common.hpp, the <format> floor) comes
# from blake3pp::tooling, set up in the top-level CMakeLists.

add_executable(blake3ppsum sum.cpp)
target_link_libraries(blake3ppsum PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  blake3pp::tooling
  blake3pp::tool_allocator
  blake3pp::node_program)

add_executable(blake3ppgen gen.cpp)
target_link_libraries(blake3ppgen PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  blake3pp::tooling
  blake3pp::tool_allocator
  blake3pp::node_program)

# Smoke tests, driven by CMake script mode so they run identically on
# every platform (no shell): known-answer for the empty input via stdin,
# and the --check round trip including corruption detection.
foreach(case version empty_stdin stdin_parallel check_roundtrip length_prefix
    keyed_roundtrip derive_key gen_deterministic gen_seek gen_threads
    gen_output gen_seed_file sum_errors key_forms gen_errors)
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
