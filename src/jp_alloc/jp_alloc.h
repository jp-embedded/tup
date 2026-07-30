/* jp_alloc.h - C interface for jp_alloc memory allocator
 *
 * Sized API (jp_alloc_sized/jp_free_sized): headerless allocations for
 * callers that know the size at free time. Saves 16 bytes per object
 * vs the standard malloc path. Use jp_realloc_sized for resize.
 *
 * When jp_alloc.c is not linked (TUP_USE_JP_ALLOC=n), the sized API
 * falls back to libc malloc (the size hint is ignored).
 */
#ifndef JP_ALLOC_H
#define JP_ALLOC_H

#include <stddef.h>

#ifdef JP_ALLOC_COMPILED
/* jp_alloc.c is linked — use the real headerless sized API */
void *jp_alloc_sized(size_t size);
void  jp_free_sized(void *mem, size_t size);
void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz);
void  jp_alloc_reset(void);
#else
/* jp_alloc.c is NOT linked — fall back to libc malloc (size hint ignored) */
#include <stdlib.h>

static inline void *jp_alloc_sized(size_t size) { return malloc(size); }
static inline void  jp_free_sized(void *mem, size_t size) { (void)size; free(mem); }
static inline void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz) { (void)oldsz; return realloc(mem, newsz); }
static inline void  jp_alloc_reset(void) { }
#endif

#endif /* JP_ALLOC_H */