#pragma once

// Opting a staging buffer out of C++26's erroneous-behaviour filling.
//
// P2795 made reading an uninitialized automatic variable erroneous rather
// than undefined, and GCC 16 implements that by filling every one of them
// at -std=c++26; clang 22 does not. The buffers marked with this are
// written before they are read, to a length their caller computes, so the
// fill is pure cost: single-thread on Zen 3+, the pipeline around
// upstream's hand-written avx2 assembly ran 8% slower than the identical
// sources at -std=c++23.
//
// [[indeterminate]] is the standard's own opt-out and says what is meant:
// this object holds an indeterminate value on purpose. It appertains to
// the declarator, so on an array it goes after the name and before the
// bounds; after the bounds it lands on the type instead, where GCC
// rejects it as inapplicable and keeps filling.
#if __has_cpp_attribute(indeterminate)
#define BLAKE3PP_CXXATTR_UNINITIALIZED [[indeterminate]]
// GCC 16.0 and 16.1 fill these buffers and accept the attribute that stops
// it, but do not report the attribute through __has_cpp_attribute, so the
// check above misses the only compiler measured here that fills
// (gcc.gnu.org/bugzilla PR126309, fixed in 16.2).
#elif defined(__GNUC__) && (__GNUC__ == 16) && defined(__cplusplus) && (__cplusplus > 202302L)
#define BLAKE3PP_CXXATTR_UNINITIALIZED [[indeterminate]]
#else
#define BLAKE3PP_CXXATTR_UNINITIALIZED
#endif
