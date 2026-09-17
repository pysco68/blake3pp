# Each subdirectory here is one self-contained example, with its own
# main() and its own README. The file example needs the I/O layer, so it
# is added only when that is part of the build.

set(_examples
  01-hello-hash
  02-incremental
  03-keyed-and-derive-key
  04-extended-output
  06-multi-core
  07-which-kernel
)

if(BLAKE3PP_WITH_IO)
  list(APPEND _examples 05-hash-a-file)
endif()

list(SORT _examples)

foreach(_example IN LISTS _examples)
  string(REPLACE "-" "_" _target "blake3pp_example_${_example}")
  add_executable(${_target} ${_example}/main.cpp)
  target_link_libraries(${_target} PRIVATE blake3pp::blake3pp blake3pp_warnings)
  # The process-wide scheduler is a replaceable definition, linked when
  # this build provides a sizable one.
  if(TARGET blake3pp_parallel_backend)
    target_link_libraries(${_target} PRIVATE blake3pp::parallel_backend)
  endif()

  # Running an example is the cheapest check that it still describes the
  # library it documents.
  if(BLAKE3PP_BUILD_TESTS)
    add_test(NAME ${_target} COMMAND ${_target})
  endif()
endforeach()
