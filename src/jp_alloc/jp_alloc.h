/* jp_alloc.h - lock-free allocator (C11)
 *
 * Public interface for jp_alloc, a per-thread buddy-split allocator.
 * See jp_alloc.c for the implementation.
 *
 * Usage:
 *   1. Link jp_alloc.c into your binary. It overrides malloc/free/calloc/
 *      realloc globally, so all allocations (including libc-internal) route
 *      through jp_alloc's pools.
 *   2. #include this header in any file that wants the jp_* API directly.
 *   3. Define JP_ALLOC_IMPLEMENTATION before including this header in
 *      jp_alloc.c to get the real declaration of jp_alloc_reset (not the
 *      inline stub).
 *
 * Configuration (compile-time, all optional):
 *   -DJP_ALLOC_DEBUG             Free-list/double-free/corruption checks
 *   -DJP_ALLOC_INTERMEDIATE_K=4  Intermediate class ratio (0, 4, or 6)
 *   -DJP_ALLOC_MADVISE_SIZE=64K Minimum advised pool block size
 *   -DJP_ALLOC_MADVISE_MODE=... NONE, DONTNEED, or Linux FREE
 *
 * Platform requirements:
 *   - GCC 4.7+ or Clang 3.0+ (uses __atomic builtins, _Thread_local, _Alignas)
 *   - MSVC 2015+ with the compiler fallbacks in jp_alloc.c
 *   - POSIX (pthreads) or Windows with MinGW (WinPthread)
 *   - 32-bit and 64-bit supported; no platform-specific intrinsics
 */
#ifndef JP_ALLOC_H
#define JP_ALLOC_H

#include <stddef.h>

#define JP_ALLOC_MADVISE_NONE      0
#define JP_ALLOC_MADVISE_DONTNEED  1
#define JP_ALLOC_MADVISE_FREE      2

#ifdef JP_ALLOC_IMPLEMENTATION
/* jp_alloc.c is linked — real definition of jp_alloc_reset lives there */
void jp_alloc_reset(void);
#else
/* Inline stub used when jp_alloc.c is not linked (no-op cleanup hook) */
static inline void jp_alloc_reset(void) { }
#endif

/* Direct API — also exported as malloc/free/calloc/realloc overrides */
void *jp_alloc(size_t size);
void  jp_free(void *mem);
void *jp_calloc(size_t num, size_t nsize);
void *jp_realloc(void *mem, size_t new_size);
void *jp_alloc_aligned(size_t alignment, size_t size);
size_t jp_good_size(size_t size);

struct jp_pool_config {
	size_t size;
	size_t alignment;
};

#define JP_POOL_CONFIG(type) { sizeof(type), _Alignof(type) }

#ifdef JP_ALLOC_FALLBACK
#include <stdlib.h>
static inline void *jp_alloc_sized(size_t size) { return malloc(size); }
static inline void jp_free_sized(void *mem, size_t size) { (void)size; free(mem); }
static inline void *jp_realloc_sized(void *mem, size_t old_size, size_t new_size)
{
	(void)old_size;
	return realloc(mem, new_size);
}
static inline void *jp_pool_alloc(const struct jp_pool_config *pool)
{
	return malloc(pool->size);
}
static inline void jp_pool_free(const struct jp_pool_config *pool, void *mem)
{
	(void)pool;
	free(mem);
}
#else
void *jp_alloc_sized(size_t size);
void  jp_free_sized(void *mem, size_t size);
void *jp_realloc_sized(void *mem, size_t old_size, size_t new_size);
void *jp_pool_alloc(const struct jp_pool_config *pool);
void  jp_pool_free(const struct jp_pool_config *pool, void *mem);
#endif

#endif /* JP_ALLOC_H */
