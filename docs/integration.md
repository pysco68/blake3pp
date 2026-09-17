# Integration

[← blake3pp](../README.md)

## Consuming via CMake

Vendoring is the normal path, and the one that probes *your* toolchain:

```cmake
include(FetchContent)
FetchContent_Declare(blake3pp GIT_REPOSITORY <this-repo> GIT_TAG main)
FetchContent_MakeAvailable(blake3pp)
target_link_libraries(app PRIVATE blake3pp::blake3pp)
```

The library is C++20; with a C++26 toolchain it uses native `std::simd`,
otherwise `std::experimental::simd` or xsimd: the same source, probed at
configure time. Tests, benchmarks and the `blake3ppsum` CLI only build
when blake3pp is the top-level project, so vendoring costs you nothing
beyond the library itself.

## Installing and find_package

```bash
cmake --preset linux-gcc16-cxx26
cmake --build --preset linux-gcc16-cxx26
cmake --install build/linux-gcc16-cxx26 --prefix /opt/blake3pp
```

```cmake
find_package(blake3pp REQUIRED)          # CMAKE_PREFIX_PATH=/opt/blake3pp
target_link_libraries(app PRIVATE blake3pp::blake3pp)
```

What gets installed is the static library, the public headers, and the
package config, nothing else. The archive is **self-contained**: the
per-architecture SIMD kernels are compiled into it, and it carries no link
dependency on any third-party library, so there is no `find_dependency()`
in the config and nothing to install alongside it. `BLAKE3PP_INSTALL`
(default: on for top-level builds) turns the rules off for consumers who
vendor.

One thing does not travel, and it is the reason for the last column of the
header table above. The execution provider is header-only and belongs to
whoever builds the final program, so blake3pp does not install it. Code
that includes `<blake3pp/parallel.hpp>` or `<blake3pp/parallel_io.hpp>`
(including anything that pulls the umbrella header) must put that
provider's headers on its own include path:

```cmake
find_package(blake3pp REQUIRED)
message(STATUS "built against: ${blake3pp_EXECUTION_PROVIDER}")   # e.g. stdexec
target_include_directories(app SYSTEM PRIVATE ${STDEXEC_INCLUDE_DIR})
```

The imported target defines the matching `BLAKE3PP_EXECUTION_*` macro, so
those headers select the same provider branch the library was compiled
against, and `blake3pp_EXECUTION_PROVIDER` tells you which one that was.
Consumers that stay on `core.hpp` / `dispatch.hpp` / `io.hpp` need none of
this: link the target and go.

Because the choices are frozen at install time, an installed package is
only valid for toolchains ABI-compatible with the one that built it; the
package version file declares `SameMajorVersion` compatibility, which
covers blake3pp's own API but says nothing about your compiler. If in
doubt, vendor.

## Prebuilt releases

CI publishes binary releases as dev-* prereleases from every green main
run:

| release | targets | toolchain |
|---------|---------|-----------|
| Linux, fully static | x86_64, aarch64, riscv64, ppc64le, s390x | zig (clang + musl) |
| Windows | x64 and arm64 | clang-cl from the VS toolset |
| macOS | Apple silicon | Apple clang |
| wasm | wasm32-simd128, node-runnable js+wasm pairs and the browser ES module | Emscripten |

The static Linux binaries are one file per architecture that runs on
any distro: no glibc version coupling, no dynamic loader, runtime SIMD
dispatch intact. Two of them are even the work of two compilers at
once. How that works, and why it holds up, is explained in
[docs/static-linux-binaries.md](static-linux-binaries.md).

## Header layout

The split follows one rule. Members are the sequential primitives of the
value types (`update`, `finalize`, `finalize_xof`, `fill`, `take`,
`seek`). Free functions are the entry points that bring in a resource
the type does not own, a scheduler or a file, and each comes as a pair
so call sites read alike with and without cores:
`hash(data)` and `hash(data, sched)`,
`update_file(h, path)` and `update_file(h, path, sched)`,
`fill(r, out)` and `fill(r, out, sched)`. The scheduler-taking
half of each pair lives in the header that owns the dependency.
The two I/O headers exist only when the library is built with
`BLAKE3PP_WITH_IO=ON` (the default); see
[Freestanding and RTOS builds](freestanding.md).
