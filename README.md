# Modern C++ devcontainer with Claude Code

Ubuntu 26.04 LTS base, GCC 16 and Clang 22 side by side, upstream CMake and
Ninja, `docker` talking to the host daemon, and Claude Code with its config
persisted to the Windows/WSL host.

## Layout

```
.devcontainer/
  devcontainer.json     features, bind mounts, env
  Dockerfile            toolchain
  post-create.sh        ownership fixes, shell setup, version report
CMakePresets.json       gcc-26 / clang-26 / clang-2d / asan
```

## First run

1. In **WSL** (not a Windows shell), `cd` into the repo and run `code .`.
   This matters: `${localEnv:HOME}` must resolve to your WSL home so the bind
   mounts land inside the Linux filesystem. If you open a `/mnt/c/...` path or
   launch from Windows, `HOME` is unset or points at `C:\Users\you` and the
   mounts silently break.
2. Docker Desktop → Settings → Resources → WSL Integration → enable your distro.
3. **Reopen in Container**. First build takes a while (Clang is large).
4. Check the version block `post-create.sh` prints at the end.
5. Run `claude` and sign in.

Everything persistent lives in `~/.devcontainer-persist/` on the host:

| Host path                              | Container path                    | What it holds |
|----------------------------------------|-----------------------------------|---------------|
| `~/.devcontainer-persist/claude`       | `/home/vscode/.claude`            | Claude Code credentials, settings, session history, `.claude.json` |

## Why `CLAUDE_CONFIG_DIR` is set

Claude Code splits its state: `~/.claude` holds the token, settings and history,
but the OAuth account and per-project trust live in `~/.claude.json`, a separate
file in `$HOME`. Mounting `~/.claude` alone therefore does **not** keep you
signed in. Setting `CLAUDE_CONFIG_DIR` to the same path makes Claude Code write
`.claude.json` inside the mount too, which is why there is no symlink hack here.

## C++26 and C++29: what you actually get

- **C++26**: fully available in both compilers. `CMAKE_CXX_STANDARD 26` on the
  `gcc-26` and `clang-26` presets.
- **C++29**: not a released GCC feature. GCC only gained `-std=c++29` /
  `-std=c++2d` on trunk for the future GCC 17, so GCC 16 will reject it. Clang
  has a `-std=c++2d` mode; the `clang-2d` preset uses it, and the version report
  from `post-create.sh` tells you whether your installed Clang accepts it. For
  the newest possible Clang, rebuild with `"INSTALL_LLVM_SNAPSHOT": "1"` in
  `devcontainer.json` (that pulls the LLVM 23 snapshot branch alongside 22).

One real trap: Clang parsing libstdc++ headers from a *newer* GCC often breaks,
because libstdc++ uses GCC built-ins Clang has not implemented yet. With GCC 16
and Clang 22 installed together, prefer the `clang-26` preset (libc++). The
`clang-26-libstdcxx` preset exists if you need ABI compatibility with GCC-built
libraries, and is the one to suspect first when Clang chokes inside `<ranges>`.

For `import std;`, CMake still gates it behind `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`,
whose value is a UUID that changes with each CMake release; look it up in the
docs for the exact CMake version the container reports rather than copying one
from a blog post.

## Docker from inside

The `docker-outside-of-docker` feature installs the CLI and proxies the host
socket, handling the group-ID mismatch that otherwise gives you permission
denied on `/var/run/docker.sock`.

Containers you start this way are siblings, not children. That means **`-v`
paths are resolved by the host daemon, not inside this container**:

```bash
docker run --rm -v /workspaces/myrepo:/src alpine ls /src   # empty or wrong
```

Your workspace is bind-mounted from a WSL path, so pass that path instead. The
`${localWorkspaceFolder}` value is available in `devcontainer.json` if you want
to expose it as an env var for scripts.

## Switching versions

All of the pins are build args in `devcontainer.json`: `GCC_VERSION`,
`LLVM_VERSION`, `CMAKE_VERSION`, `NINJA_VERSION`, `UBUNTU_VERSION`. Change and
rebuild. If you drop back to Ubuntu 24.04, GCC 16 is no longer in the default
repos; uncomment the `ppa:ubuntu-toolchain-r/test` line in the Dockerfile.

---

# Sanitizers, analysis tools, and the compiler matrix

## What is in the image

**Compiler sanitizers.** Every GCC pulls in its own `libasan` / `libubsan` /
`libtsan` / `liblsan` through `libgcc-N-dev`, and every Clang gets
`libclang-rt-N-dev`, so ASan, UBSan, TSan and LSan work with any toolchain in
the matrix. MSan is Clang-only (GCC does not implement it). libFuzzer is
installed for the default Clang.

**Everything else:** `valgrind` (memcheck, helgrind, DRD, massif, callgrind),
`gdb`, `lldb`, `strace`, `ltrace`, `cppcheck`, `iwyu`, `clang-tidy`,
`lcov`/`gcovr` plus `llvm-cov`/`llvm-profdata`, `heaptrack`, `hyperfine`,
`libbenchmark-dev` and `libgtest-dev`.

`runArgs` already grants `SYS_PTRACE` and unconfined seccomp, which the
sanitizers need as much as the debuggers do. `ASAN_OPTIONS`, `UBSAN_OPTIONS`,
`TSAN_OPTIONS` and `ASAN_SYMBOLIZER_PATH` are set in `containerEnv`; they are
inert for uninstrumented binaries.

## Turning sanitizers on

Presets set one cache variable rather than editing `CMAKE_CXX_FLAGS`, because
the toolchain presets already own that variable for `-stdlib=`. Wire it up once:

```cmake
include(cmake/Sanitizers.cmake)
target_link_libraries(myapp PRIVATE project::sanitizers)
```

```bash
cmake --preset asan && cmake --build --preset asan   # ASan + UBSan, Clang
cmake --preset gcc-asan                              # same, GCC + libstdc++
cmake --preset tsan
cmake --preset fuzz                                  # libFuzzer + ASan + UBSan
cmake --preset coverage                              # -fprofile-instr-generate
ctest --preset valgrind -T memcheck                  # valgrind instead
```

`Sanitizers.cmake` rejects the combinations the runtimes genuinely cannot do
(TSan+ASan, MSan+ASan) rather than letting the linker produce something
confusing.

**MSan needs one extra step.** It flags uninitialised reads anywhere in the
process, including inside an uninstrumented standard library, so it needs a
libc++ built with `-fsanitize=memory`. Run
`sudo bash .devcontainer/build-msan-libcxx.sh` once; the `msan` preset already
points at `/opt/libcxx-msan`. That build lives in the container layer, so a
rebuild discards it; move it into the Dockerfile if you use MSan routinely.

**If ASan aborts on startup** with a shadow-memory mapping error, that is the
kernel's ASLR entropy rather than your code, and it tends to show up with older
Clang. All WSL2 distros share one kernel, so from any WSL shell:

```bash
sudo sysctl -w vm.mmap_rnd_bits=28
```

`perf` and `rr` are deliberately absent: both want hardware performance
counters, which WSL2 generally does not expose.

## The compiler matrix

`GCC_VERSIONS` and `CLANG_VERSIONS` in `devcontainer.json` drive the whole
thing. Defaults are GCC 12/14/16 and Clang 18/20/22. Ubuntu 26.04 offers GCC
11-16 and Clang 17-22 from its own archive; apt.llvm.org only carries the
development branch plus the last two releases, which is why anything below
Clang 21 comes from Ubuntu and 22/23 come from LLVM. Budget roughly 1-1.5 GB
per extra Clang.

Presets cover the standards axis too: `gcc12-c20`, `gcc14-c23`, `gcc16-c23`,
`gcc16-c26`, `clang18-c20`, `clang20-c23`, `clang22-c26`, `clang22-c2d`. Note
`gcc16-c23`: a new compiler pinned to an old standard, which is the case that
catches fallback code assuming "new compiler implies new library".

Each Clang is paired with its own matching libc++. That is not cosmetic: Clang
reading libstdc++ headers from a much newer GCC breaks on built-ins it has not
implemented, and with GCC 16 in the image that is the default outcome. The
`clang18-c20-libstdcxx` preset shows the fix when you do need libstdc++ ABI
compatibility: `--gcc-install-dir` pinned to a compatible GCC.

## Seeing which fallbacks fire

```bash
tools/feature-matrix.py                          # every toolchain, C++20/23/26
tools/feature-matrix.py --std 20 23 26 --format markdown
tools/feature-matrix.py --only clang --stdlib both
```

It compiles a generated feature-test probe with every installed compiler and
prints macros as rows, toolchain/standard combinations as columns, and `.` where
a macro is absent. Combinations that fail to build are reported separately
rather than silently dropped. Add whatever your code keys off to the `FEATURES`
list at the top.

One thing to watch: `__cpp_lib_execution` is the C++17 parallel-algorithms macro
and is set almost everywhere. The senders facility is `__cpp_lib_senders`.

`cmake/StdFeatures.cmake` turns the same probes into build decisions: it
defines `PROJECT_HAS_STD_SIMD`, `PROJECT_HAS_STD_SENDERS` and friends, and only
fetches xsimd or stdexec when the standard library does not cover the feature.
So a C++26 build pulls in no third-party code while the C++20 build of the same
tree still works, which is exactly the story worth demonstrating.

---

# Toolchain files (for tools without preset support)

`cmake/toolchains/` holds one self-contained file per configuration. A consumer
passes nothing but the path:

```bash
cmake -S . -B build/clang22-c26 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang22-c26.cmake
cmake --build build/clang22-c26
```

That is the whole interface: no companion `-D` flags, no environment. Anything
that can pass `CMAKE_TOOLCHAIN_FILE` can drive the full matrix.

## Regenerating

`common.cmake` is the engine; every other file is generated and starts with a
"do not edit" banner. Change `tools/gen-toolchains.py` and re-run it:

```bash
python3 tools/gen-toolchains.py --list      # see the matrix without writing
python3 tools/gen-toolchains.py             # write cmake/toolchains/*.cmake
python3 tools/gen-toolchains.py --all       # instrument every combo, not just newest
python3 tools/gen-toolchains.py --presets   # also emit CMakePresets-toolchains.json
```

The default matrix is 24 files: GCC 12/14/16 and Clang 18/20/22 across C++20/23/26,
three Clang-against-pinned-libstdc++ variants, one experimental `-std=c++2d`, and
asan/tsan/msan/fuzzer/coverage on the newest GCC and Clang. `--all` applies the
instrumentation axis to every base combination instead.

`--presets` writes a preset file whose entries use `toolchainFile`, so the preset
and non-preset paths configure identically instead of drifting apart. Keep the
hand-written `CMakePresets.json` or switch to the generated one, but not both
under the same preset names.

## Writing one by hand

Six lines, and the naming convention matches the preset names one-for-one:

```cmake
set(TC_C_COMPILER    "clang-22")
set(TC_CXX_COMPILER  "clang++-22")
set(TC_STDLIB        "libc++")
set(TC_CXX_STANDARD  "26")
set(TC_SANITIZERS    address undefined)
include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
```

`common.cmake` documents every `TC_*` input at the top. The interesting ones:
`TC_GCC_INSTALL_DIR` pins Clang to one GCC's libstdc++, `TC_CXX_STANDARD_RAW`
passes an `-std=` value CMake does not know yet, `TC_COVERAGE` takes `llvm` or
`gcov`, and `TC_DEFAULT_BUILD_TYPE` supplies a build type when the caller has
nowhere to specify one.

## Things that will bite you

**A build directory is married to its toolchain file.** CMake caches
`CMAKE_TOOLCHAIN_FILE`; pointing an existing build tree at a different one is
not supported. Use one directory per configuration; the generated comment
header in each file suggests `build/<name>` for exactly this reason.

**Everything is self-contained on purpose.** CMake re-reads the toolchain file
inside `try_compile` sub-builds, and `-D` variables from your command line are
*not* visible there unless you name them in
`CMAKE_TRY_COMPILE_PLATFORM_VARIABLES`. Parameterising these files with extra
`-D` flags would mean the compiler-ABI check runs with different flags than your
real build, which fails in confusing ways, most visibly with sanitizers, where
the check link fails before your project configures at all.

**Flags use the `_INIT` variables.** `CMAKE_CXX_FLAGS_INIT` seeds
`CMAKE_CXX_FLAGS` on the first configure only. That is what lets you add your own
flags afterwards without them being overwritten on every re-run, but it also
means editing a toolchain file has no effect on an existing build directory.
Delete and reconfigure.

**`CMAKE_CXX_STANDARD` here is a default, not a mandate.** A project that sets it
itself, or a dependency calling `target_compile_features(... cxx_std_23)`, still
wins. That matters most for the `c2d` toolchain: the raw `-std=c++2d` only
survives while nothing asks CMake for a standard, because CMake appends its own
`-std=` after `CMAKE_CXX_FLAGS`.

**Sanitizer flags land on every target**, including anything pulled in by
`FetchContent`. For sanitizers that is what you want, and for MSan it is
mandatory. It also means `cmake/Sanitizers.cmake` and these toolchain files are
two routes to the same place; pick one. Use the toolchain route if you are
driving builds from something that cannot read presets, and drop the
`project::sanitizers` link from your targets if you do.

## License

Copyright 2026 Yannic Staudt. Apache License 2.0 with LLVM Exceptions; see
[LICENSE](LICENSE).
