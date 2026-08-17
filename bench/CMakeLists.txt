# The official BLAKE3 C library (with its hand-written assembly kernels) is
# built alongside the bench as the performance baseline, so every run shows
# our variants against the hand-tuned ceiling on the same machine. Its
# c/CMakeLists.txt is a proper subproject; SOURCE_SUBDIR points there.
option(BLAKE3PP_BENCH_UPSTREAM
  "Include the upstream BLAKE3 C library as a baseline in the benchmark" ON)

add_executable(blake3pp_bench throughput.cpp)
blake3pp_tool_allocator(blake3pp_bench)
target_link_libraries(blake3pp_bench PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings)
# For the asm-plug-in rows: the bench builds a kernel_ops table around
# upstream's assembly, which needs the internal kernel boundary header.
target_include_directories(blake3pp_bench PRIVATE "${PROJECT_SOURCE_DIR}/src")

add_executable(blake3pp_bench_file file_throughput.cpp)
blake3pp_tool_allocator(blake3pp_bench_file)
target_link_libraries(blake3pp_bench_file PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings)

if(EMSCRIPTEN)
  # Upstream's build hard-codes x86/ARM SIMD kernels with -m flags the
  # wasm target rejects; the comparison rows are host-hardware-only.
  set(BLAKE3PP_BENCH_UPSTREAM OFF)
endif()
if(BLAKE3PP_BENCH_UPSTREAM)
  # The upstream library is C plus hand-written assembly; enabling the
  # languages here (rather than letting its nested project() try) keeps
  # compiler-feature detection in a scope CMake handles reliably.
  enable_language(C ASM)
  include(FetchContent)
  FetchContent_Declare(blake3_upstream
    URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.6.tar.gz
    URL_HASH SHA256=da7b5b0b6cf7106fe54b7d718d1ea371cce434cd15ebe5e56ca011b645cbef0e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR c
    SYSTEM)
  FetchContent_MakeAvailable(blake3_upstream)
  target_link_libraries(blake3pp_bench PRIVATE BLAKE3::blake3)
  target_compile_definitions(blake3pp_bench PRIVATE BLAKE3PP_BENCH_UPSTREAM=1)
endif()

# Under the beman provider the executables carry the program-level
# parallel_scheduler backend (see src/execution/beman_backend.cpp).
if(TARGET blake3pp_beman_backend)
  target_link_libraries(blake3pp_bench PRIVATE blake3pp::beman_backend)
  target_link_libraries(blake3pp_bench_file PRIVATE blake3pp::beman_backend)
endif()
