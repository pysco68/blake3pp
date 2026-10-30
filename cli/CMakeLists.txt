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

# Fault tests: blake3ppsum and blake3ppgen in pipelines whose producer
# closes early, dies or stalls, whose reader goes away, whose stdin is
# non-blocking, closed or failing, and whose reads are interrupted by
# signals. faults.cmake states the contract they hold the tools to. The
# faults come from the pipe instruments in bench/pipe/, so these tests
# exist where those were built, and not in a cross build.
if(TARGET pipe_pump AND NOT CMAKE_CROSSCOMPILING)
  foreach(case short_stream empty_stream producer_dies pipefail stall read_error
      eintr_storm nonblock_stdin closed_stdin stdout_closed gen_reader_closed)
    add_test(NAME cli_fault_${case}
      COMMAND ${CMAKE_COMMAND}
        "-DSUM=$<TARGET_FILE:blake3ppsum>"
        "-DGEN=$<TARGET_FILE:blake3ppgen>"
        "-DPUMP=$<TARGET_FILE:pipe_pump>"
        "-DDRAIN=$<TARGET_FILE:pipe_drain>"
        "-DRUX=$<TARGET_FILE:pipe_rux>"
        "-DSHIM=$<$<TARGET_EXISTS:pipe_readshim>:$<TARGET_FILE:pipe_readshim>>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/fault-${case}"
        "-DCASE=${case}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/faults.cmake")
    # A case that cannot run on this platform says "skipped:" and exits 0;
    # ctest reports that as Skipped, not Passed. The command is cmake, so
    # ctest cannot see the tools it drives: they are named as required
    # files, and a build that left them out reports Not Run.
    set_tests_properties(cli_fault_${case} PROPERTIES
      SKIP_REGULAR_EXPRESSION "skipped:"
      REQUIRED_FILES "$<TARGET_FILE:blake3ppsum>;$<TARGET_FILE:blake3ppgen>;$<TARGET_FILE:pipe_pump>;$<TARGET_FILE:pipe_drain>;$<TARGET_FILE:pipe_rux>")
  endforeach()

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
    set_tests_properties(pipe_runpipe_smoke PROPERTIES RUN_SERIAL TRUE
      REQUIRED_FILES "$<TARGET_FILE:blake3ppsum>;$<TARGET_FILE:pipe_pump>;$<TARGET_FILE:pipe_drain>;$<TARGET_FILE:pipe_rux>")
  endif()
endif()

# The sized parallel scheduler is one definition per program; the tools
# take it when the build offers one (see the root CMakeLists).
if(TARGET blake3pp_parallel_backend)
  target_link_libraries(blake3ppsum PRIVATE blake3pp::parallel_backend)
  target_link_libraries(blake3ppgen PRIVATE blake3pp::parallel_backend)
endif()
