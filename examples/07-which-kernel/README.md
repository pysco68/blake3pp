# Which kernel is running

blake3pp is a fat binary. Every SIMD variant the target platform
supports is compiled in, and which one runs is decided when the program
starts.

[Try it on Compiler Explorer](https://pysco68.github.io/blake3pp/try/07-which-kernel/)

Two questions, two different answers:

```cpp
blake3pp::compiled_arches()    // what this build carries
blake3pp::available_arches()   // what this CPU can run, best first
blake3pp::best_available()     // what dispatch will pick
```

The first is a property of the build. The second is a property of the
processor, and `available_arches()` is ordered so that its first entry is
always `best_available()`.

## Choosing for yourself

```cpp
blake3pp::hasher pinned{blake3pp::arch::sse42};
```

A hasher takes a variant, which is how the tests and benchmarks compare
kernels against one another on the same machine. Every variant produces
the same digest, so pinning one changes the speed and nothing else.

Asking for a variant the CPU cannot run is not an error. Dispatch falls
back to the best available one. `is_available()` answers the question
first if you would rather not be surprised.

## The build banner

```cpp
std::cout << blake3pp::version() << ' ' << blake3pp::simd_provider()
          << ' ' << blake3pp::execution_provider() << '\n';
```

Worth printing in a diagnostics banner. Which SIMD and execution
providers a build resolved to is the first thing a bug report needs.

## Running it

```
cmake --build build --target blake3pp_example_07_which_kernel
./build/examples/blake3pp_example_07_which_kernel
```

The output depends on both the build and the machine.

## Next

- [Architectures and kernels](../../docs/architectures.md) lists every
  variant and how dispatch decides.
