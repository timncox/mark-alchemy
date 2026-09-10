/*
 * Compiles the vendored Mark engine with calloc/free redirected to the
 * SDRAM bump allocator (versio_alloc.c, carried over from the Smack ports;
 * the name is the file's provenance, not the platform).
 *
 * The engine's five loop tracks plus the undo buffer are ~63 MB at the
 * capacity set in vendor/mark_core.h; only SDRAM can hold them and Daisy has
 * no heap to speak of. Doing the redirect here rather than as a global -D
 * keeps it scoped to this translation unit -- libDaisy, the Alchemy SDK and
 * everything else keep the real calloc/free. stdlib.h is included FIRST so
 * the macros rewrite call sites only, never the library's own declarations.
 *
 * The engine also calls malloc()/free() on paths that only run for
 * external FX modules and sessions, which cannot start on this board
 * (host_stubs.c); malloc stays the real one, and nothing reaches it.
 *
 * Build this file; do NOT add vendor/mark_core.c to the source list too, or
 * you get duplicate symbols.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "versio_alloc.h"

#define calloc versio_calloc
#define free   versio_free

#include "vendor/mark_core.c"
