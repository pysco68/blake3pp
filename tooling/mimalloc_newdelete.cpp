// The global operator new/delete override, for one executable.
//
// mimalloc's header defines the replaceable global operators as ordinary
// (non-inline) functions, so it must be compiled into exactly ONE
// translation unit per program, which is why this is a source file rather
// than an include in some shared header. blake3pp_tool_allocator() adds it
// to each executable individually; two copies in one link is a duplicate
// symbol error, and that is the intended failure mode.
//
// WINDOWS ONLY. On POSIX, mimalloc's combined object already defines these
// operators (along with the wholesale malloc/free override), so compiling
// this into a POSIX link would collide with it. The choice lives in the
// top-level CMakeLists next to the reasoning for both platforms; this file
// is deliberately not self-guarding, so a mistake there fails loudly at
// link time instead of silently doing nothing.
//
// <mimalloc-new-delete.h> is reached through mimalloc-static's
// INTERFACE_INCLUDE_DIRECTORIES.

#include <mimalloc-new-delete.h>
