# Instruments for measuring a stream through a pipe: a producer, a drain,
# a per-process cost reporter and a probe of the pipe's own behaviour,
# plus the two scripts that run them (see README.md in the parent).
#
# They are standalone: no blake3pp library, only CLI11 for their options
# (thief also needs stdexec), so they build wherever the compiler does --
# including Windows, where the bench executables above do not link.
find_package(Threads REQUIRED)

foreach(tool IN ITEMS pump drain rux pipeprobe)
  add_executable(pipe_${tool} ${tool}.cpp)
  set_target_properties(pipe_${tool} PROPERTIES OUTPUT_NAME ${tool})
  target_link_libraries(pipe_${tool} PRIVATE blake3pp_warnings blake3pp::tooling Threads::Threads)
endforeach()

# The scripts sit beside the binaries, so runpipe finds its tools there.
foreach(script IN ITEMS runpipe.sh runpipe.ps1)
  configure_file(${script} ${CMAKE_CURRENT_BINARY_DIR}/${script} COPYONLY)
endforeach()

# read() and poll() counter and fault injector, loaded with LD_PRELOAD or
# DYLD_INSERT_LIBRARIES. Not on
# Windows, which has no preload mechanism (see readshim.cpp), and not
# where the toolchain cannot make a shared object.
get_property(_pipe_shared GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS)
if(UNIX AND NOT EMSCRIPTEN AND _pipe_shared)
  add_library(pipe_readshim MODULE readshim.cpp)
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
