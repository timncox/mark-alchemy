/*
 * versio_alloc — a bump allocator so the vendored engine needs no edits.
 *
 * mark_create() does twelve calloc()s (five loop tracks and the undo buffer
 * at MARK_MAX_SECONDS each, five delay lines, the engine struct) and
 * mark_destroy() frees them. Daisy has no meaningful heap, and rewriting
 * mark_create() would fork the engine.
 *
 * Instead the ARM build compiles smack_core.c with
 *     -Dcalloc=versio_calloc -Dfree=versio_free
 * pointing those two calls at an SDRAM pool. Allocation happens once at boot
 * and is never returned, so free() is a no-op -- correct here, because the
 * module creates exactly one engine and keeps it until power-off. It also
 * means the engine's capacity FALLBACK LADDER cannot work (a failed rung's
 * buffers are never reclaimed), so MARK_MAX_SECONDS must fit the pool on
 * the first attempt; test/test_alloc.c proves it does.
 *
 * The pool itself is declared in mark_alchemy.cpp (it needs DSY_SDRAM_BSS
 * from libDaisy); this file stays plain C so it can be unit-tested natively.
 */
#ifndef VERSIO_ALLOC_H
#define VERSIO_ALLOC_H

#include <stddef.h>

/*
 * Pool size, defined here rather than in mark_alchemy.cpp so the firmware and
 * test/test_alloc.c cannot disagree about it (smack-versio once let them
 * drift and the test kept passing on a pool the firmware no longer used).
 *
 * The loop buffers dominate: (MARK_TRACKS + 1 undo) * MARK_MAX_SECONDS *
 * 48000 * 2 channels * 2 bytes = 63.4 MB at 55 s, plus five 256 KB delay
 * lines and the engine struct. The Alchemy SDK keeps ~64 KB of SDRAM for
 * itself (HostLink buffers, from the smack-alchemy .map), so 62 MiB leaves
 * that and a margin inside the Seed2 DFM's 64 MiB.
 *
 * Deliberately NOT derived from MARK_MAX_SECONDS here: mark_core.h declares
 * no linkage of its own, and the firmware includes it inside an extern "C"
 * block. Pulling it in from this header -- which is included first -- would
 * bind the engine's symbols with C++ linkage and the include guard would then
 * skip the wrapped copy, so every mark_* call fails to link. The relationship
 * to the capacity is asserted in test/test_alloc.c instead, where both
 * headers are C and can be compared safely.
 */
#define VERSIO_POOL_BYTES (62u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

void   versio_alloc_init(void *pool, size_t bytes);
void  *versio_calloc(size_t nmemb, size_t size);
void   versio_free(void *ptr);

/* For the boot-time log: how much of the pool the engine actually took. */
size_t versio_alloc_used(void);
size_t versio_alloc_capacity(void);

/* Set when an allocation did not fit. If this is true after smack_create(),
 * the module is broken and must say so rather than run half-initialised.
 * (After mark_create(): a leaked ladder rung would show here too.) */
int    versio_alloc_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* VERSIO_ALLOC_H */
