/* jp_alloc.h - C interface for jp_alloc memory allocator
 *
 * The headerless sized API has been removed. All allocation now flows
 * through the unified header'd malloc/free path implemented in jp_alloc.c.
 * The sized entry points (jp_alloc_sized/jp_free_sized/jp_realloc_sized)
 * remain as inline libc wrappers so callers keep compiling; the size hint
 * is ignored.
 *
 * jp_alloc_reset() and jp_alloc_stats() have real definitions in
 * jp_alloc.c (compiled when JP_ALLOC_IMPLEMENTATION is defined before
 * including this header) and inline no-op/zero stubs otherwise.
 */
#ifndef JP_ALLOC_H
#define JP_ALLOC_H

#include <stddef.h>
#include <stdlib.h>

/* Sized API: inline libc wrappers (size hint ignored). The headerless
 * sized pool was removed; callers now use the unified header'd path. */
static inline void *jp_alloc_sized(size_t size) { return malloc(size); }
static inline void  jp_free_sized(void *mem, size_t size) { (void)size; free(mem); }
static inline void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz) { (void)oldsz; return realloc(mem, newsz); }

#ifdef JP_ALLOC_IMPLEMENTATION
/* jp_alloc.c is linked — real definitions live there. */
void  jp_alloc_reset(void);
/* Per-thread cache hit/miss counters. Counted only when JP_ALLOC_DEBUG
 * is defined in the linked jp_alloc.c; in a release build the function
 * exists but returns zeros (the increment sites compile out). Used by
 * the bench to find the throughput-optimal cache cap. */
void  jp_alloc_stats(size_t *hits, size_t *misses);
#else
/* jp_alloc.c is NOT linked — provide inline stubs. */
static inline void  jp_alloc_reset(void) { }
/* No jp_alloc.c, so no cache to instrument; return zeros as a sentinel. */
static inline void  jp_alloc_stats(size_t *hits, size_t *misses) { if(hits) *hits = 0; if(misses) *misses = 0; }
#endif

#endif /* JP_ALLOC_H */