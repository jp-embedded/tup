/* jp_alloc.h - lock-free allocator (C11)
 *
 * Public interface for jp_alloc — a lock-free, EBR-protected, thread-caching
 * memory allocator. See jp_alloc.c for the implementation.
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
 *   -DJP_ALLOC_DEBUG          ABA / double-free / corruption self-checks
 *   -DJP_CACHE_N=32           Per-thread TLS cache cap per size class
 *   -DJP_REFILL=16            Batch size for global freelist refill (CASs/op)
 *   -DJP_ALLOC_POOL_COUNT=17  Number of power-of-2 pool classes (1..64K)
 *   -DJP_CACHELINE=64         Cache-line size for alignment padding
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

#endif /* JP_ALLOC_H */