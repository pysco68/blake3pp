# WebAssembly toolchain: Emscripten + node as the runner. Executables come
# out as .js+.wasm pairs; CROSSCOMPILING_EMULATOR=node makes ctest and the
# smoke tests run them transparently.
#
# Deliberate choices:
#   -fwasm-exceptions   native wasm EH (node >= 18): the library's throwing
#                       I/O APIs and doctest need real exceptions
#   -pthread            wasm threads (SharedArrayBuffer): stdexec's pool
#                       runs on real workers under node
#   NODERAWFS           direct host-filesystem access, so the CLI tools and
#                       I/O tests operate on real files
#   simd128             enabled per-kernel (ARCH_FLAGS), not globally: the
#                       scalar variant stays honestly scalar

# Platform/Emscripten.cmake beside this file wraps emscripten's own; see
# the shim for why the compiler names are set there and not here.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
include(Platform/Emscripten)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

string(APPEND CMAKE_CXX_FLAGS_INIT " -fwasm-exceptions -pthread")
string(APPEND CMAKE_C_FLAGS_INIT " -pthread")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT
       " -fwasm-exceptions -pthread -sPTHREAD_POOL_SIZE=8 -sNODERAWFS=1"
       " -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4294967296"
       " -sSTACK_SIZE=1048576 -sEXIT_RUNTIME=1")

# Resolved to an absolute path: ctest would find a bare "node" on PATH, but
# a test launcher that execs its argument literally (cmake-re's
# tipi-test-driver does) fails with "execve: No such file or directory".
find_program(BLAKE3PP_NODE NAMES node nodejs REQUIRED NO_CACHE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${BLAKE3PP_NODE}")
