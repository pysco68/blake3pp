// <blake3pp/io.hpp> must not reach an execution provider.
//
// The pipeline's headers name one -- that is their job -- and the I/O
// surface is public API that a caller may include on its own, with a
// provider it never asked for and a build that suddenly needs stdexec on
// the include path. Nothing but an include from the wrong side is needed
// to break it, so the guard is a translation unit that includes the
// public header alone and refuses to compile if a provider came with it.

#include <blake3pp/io.hpp>

#include <doctest/doctest.h>

#if defined(STDEXEC_ATTRIBUTE) || defined(BEMAN_EXECUTION_TRY_EVAL) || \
    defined(BEMAN_EXECUTION_DELETE)
#error \
    "<blake3pp/io.hpp> pulled in an execution provider. Something under include/blake3pp reachable from io.hpp includes parallel.hpp, ex_compat.hpp or file_pipeline.hpp; the pipeline's provider-facing headers are included by the .cpp that drives them, never by the public I/O surface."
#endif

TEST_SUITE("io") {

TEST_CASE("io.hpp does not drag in an execution provider") {
  // The check is the #error above, at compile time. This case exists so
  // the guarantee appears in the run with the rest.
  CHECK(true);
}

}  // TEST_SUITE
