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
# every platform (no shell). They cover known answers, --check round
# trips with corruption detection, the key forms, the stream and --diag
# paths, the generator's determinism, seek and --output, and the usage
# errors (see smoketest.cmake).
foreach(case version engines empty_stdin stdin_parallel stdin_diag check_roundtrip
    check_batched length_prefix
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

# The bench scripts (bench/pipe/), smoke-run where their tools were built.
if(TARGET pipe_pump AND NOT CMAKE_CROSSCOMPILING)
  # The platform's bench script, smoke-run (runpipe.ps1 on Windows where
  # PowerShell is found, runpipe.sh elsewhere): one short validated
  # rotation through blake3ppsum, which exercises every check the script
  # makes -- both exit codes, the digest against the reference, the drain
  # ceiling. Serial, because a drain ceiling measured while other tests
  # load the machine is no ceiling.
  set(_pipe_dir "${PROJECT_SOURCE_DIR}/bench/pipe")
  if(WIN32)
    find_program(BLAKE3PP_POWERSHELL NAMES powershell pwsh)
    if(BLAKE3PP_POWERSHELL)
      add_test(NAME pipe_runpipe_smoke
        COMMAND ${BLAKE3PP_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File "${_pipe_dir}/runpipe.ps1"
                -ToolDir "$<TARGET_FILE_DIR:pipe_pump>" -Rounds 2 -Shape smoke
                -Producer "$<TARGET_FILE:pipe_pump> --gen 0.0625 --chunk 64"
                "sum=$<TARGET_FILE:blake3ppsum> -")
    endif()
  else()
    add_test(NAME pipe_runpipe_smoke
      COMMAND sh "${_pipe_dir}/runpipe.sh"
              -t "$<TARGET_FILE_DIR:pipe_pump>" -r 2 -s smoke
              -p "$<TARGET_FILE:pipe_pump> --gen 0.0625 --chunk 64"
              "sum=$<TARGET_FILE:blake3ppsum> -")
  endif()
  if(TEST pipe_runpipe_smoke)
    set_tests_properties(pipe_runpipe_smoke PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()

# The sized parallel scheduler is one definition per program; the tools
# take it when the build offers one (see the root CMakeLists).
if(TARGET blake3pp_parallel_backend)
  target_link_libraries(blake3ppsum PRIVATE blake3pp::parallel_backend)
  target_link_libraries(blake3ppgen PRIVATE blake3pp::parallel_backend)
endif()
