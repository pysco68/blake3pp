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
  memory_source.cpp
  subtree_fold.cpp
  tree_reducer.cpp
  contracts.cpp
  unwinder.cpp
  "${vectors_hpp}")
# The io suite tests hash_file and the backend engines; without the
# file-I/O layer there is nothing for it to link against.
if(BLAKE3PP_WITH_IO)
  target_sources(blake3pp_tests PRIVATE
    io.cpp io_context.cpp io_no_provider.cpp file_pipeline.cpp
    write_pipeline.cpp async_io.cpp
    # Two translation units on purpose: see RULE 6 in
    # detail/ex_compat.hpp. One drives the pipeline through the async
    # senders and one through the blocking entry point, and only the
    # pair reproduces the linkage defect they guard.
    provider_linkage.cpp provider_linkage_tu2.cpp)
endif()
target_include_directories(blake3pp_tests PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${PROJECT_SOURCE_DIR}/src")
# A sanitized build runs the reducer's property cases tens of times
# slower, to the point where one case takes minutes and a timeout can no
# longer tell a stall from work. Those lanes run a prefix of the same
# fixed-seed sequence; the plain lanes keep the full count.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  target_compile_definitions(blake3pp_tests PRIVATE BLAKE3PP_TEST_REDUCED_CASES=1)
endif()
# The blocking update_file() takes no stop token (async_io.hpp is where a
# caller brings one), so the seam in <blake3pp/parallel_io.hpp> that
# installs a stop is compiled here and nowhere else. Every TU of this
# target gets it, which keeps the inline function it guards one
# definition; the library itself never includes that header.
target_compile_definitions(blake3pp_tests PRIVATE BLAKE3PP_TEST_PIPELINE_STOP=1)
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
  # Without a per-case limit a stall in the pipeline or a backend holds
  # the run until the CI job's own limit, with nothing to say which case
  # was stuck. A deadlock is unbounded, so any finite limit catches it,
  # and the number is set by the slowest case doing real work: the
  # reducer's property cases under GCC with address+undefined, which hash
  # a couple of gigabytes each and take about four minutes there against
  # under a second on the plain lanes.
  #
  # PROCESSORS is what ctest -j<N> needs to know about a suite whose cases
  # run their own threads: without it the two parallelisms multiply. The
  # number is the cap tests/main.cpp sets on the suite's scheduler.
  doctest_discover_tests(blake3pp_tests PROPERTIES TIMEOUT 900 PROCESSORS 4)
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
  foreach(shape vendor thead-lie sg2042 sg2042-off legacy legacy-trap legacy-071 modern nothing assume-1 assume-0
      foreign-siginfo foreign-plain)
    add_test(NAME riscv_probe_${shape} COMMAND blake3pp_riscv_probe_shapes ${shape})
  endforeach()
endif()

# The libFuzzer harness, where the toolchain instruments for it
# (linux-clang22-cxx23-fuzzer sets BLAKE3PP_TOOLCHAIN_FUZZER; the runtime
# carries its own main(), so only this target links it). The harness
# compares the library's paths against each other rather than against a
# known answer -- see tests/fuzz_paths.cpp.
#
# ctest runs a short pass of it on every build of this lane: a few
# thousand inputs take under a second and cover the lengths around every
# boundary. A real campaign is `blake3pp_fuzz_paths -max_total_time=...`
# with a corpus directory, which is a job rather than a test.
if(BLAKE3PP_TOOLCHAIN_FUZZER)
  add_executable(blake3pp_fuzz_paths fuzz_paths.cpp)
  target_compile_options(blake3pp_fuzz_paths PRIVATE -fsanitize=fuzzer)
  target_link_options(blake3pp_fuzz_paths PRIVATE -fsanitize=fuzzer)
  target_link_libraries(blake3pp_fuzz_paths PRIVATE
    blake3pp::blake3pp blake3pp_warnings)
  add_test(NAME fuzz_paths_smoke
    COMMAND blake3pp_fuzz_paths -runs=4000 -max_len=70000 -rss_limit_mb=4096)
endif()
