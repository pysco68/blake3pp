# doctest: a single-header framework with no dependency tree. Still built
# from source with the project's own toolchain flags rather than a system
# package: the matrix includes libc++ builds. AtConfigureTime because the
# doctest_discover_tests() cmake module (installed into the config dir) is
# include()d below, at configure.
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5) # v2.4.12
FetchContent_MakeHermetic(doctest
  HERMETIC_BUILD_SYSTEM cmake
  HERMETIC_TOOLCHAIN_EXTENSION [=[
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  ]=])
HermeticFetchContent_MakeAvailableAtConfigureTime(doctest)

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
  parallel.cpp
  subtree_fold.cpp
  tree_reducer.cpp
  "${vectors_hpp}")
# The io suite tests hash_file and the backend engines; without the
# file-I/O layer there is nothing for it to link against.
if(BLAKE3PP_WITH_IO)
  target_sources(blake3pp_tests PRIVATE io.cpp)
endif()
target_include_directories(blake3pp_tests PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(blake3pp_tests PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings
  blake3pp::node_program
  doctest::doctest)

list(APPEND CMAKE_MODULE_PATH
  "${HERMETIC_FETCHCONTENT_INSTALL_DIR}/doctest-install/lib/cmake/doctest"
  "${HERMETIC_FETCHCONTENT_INSTALL_DIR}/doctest-install/lib64/cmake/doctest")
include(doctest)
if(EMSCRIPTEN)
  # Per-case discovery parses --list-test-cases output; under node,
  # emscripten's isatty says "terminal" and the ANSI-colored listing
  # defeats the parser. One aggregate entry (emulator-wrapped by ctest)
  # keeps the whole suite running instead.
  add_test(NAME blake3pp_tests COMMAND blake3pp_tests --no-colors)
elseif(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  # Discovery must RUN the binary, which a cross build without an
  # emulator cannot (e.g. arm64-windows built on x64). Register the
  # aggregate; it becomes runnable on real hardware.
  add_test(NAME blake3pp_tests COMMAND blake3pp_tests --no-colors)
else()
  doctest_discover_tests(blake3pp_tests)
endif()

# The sized parallel scheduler is one definition per program; the test
# binary takes it when the build offers one (see the root CMakeLists).
if(TARGET blake3pp_parallel_backend)
  target_link_libraries(blake3pp_tests PRIVATE blake3pp::parallel_backend)

  # Its own binary: the assertions are about process-global state in a
  # fixed order, which a shared test binary cannot express.
  add_executable(blake3pp_sized_scheduler_test sized_scheduler.cpp)
  target_link_libraries(blake3pp_sized_scheduler_test
    PRIVATE blake3pp::blake3pp blake3pp::parallel_backend doctest::doctest
            blake3pp_warnings)
  add_test(NAME sized_parallel_scheduler
    COMMAND blake3pp_sized_scheduler_test)
endif()

# The riscv detection ladder against machine shapes the emulator cannot
# present (T-Head vendor ids, kernels without hwprobe, probes that trap):
# a second compilation of the detection TU with its test seam enabled,
# one process per shape. See tests/riscv_probe_shapes.cpp.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(blake3pp_riscv_probe_shapes
    riscv_probe_shapes.cpp
    "${PROJECT_SOURCE_DIR}/src/dispatch/cpu_detect_riscv.cpp")
  target_compile_definitions(blake3pp_riscv_probe_shapes PRIVATE BLAKE3PP_TEST_PROBE_SHAPES=1)
  target_include_directories(blake3pp_riscv_probe_shapes PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(blake3pp_riscv_probe_shapes PRIVATE blake3pp_warnings)
  foreach(shape vendor thead-lie sg2042 sg2042-off legacy legacy-trap legacy-071 modern nothing assume-1 assume-0)
    add_test(NAME riscv_probe_${shape} COMMAND blake3pp_riscv_probe_shapes ${shape})
  endforeach()
endif()
