# Instruments for measuring a stream through a pipe: a producer, a drain,
# a per-process cost reporter and a probe of the pipe's own behaviour,
# plus the two scripts that run them (see README.md in the parent).
#
# They are standalone: no blake3pp library, only CLI11 for their options
# and two of its headers that link nothing (thief also needs stdexec), so
# they build wherever the compiler does --
# including Windows, where the bench executables above do not link.
find_package(Threads REQUIRED)

foreach(tool IN ITEMS pump drain rux pipeprobe)
  add_executable(pipe_${tool} ${tool}.cpp)
  set_target_properties(pipe_${tool} PROPERTIES OUTPUT_NAME ${tool})
  target_link_libraries(pipe_${tool} PRIVATE blake3pp_warnings blake3pp::tooling Threads::Threads)
  # The library's busy-wait hint and drain's stream_watch are headers that
  # need no library to link: the include directory, and still no library.
  target_include_directories(pipe_${tool} PRIVATE "${PROJECT_SOURCE_DIR}/include")
endforeach()

# Sizes and delays past what the tools can represent are refused with the
# option's diagnostic before anything is written or read; a test passes
# only on that diagnostic, so a crash or a stream that never ends fails
# it. 2^34 GiB is 2^64 bytes, and a delay past a day is out of range.
add_test(NAME pipe_pump_rejects_gen COMMAND pipe_pump --gen 17179869184)
add_test(NAME pipe_pump_rejects_negative_gen COMMAND pipe_pump --gen -1)
set_tests_properties(pipe_pump_rejects_gen pipe_pump_rejects_negative_gen
  PROPERTIES PASS_REGULAR_EXPRESSION "--gen: must be a positive number of GiB"
  TIMEOUT 60)
add_test(NAME pipe_pump_rejects_gap
  COMMAND pipe_pump --gen 1 --gap-us 86400000001)
set_tests_properties(pipe_pump_rejects_gap PROPERTIES
  PASS_REGULAR_EXPRESSION "--gap-us: Value 86400000001 not in range"
  TIMEOUT 60)
add_test(NAME pipe_drain_rejects_budget COMMAND pipe_drain --budget 86400000001)
set_tests_properties(pipe_drain_rejects_budget PROPERTIES
  PASS_REGULAR_EXPRESSION "--budget: Value 86400000001 not in range"
  TIMEOUT 60)

# The scripts sit beside the binaries, so runpipe finds its tools there.
foreach(script IN ITEMS runpipe.sh runpipe.ps1)
  configure_file(${script} ${CMAKE_CURRENT_BINARY_DIR}/${script} COPYONLY)
endforeach()

# read() and poll() counter and fault injector, loaded with LD_PRELOAD or
# DYLD_INSERT_LIBRARIES. Not on
# Windows, which has no preload mechanism (see readshim.cpp), and not
# where the toolchain cannot make a shared object. SHARED, not MODULE:
# on macOS a MODULE is an MH_BUNDLE, which DYLD_INSERT_LIBRARIES refuses
# to load ("cannot link against bundle").
get_property(_pipe_shared GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS)
if(UNIX AND NOT EMSCRIPTEN AND _pipe_shared)
  add_library(pipe_readshim SHARED readshim.cpp)
  target_link_libraries(pipe_readshim PRIVATE blake3pp_warnings ${CMAKE_DL_LIBS})
endif()

# The idle-pool cost. POSIX only, because it reads the process's context
# switches through getrusage (see thief.cpp), and stdexec only.
if(NOT WIN32 AND _blake3pp_execution_provider STREQUAL "stdexec")
  add_executable(pipe_thief thief.cpp)
  set_target_properties(pipe_thief PROPERTIES OUTPUT_NAME thief)
  target_link_libraries(pipe_thief PRIVATE
    blake3pp_warnings blake3pp::features blake3pp::tooling Threads::Threads)
endif()

# The scripts' smoke test needs blake3ppsum, and cli/ is added after
# bench/, so it is registered there (cli/CMakeLists.txt), beside the
# fault tests that use these same tools.
