# syntax=docker/dockerfile:1
#
# Emscripten toolchain image for the wasm32-emcc-cxx23 preset. The apt
# package matches the absolute path the toolchain file hard-codes
# (/usr/share/emscripten/cmake/Modules/Platform/Emscripten.cmake); nodejs
# serves as CMAKE_CROSSCOMPILING_EMULATOR (needs >= 18 for -fwasm-exceptions;
# 26.04 ships 22).
FROM base

RUN apt-get update && apt-get install -y --no-install-recommends \
        emscripten nodejs \
    && rm -rf /var/lib/apt/lists/* \
    && emcc --version | head -1 \
    && node --version

# No cache prewarm needed: Ubuntu's package ships a complete prebuilt sysroot
# cache (/usr/share/emscripten/cache, FROZEN_CACHE=True), covering the
# pthread/wasm-eh/simd variants. This layer just proves it with the project's
# flag combination: if a future package drops a variant, the image build
# fails here instead of every tc run recompiling system libs.
RUN set -eux; \
    printf '#include <cstdio>\nint main() { std::puts("ok"); }\n' > /tmp/hello.cpp; \
    emcc -pthread -fwasm-exceptions -msimd128 -O2 \
      -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 \
      /tmp/hello.cpp -o /tmp/hello.js; \
    node /tmp/hello.js; \
    rm -f /tmp/hello*
