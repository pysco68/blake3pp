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

# The same two sources, built so that --help prints the placeholders
# docs/tools.md uses in place of what depends on the machine or the build
# (the core count, the async engines). tools/gen-help-docs.py runs
# them to write and check the transcripts there. Not built by default and
# not installed.
foreach(_tool IN ITEMS sum gen)
  add_executable(blake3pp${_tool}_help_docs EXCLUDE_FROM_ALL ${_tool}.cpp)
  target_compile_definitions(blake3pp${_tool}_help_docs PRIVATE
    BLAKE3PP_HELP_DOCS=1)
  target_link_libraries(blake3pp${_tool}_help_docs PRIVATE
    blake3pp::blake3pp
    blake3pp_warnings
    blake3pp::tooling
    blake3pp::tool_allocator
    blake3pp::node_program)
  if(TARGET blake3pp_parallel_backend)
    target_link_libraries(blake3pp${_tool}_help_docs PRIVATE
      blake3pp::parallel_backend)
  endif()
endforeach()
add_custom_target(help_docs
  DEPENDS blake3ppsum_help_docs blake3ppgen_help_docs)

# Names on Windows. The tools take names as narrow strings: from argv, from
# checksum lists, and into std::filesystem::path and the narrow CRT, all
# of which use the process's ANSI code page. utf8.manifest sets that code
# page to UTF-8, so a name is its UTF-8 bytes there as on every other
# platform, and the lines the tools print are UTF-8. CMake merges a
# .manifest source with the manifest the linker generates and embeds the
# result, for link.exe and lld-link alike. /utf-8 makes the literal
# encoding UTF-8 under cl as it already is under clang-cl, which is what
# makes std::print write to a console through the wide console API: names
# then display whatever the console's own code page is, and the console's
# setting is left alone.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  foreach(_tool IN ITEMS blake3ppsum blake3ppgen
      blake3ppsum_help_docs blake3ppgen_help_docs)
    target_sources(${_tool} PRIVATE utf8.manifest)
    target_compile_options(${_tool} PRIVATE /utf-8)
  endforeach()
endif()

# Smoke tests, driven by CMake script mode so they run identically on
# every platform (no shell). They cover known answers, --check round
# trips with corruption detection, the checksum-list format (escaped
# names, CRLF lists, the closing counts), names outside the ANSI code
# page, the key forms, the stream and --diag paths, the generator's
# determinism, seek and --output, and the usage errors (see
# smoketest.cmake).
foreach(case version engines empty_stdin stdin_parallel stdin_diag trace_inputs
    check_roundtrip check_batched check_empty_list check_crlf check_names
    check_escaped_line check_nonhex check_counts utf8_names check_utf8_list
    gen_utf8_names length_prefix
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

# A standard input in O_NONBLOCK mode, driven from Python because CMake
# script mode cannot set the flag on a pipe. It runs in cross builds too,
# under the emulator, which is where the wasm build's node meets it. The
# interpreter is named by path: ctest runs a command that names a target
# under the emulator, which would hand Python to node.
if(NOT WIN32)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME cli_stdin_nonblock
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/stdin_nonblock.py"
        "${CMAKE_CURRENT_BINARY_DIR}/smoke-stdin_nonblock"
        "$<TARGET_FILE:blake3ppsum>" ${CMAKE_CROSSCOMPILING_EMULATOR})
  endif()
endif()

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
