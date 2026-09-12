# The browser module: the library behind embind, as one ES module plus
# its .wasm. The name carries the kernel set, because that is what an
# embedder chooses between at load time (a SIMD-bearing module fails
# validation on an engine without wasm SIMD; see README, "The wasm
# build"): blake3pp-web-simd128 from the default build, blake3pp-web-scalar
# with BLAKE3PP_WASM_SIMD128=OFF.
if(BLAKE3PP_WASM_SIMD128)
  set(_web_kernels simd128)
else()
  set(_web_kernels scalar)
endif()

add_executable(blake3pp_web module.cpp)
target_link_libraries(blake3pp_web PRIVATE
  blake3pp::blake3pp
  blake3pp_warnings)
set_target_properties(blake3pp_web PROPERTIES
  OUTPUT_NAME "blake3pp-web-${_web_kernels}"
  SUFFIX ".mjs")
# The embedding, as opposed to the node programs' (blake3pp::node_program):
# an ES module exporting one factory, usable from a page, a worker or
# node; a worker per hardware thread spawned before the factory resolves,
# so a pool of that many threads never waits on the event loop (a request
# beyond it fails instead of deadlocking: PTHREAD_POOL_SIZE_STRICT); the
# runtime kept alive, since the module serves calls for as long as the
# page lives. No filesystem: the embedder hands over bytes, placed with
# Module._malloc and written through Module.HEAPU8, both exported here.
target_link_options(blake3pp_web PRIVATE
  -lembind
  -sMODULARIZE=1
  -sEXPORT_ES6=1
  -sEXPORT_NAME=createBlake3pp
  -sEXPORTED_FUNCTIONS=_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=HEAPU8
  -sENVIRONMENT=web,worker,node
  "SHELL:-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency"
  -sPTHREAD_POOL_SIZE_STRICT=1
  -sEXIT_RUNTIME=0)

# The module's own check, under node like every other wasm test: the
# empty-input vector, and one digest reproduced by every path the module
# exposes (sequential, pooled, streamed, each compiled variant).
if(BLAKE3PP_BUILD_TESTS)
  add_test(NAME web_module
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR}
      "${CMAKE_CURRENT_SOURCE_DIR}/test.mjs" "$<TARGET_FILE:blake3pp_web>")
endif()
