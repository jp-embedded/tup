/* jp_alloc.h - C interface for jp_alloc memory allocator
 *
 * Sized API (jp_alloc_sized/jp_free_sized): headerless allocations for
 * callers that know the size at free time. Saves 16 bytes per object
 * vs the standard malloc path. Use jp_realloc_sized for resize.
 *
 * On Windows, the sized API falls back to libc malloc (the size hint
 * is ignored). jp_alloc's global malloc override is non-Windows only.
 */
#ifndef JP_ALLOC_H
#define JP_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN32
/* Headerless sized API — caller must pass the same size to free as to alloc.
 * Do NOT mix with malloc/free on the same pointer. */
void *jp_alloc_sized(size_t size);
void  jp_free_sized(void *mem, size_t size);
void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz);

/* No-op for valgrind cleanup compatibility (replaces mempool_clear). */
void  jp_alloc_reset(void);
#else
/* Windows fallback: libc malloc wrappers */
#include <stdlib.h>

static inline void *jp_alloc_sized(size_t size) { return malloc(size); }
static inline void  jp_free_sized(void *mem, size_t size) { (void)size; free(mem); }
static inline void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz) { (void)oldsz; return realloc(mem, newsz); }
static inline void  jp_alloc_reset(void) { }
#endif

#ifdef __cplusplus
}
#endif

#endif /* JP_ALLOC_H */
