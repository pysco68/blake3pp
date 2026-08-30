// The global operator new/delete override, for one executable.
//
// mimalloc's header defines the replaceable global operators as ordinary
// (non-inline) functions, so it must be compiled into exactly ONE
// translation unit per program, which is why this is a source file rather
// than an include in some shared header. blake3pp_tool_allocator() adds it
// to each executable individually; two copies in one link is a duplicate
// symbol error, and that is the intended failure mode.
//
// WINDOWS AND MACOS ONLY. On ELF POSIX, mimalloc's force-loaded archive
// already provides the wholesale malloc/free override (which operator
// new/delete reach through malloc), so compiling this into such a link
// would collide with its own new/delete replacements. On Windows and
// macOS the static archive is built WITHOUT the override machinery
// (unreliable there: redirect DLL resp. malloc-zone crash; see the
// top-level CMakeLists), and this TU is the allocator hookup. The choice
// lives in the top-level CMakeLists next to the reasoning for all three
// platforms; this file is deliberately not self-guarding, so a mistake
// there fails loudly at link time instead of silently doing nothing.
//
// <mimalloc-new-delete.h> is reached through mimalloc-static's
// INTERFACE_INCLUDE_DIRECTORIES.

#include <mimalloc-new-delete.h>
