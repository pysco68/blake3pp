# doctest: a single-header framework with no dependency tree. Still built
# from source with the project's own toolchain flags rather than a system
# package: the matrix includes libc++ builds.
include(FetchContent)
FetchContent_Declare(doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
  URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(vectors_hpp "${CMAKE_CURRENT_BINARY_DIR}/generated/test_vectors.hpp")
add_custom_command(
  OUTPUT "${vectors_hpp}"
  COMMAND Python3::Interpreter
          "${PROJECT_SOURCE_DIR}/tools/gen-test-vectors.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/vectors/test_vectors.json"
          "${vectors_hpp}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen-test-vectors.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/vectors/test_vectors.json"
  COMMENT "Generating test_vectors.hpp"
  VERBATIM)

add_executable(blake3pp_tests
  main.cpp
  official_vectors.cpp
  incremental.cpp
  kernel_ops.cpp
  "${vectors_hpp}")
target_include_directories(blake3pp_tests PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(blake3pp_tests PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  doctest::doctest)

list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
include(doctest)
doctest_discover_tests(blake3pp_tests)
