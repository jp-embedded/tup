/* jp_alloc - per-thread buddy-split memory allocator
 *
 * https://github.com/jp-embedded/jp_alloc
 * GPL-3.0-or-later
 *
 * A per-thread memory allocator written in pure C11. Features:
 *
 * - Per-thread power-of-2 pools with binary buddy splitting (1B..8MB)
 * - Zero atomics on the hot path: alloc = freelist pop, free = freelist push
 * - No magazines, no EBR, no CAS, no global pools, no TLS cache array
 * - Safe payload-page reclamation for page-sized and larger pool blocks
 * - Windows (VirtualAlloc) and POSIX (mmap) backends
 * - mremap for large reallocs on Linux
 * - Portable to 32-bit and 64-bit (GCC 4.7+, Clang 3.0+, MSVC 2015+)
 *
 * Cross-thread free: when Thread B frees a block that Thread A allocated,
 * B pushes it onto B's own freelist. B can hand it out later.
 *
 * Cross-thread memory flow (A allocates, B frees) can cause A's freelist
 * to drain while B accumulates free blocks. Step 1 ignores this — A
 * buddy-splits or mmaps a new region. Step 2 (future) adds a global
 * return list for batch cross-thread transfer.
 */

/* mremap is Linux-only and requires _GNU_SOURCE before includes */
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <malloc.h>

#ifndef JP_ALLOC_IMPLEMENTATION
#define JP_ALLOC_IMPLEMENTATION
#endif
#include "jp_alloc.h"

#ifdef JP_ALLOC_DEBUG
#include <stdio.h>
#endif

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

/* ---- Portability fallbacks ---- */

#ifndef JP_CACHELINE
#define JP_CACHELINE 64
#endif

#ifndef _Alignas
#ifdef __GNUC__
#define _Alignas(x) __attribute__((aligned(x)))
#endif
#endif
#ifndef _Thread_local
#ifdef __GNUC__
#define _Thread_local __thread
#endif
#endif

#ifndef JP_ALLOC_INTERMEDIATE_K
#define JP_ALLOC_INTERMEDIATE_K 4
#endif

#ifndef JP_ALLOC_MADVISE_SIZE
#define JP_ALLOC_MADVISE_SIZE (64 * 1024)
#endif

#ifndef JP_ALLOC_MADVISE_MODE
#define JP_ALLOC_MADVISE_MODE JP_ALLOC_MADVISE_NONE
#endif

#if JP_ALLOC_MADVISE_MODE < JP_ALLOC_MADVISE_NONE || \
    JP_ALLOC_MADVISE_MODE > JP_ALLOC_MADVISE_FREE
#error "JP_ALLOC_MADVISE_MODE must be NONE, DONTNEED, or FREE"
#endif

/* ---- Pool table: power-of-2 + intermediate size classes ----
 *
 * K=0: pure power-of-2 (24 pools, 1..8M).
 * K=4: 28 pools. Intermediate = 2^i * 5 through the 4K parent.
 * K=6: 26 pools. Intermediate = 2^i * 21 through the 4K parent.
 * Intermediates with small < sizeof(union header) are skipped.
 * Intermediates with a parent above 4K are skipped so every asymmetric
 * split, and every resulting block, stays within one page.
 * Identity: 1*small + 3*intermediate = split_from (pow2).
 */

struct pool_info {
	uint32_t size;
	uint8_t  is_pow2;
	uint32_t split_from;
	uint32_t small_size;
};

#if JP_ALLOC_INTERMEDIATE_K == 0
static const struct pool_info g_pools[] = {
	{1u,1,0,0},{2u,1,0,0},{4u,1,0,0},{8u,1,0,0},{16u,1,0,0},{32u,1,0,0},
	{64u,1,0,0},{128u,1,0,0},{256u,1,0,0},{512u,1,0,0},{1024u,1,0,0},
	{2048u,1,0,0},{4096u,1,0,0},{8192u,1,0,0},{16384u,1,0,0},{32768u,1,0,0},
	{65536u,1,0,0},{131072u,1,0,0},{262144u,1,0,0},{524288u,1,0,0},
	{1048576u,1,0,0},{2097152u,1,0,0},{4194304u,1,0,0},{8388608u,1,0,0},
};
#elif JP_ALLOC_INTERMEDIATE_K == 4
static const struct pool_info g_pools[] = {
	{1u,1,0,0},{2u,1,0,0},{4u,1,0,0},{8u,1,0,0},{16u,1,0,0},{32u,1,0,0},
	{64u,1,0,0},{128u,1,0,0},
	{160u,0,512u,32u},
	{256u,1,0,0},
	{320u,0,1024u,64u},
	{512u,1,0,0},
	{640u,0,2048u,128u},
	{1024u,1,0,0},
	{1280u,0,4096u,256u},
	{2048u,1,0,0},
	{4096u,1,0,0},
	{8192u,1,0,0},
	{16384u,1,0,0},
	{32768u,1,0,0},
	{65536u,1,0,0},
	{131072u,1,0,0},
	{262144u,1,0,0},
	{524288u,1,0,0},
	{1048576u,1,0,0},
	{2097152u,1,0,0},
	{4194304u,1,0,0},
	{8388608u,1,0,0},
};
#elif JP_ALLOC_INTERMEDIATE_K == 6
static const struct pool_info g_pools[] = {
	{1u,1,0,0},{2u,1,0,0},{4u,1,0,0},{8u,1,0,0},{16u,1,0,0},{32u,1,0,0},
	{64u,1,0,0},{128u,1,0,0},{256u,1,0,0},{512u,1,0,0},
	{672u,0,2048u,32u},
	{1024u,1,0,0},
	{1344u,0,4096u,64u},
	{2048u,1,0,0},
	{4096u,1,0,0},
	{8192u,1,0,0},
	{16384u,1,0,0},
	{32768u,1,0,0},
	{65536u,1,0,0},
	{131072u,1,0,0},
	{262144u,1,0,0},
	{524288u,1,0,0},
	{1048576u,1,0,0},
	{2097152u,1,0,0},
	{4194304u,1,0,0},
	{8388608u,1,0,0},
};
#else
#error "JP_ALLOC_INTERMEDIATE_K must be 0, 4, or 6"
#endif

#define JP_POOL_COUNT (sizeof(g_pools) / sizeof(g_pools[0]))

/* __builtin_clzll fallback for MSVC */
#ifdef _MSC_VER
#include <intrin.h>
static inline int jp_clzll(unsigned long long x)
{
	unsigned long r;
	_BitScanReverse64(&r, x);
	return 63 - (int)r;
}
#else
#define jp_clzll(x) __builtin_clzll(x)
#endif

static inline size_t pool_id_by_size(size_t size)
{
	if(size <= 1) return 0;
	size_t exponent = 64 - (size_t)jp_clzll(size - 1);
#if JP_ALLOC_INTERMEDIATE_K == 0
	return exponent;
#elif JP_ALLOC_INTERMEDIATE_K == 4
	if(exponent < 8) return exponent;
	if(exponent <= 11) {
		size_t intermediate = (size_t)5 << (exponent - 3);
		return 2 * exponent - (size <= intermediate ? 8 : 7);
	}
	return exponent + 4;
#else
	if(exponent < 10) return exponent;
	if(exponent <= 11) {
		size_t intermediate = (size_t)21 << (exponent - 5);
		return 2 * exponent - (size <= intermediate ? 10 : 9);
	}
	return exponent + 2;
#endif
}

/* ---- Debug header (enabled by -DJP_ALLOC_DEBUG) ---- */
#ifdef JP_ALLOC_DEBUG
#define JP_UNSIZED_MAGIC 0x0BADDEA11DECULL
#define JP_STATE_FREE    0xDEADBEEFFULL
#define JP_STATE_LIVE    0xCAFEBABEULL
#define JP_FREE_COOKIE   0x9E3779B97F4A7C15ULL

#define JP_CHECK(cond, ...) do { \
	if(!(cond)) { fprintf(stderr, "jp_alloc: " __VA_ARGS__); abort(); } \
} while(0)
#endif

union header;

#ifdef JP_ALLOC_DEBUG
static inline uint64_t jp_free_cookie(union header *h, union header *next,
				      size_t pid)
{
	return JP_FREE_COOKIE ^ (uintptr_t)h ^ ((uintptr_t)next >> 4)
		^ ((uint64_t)pid << 48);
}
#endif

/* ---- OS page allocation ---- */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static size_t os_page_size(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return si.dwPageSize;
}

static void *os_alloc_pages(size_t size)
{
	return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void os_free_pages(void *mem, size_t size)
{
	(void)size;
	VirtualFree(mem, 0, MEM_RELEASE);
}

#else /* POSIX */

#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static size_t os_page_size(void)
{
	return sysconf(_SC_PAGESIZE);
}

static void *os_alloc_pages(size_t size)
{
	void *mem = mmap(0, size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(mem == MAP_FAILED) mem = NULL;
	return mem;
}

static void os_free_pages(void *mem, size_t size)
{
	munmap(mem, size);
}

#endif /* _WIN32 */

/* ---- Statistics (compiled in with -DJP_ALLOC_STATS) ---- */
#ifdef JP_ALLOC_STATS
#include <stdio.h>
static _Atomic(size_t) g_live_blocks[JP_POOL_COUNT];
static _Atomic(size_t) g_alloc_count[JP_POOL_COUNT];
static _Atomic(size_t) g_free_count[JP_POOL_COUNT];
static _Atomic(size_t) g_mmap_count[JP_POOL_COUNT];
static _Atomic(size_t) g_mmap_bytes[JP_POOL_COUNT];
static _Atomic(size_t) g_madvise_count[JP_POOL_COUNT];
static _Atomic(size_t) g_madvise_bytes[JP_POOL_COUNT];
static _Atomic(size_t) g_direct_mmap_bytes;
static _Atomic(size_t) g_direct_live_bytes;
#define JP_STAT_LIVE(pid)   __atomic_add_fetch(&g_live_blocks[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_DEAD(pid)   __atomic_sub_fetch(&g_live_blocks[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_ALLOC(pid)  __atomic_add_fetch(&g_alloc_count[pid], 1, __ATOMIC_RELAXED)
#define JP_STAT_FREE(pid)   __atomic_add_fetch(&g_free_count[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_MMAP(pid, bytes) do { \
	__atomic_add_fetch(&g_mmap_count[pid], 1, __ATOMIC_RELAXED); \
	__atomic_add_fetch(&g_mmap_bytes[pid], (bytes), __ATOMIC_RELAXED); \
} while(0)
#define JP_STAT_MADVISE(pid, bytes) do { \
	__atomic_add_fetch(&g_madvise_count[pid], 1, __ATOMIC_RELAXED); \
	__atomic_add_fetch(&g_madvise_bytes[pid], (bytes), __ATOMIC_RELAXED); \
} while(0)
#define JP_STAT_MMAP_DIRECT(bytes) \
	__atomic_add_fetch(&g_direct_mmap_bytes, (bytes), __ATOMIC_RELAXED)
#define JP_STAT_DIRECT_LIVE(bytes) \
	__atomic_add_fetch(&g_direct_live_bytes, (bytes), __ATOMIC_RELAXED)
#define JP_STAT_DIRECT_DEAD(bytes) \
	__atomic_sub_fetch(&g_direct_live_bytes, (bytes), __ATOMIC_RELAXED)
static void jp_alloc_stats_dump(void);
static void jp_alloc_stats_register_atexit(void) __attribute__((constructor));
static void jp_alloc_stats_register_atexit(void) { atexit(jp_alloc_stats_dump); }
#else
#define JP_STAT_LIVE(pid)   ((void)0)
#define JP_STAT_DEAD(pid)   ((void)0)
#define JP_STAT_ALLOC(pid)  ((void)0)
#define JP_STAT_FREE(pid)   ((void)0)
#define JP_STAT_MMAP(pid, bytes) ((void)0)
#define JP_STAT_MADVISE(pid, bytes) ((void)0)
#define JP_STAT_MMAP_DIRECT(bytes) ((void)0)
#define JP_STAT_DIRECT_LIVE(bytes) ((void)0)
#define JP_STAT_DIRECT_DEAD(bytes) ((void)0)
#endif

/* ---- Header'd path types ---- */

union header {
	struct {
		size_t size;
		union header *next;
#ifdef JP_ALLOC_DEBUG
		uint64_t magic;
		uint64_t state;
#endif
	} s;
	max_align_t _align;
};

/* ---- Per-thread pool state ----
 *
 * Each thread has its own freelist per pool class.Alloc pops from the
 * freelist (one pointer read + write, zero atomics). Free pushes to
 * the freelist (one pointer write, zero atomics). When a freelist is
 * empty, pool_get buddy-splits from the next larger pool's freelist
 * (same thread, zero atomics). When the largest pool is empty, mmap
 * a new 8M-aligned region.
 *
 * Cross-thread: Thread B freeing a block from Thread A's region pushes
 * it onto B's own freelist. B can reuse it later. */

struct tls_state {
	union header *freelist[JP_POOL_COUNT];
};

static _Thread_local struct tls_state tls;
static _Thread_local int tls_registered = 0;

/* ---- pthread TLS init ---- */
#include <pthread.h>

static pthread_key_t  g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_destructor(void *p);

static void tls_init_once(void)
{
	pthread_key_create(&g_tls_key, tls_destructor);
}

static void tls_register(void)
{
	if(tls_registered) return;
	pthread_once(&g_tls_once, tls_init_once);
	pthread_setspecific(g_tls_key, (void *)(uintptr_t)1);
	tls_registered = 1;
}

/* TLS destructor drops this thread's freelist pointers. The underlying
 * regions remain mapped and are reclaimed by the OS at process exit. */
static void tls_destructor(void *p)
{
	(void)p;
	if(!tls_registered) return;
	tls_registered = 0;
}

/* ---- Pool operations (per-thread, zero atomics) ---- */

static inline size_t pool_id(size_t size)
{
	return pool_id_by_size(size);
}

static int is_pow2(size_t n)
{
	return (n & (n - 1)) == 0;
}

static void pool_put(union header *h, size_t pid);

/* pool_get: pop from freelist[pid]. If empty, split from a larger pool.
 * For power-of-2 pools: binary buddy split (half + half).
 * For intermediate pools: asymmetric split from next pow2 pool
 * (1*small + 3*intermediate = pow2). */
static union header *pool_get(size_t pid)
{
	/* Fast path: pop from freelist. */
	if(likely(tls.freelist[pid] != NULL)) {
		union header *h = tls.freelist[pid];
		union header *next = h->s.next;
#ifdef JP_ALLOC_DEBUG
		JP_CHECK(h->s.state == JP_STATE_FREE,
			 "pool_get: block %p is not free (state=%llx)\n", (void *)h,
			 (unsigned long long)h->s.state);
		JP_CHECK(h->s.magic == jp_free_cookie(h, next, pid),
			 "pool_get: free block %p cookie mismatch\n", (void *)h);
#endif
		tls.freelist[pid] = next;
		return h;
	}
	/* Empty: split from a larger pool. */
	if(likely(pid < JP_POOL_COUNT - 1)) {
		if(g_pools[pid].is_pow2) {
			/* Binary buddy: split next pow2 pool into two halves.
			 * Skip intermediate pools — they're not 2x the child. */
			size_t next_pid = pid + 1;
			while(next_pid < JP_POOL_COUNT && !g_pools[next_pid].is_pow2)
				next_pid++;
			union header *big = pool_get(next_pid);
			if(!big) return NULL;
			size_t sz = g_pools[pid].size;
			union header *spare = (union header *)((char *)big + sz);
			pool_put(spare, pid);
			return big;
		} else {
			/* Asymmetric: carve from next pow2 pool.
			 * 1*small + 3*intermediate = pow2. */
			size_t pow2_pid = pool_id_by_size(g_pools[pid].split_from);
			union header *big = pool_get(pow2_pid);
			if(!big) return NULL;
			size_t inter_sz = g_pools[pid].size;
			size_t small_sz = g_pools[pid].small_size;
			char *p = (char *)big;
			for(int i = 0; i < 3; i++) {
				union header *h = (union header *)p;
				pool_put(h, pid);
				p += inter_sz;
			}
			size_t small_pid = pool_id_by_size(small_sz);
			union header *small_h = (union header *)p;
			pool_put(small_h, small_pid);
			return pool_get(pid);
		}
	}
	/* Largest pool: mmap a new region. */
	size_t sz = g_pools[pid].size;
	union header *h = (union header *)os_alloc_pages(sz);
	if(!h) return NULL;
	JP_STAT_MMAP(pid, sz);
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_FREE;
#endif
	return h;
}

/* pool_put: push to this thread's freelist[pid]. Zero atomics. */
static void pool_put(union header *h, size_t pid)
{
	union header *next = tls.freelist[pid];
#ifdef JP_ALLOC_DEBUG
	JP_CHECK(next != h, "pool_put: duplicate free of block %p\n", (void *)h);
#endif
	h->s.next = next;
#ifdef JP_ALLOC_DEBUG
	h->s.magic = jp_free_cookie(h, next, pid);
	h->s.state = JP_STATE_FREE;
#endif
	tls.freelist[pid] = h;
}

static void pool_release(union header *h, size_t pid)
{
#if !defined(_WIN32) && JP_ALLOC_MADVISE_MODE != JP_ALLOC_MADVISE_NONE
	size_t block_size = g_pools[pid].size;
	size_t page_size = os_page_size();
	if(block_size >= JP_ALLOC_MADVISE_SIZE && block_size > page_size) {
#if JP_ALLOC_MADVISE_MODE == JP_ALLOC_MADVISE_FREE && defined(MADV_FREE)
		madvise((char *)h + page_size, block_size - page_size, MADV_FREE);
#elif JP_ALLOC_MADVISE_MODE == JP_ALLOC_MADVISE_DONTNEED
		madvise((char *)h + page_size, block_size - page_size, MADV_DONTNEED);
#endif
	}
#endif
	pool_put(h, pid);
}

/* ---- Aligned page allocation ---- */

static void *alloc_pages_aligned(size_t alignment, size_t size)
{
	if(unlikely(!is_pow2(alignment))) return NULL;
	const size_t ps = os_page_size();
	size_t pre_padding = 0;
	size_t align_size = 0;
	if(alignment > ps) {
		pre_padding = ps - sizeof(union header);
		align_size = alignment - ps;
	} else if(alignment > sizeof(union header)) {
		pre_padding = alignment - sizeof(union header);
	} else {
		alignment = sizeof(union header);
	}
	size_t span_size = pre_padding + size + align_size;
	size_t span_size_rounded = (span_size + ps - 1) & ~(ps - 1);
	char *span = (char *)os_alloc_pages(span_size_rounded);
	if(unlikely(span == NULL)) return NULL;
	char *hdr = span + pre_padding;
	size_t offset = (alignment - ((size_t)(hdr + sizeof(union header)) & (alignment - 1))) & (alignment - 1);
	hdr += offset;
	if(align_size > 0) {
		size_t pre_size = offset;
		size_t post_size = align_size - pre_size;
		if(pre_size > 0) os_free_pages(span, pre_size);
		if(post_size > 0) os_free_pages(span + span_size_rounded - post_size, post_size);
	}
	return hdr;
}

/* ---- Public API ---- */

size_t jp_good_size(size_t size)
{
	size_t pid = pool_id(size);
	if(likely(pid < JP_POOL_COUNT)) {
		size = g_pools[pid].size;
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
	}
	return size - sizeof(union header);
}

static size_t sized_pool_id(size_t size)
{
	if(size < sizeof(union header)) size = sizeof(union header);
	return pool_id(size);
}

void *jp_alloc_sized(size_t size)
{
	size_t pid = sized_pool_id(size);
	if(likely(pid < JP_POOL_COUNT)) {
		tls_register();
		union header *h = pool_get(pid);
		if(!h) return NULL;
		JP_STAT_ALLOC(pid);
		JP_STAT_LIVE(pid);
		return h;
	}

	size_t page_mask = os_page_size() - 1;
	size_t alloc_size = (size + page_mask) & ~page_mask;
	void *mem = os_alloc_pages(alloc_size);
	if(mem) {
		JP_STAT_MMAP_DIRECT(alloc_size);
		JP_STAT_DIRECT_LIVE(alloc_size);
	}
	return mem;
}

void jp_free_sized(void *mem, size_t size)
{
	if(!mem) return;
	size_t pid = sized_pool_id(size);
	if(likely(pid < JP_POOL_COUNT)) {
		JP_STAT_FREE(pid);
		JP_STAT_DEAD(pid);
		pool_release((union header *)mem, pid);
		return;
	}

	size_t page_mask = os_page_size() - 1;
	size_t alloc_size = (size + page_mask) & ~page_mask;
	JP_STAT_DIRECT_DEAD(alloc_size);
	os_free_pages(mem, alloc_size);
}

void *jp_realloc_sized(void *mem, size_t old_size, size_t new_size)
{
	if(!mem) return jp_alloc_sized(new_size);
	if(new_size == 0) {
		jp_free_sized(mem, old_size);
		return NULL;
	}

	size_t old_pid = sized_pool_id(old_size);
	size_t new_pid = sized_pool_id(new_size);
	if(old_pid == new_pid && old_pid < JP_POOL_COUNT) return mem;

	void *new_mem = jp_alloc_sized(new_size);
	if(!new_mem) return NULL;
	memcpy(new_mem, mem, old_size < new_size ? old_size : new_size);
	jp_free_sized(mem, old_size);
	return new_mem;
}

void *jp_pool_alloc(const struct jp_pool_config *pool)
{
	if(!pool || pool->alignment > _Alignof(max_align_t)) {
		errno = EINVAL;
		return NULL;
	}
	return jp_alloc_sized(pool->size);
}

void jp_pool_free(const struct jp_pool_config *pool, void *mem)
{
	if(!pool) return;
	jp_free_sized(mem, pool->size);
}

void jp_alloc_reset(void)
{
	/* No-op. Pool memory stays mapped; OS reclaims at exit. */
}

/* ---- Statistics dump ---- */
#ifdef JP_ALLOC_STATS
static size_t count_freelist_blocks(union header *head)
{
	size_t total = 0;
	size_t guard = 0;
	while(head && guard < 10000000) {
		total++;
		head = head->s.next;
		guard++;
	}
	return total;
}

static void jp_alloc_stats_dump(void)
{
	FILE *f = stderr;
	size_t total_live = 0, total_allocs = 0, total_frees = 0;
	size_t total_mmap_bytes = 0, total_free_blocks = 0, total_madvise = 0;

	fprintf(f, "\n=== jp_alloc STATS ===\n");
	fprintf(f, "  %-4s %-10s %-12s %-12s %-12s %-14s %-14s %-14s %-14s\n",
		"pid", "block_sz", "live", "allocs", "frees",
		"mmap_bytes", "freelist", "madvise_cnt", "live_bytes");
	for(size_t pid = 0; pid < JP_POOL_COUNT; pid++) {
		size_t block_sz = g_pools[pid].size;
		size_t live = __atomic_load_n(&g_live_blocks[pid], __ATOMIC_RELAXED);
		size_t alloc = __atomic_load_n(&g_alloc_count[pid], __ATOMIC_RELAXED);
		size_t free = __atomic_load_n(&g_free_count[pid], __ATOMIC_RELAXED);
		size_t mmap_bytes = __atomic_load_n(&g_mmap_bytes[pid], __ATOMIC_RELAXED);
		size_t fl = count_freelist_blocks(tls.freelist[pid]);
		size_t madv = __atomic_load_n(&g_madvise_count[pid], __ATOMIC_RELAXED);
		size_t live_bytes = live * block_sz;
		if(live == 0 && alloc == 0 && mmap_bytes == 0 && fl == 0 && madv == 0)
			continue;
		fprintf(f, "  %-4zu %-10zu %-12zu %-12zu %-12zu %-14zu %-14zu %-14zu %-14zu\n",
			pid, block_sz, live, alloc, free,
			mmap_bytes, fl, madv, live_bytes);
		total_live += live;
		total_allocs += alloc;
		total_frees += free;
		total_mmap_bytes += mmap_bytes;
		total_free_blocks += fl;
		total_madvise += madv;
	}
	size_t direct = __atomic_load_n(&g_direct_mmap_bytes, __ATOMIC_RELAXED);
	size_t direct_live = __atomic_load_n(&g_direct_live_bytes, __ATOMIC_RELAXED);
	fprintf(f, "  ---\n");
	fprintf(f, "  total live blocks  : %zu\n", total_live);
	fprintf(f, "  total allocs/frees : %zu / %zu (delta %zu)\n",
		total_allocs, total_frees, total_allocs - total_frees);
	fprintf(f, "  total mmap (pools) : %zu bytes (%.1f MB)\n",
		total_mmap_bytes, total_mmap_bytes / 1048576.0);
	fprintf(f, "  direct mmap (large): %zu bytes (%.1f MB), live: %zu bytes (%.1f MB)\n",
		direct, direct / 1048576.0, direct_live, direct_live / 1048576.0);
	fprintf(f, "  freelist blocks    : %zu (this thread)\n", total_free_blocks);
	fprintf(f, "  pool madvise       : %zu calls\n", total_madvise);
	fprintf(f, "=== end STATS ===\n\n");
	fflush(f);
}
#endif

#ifdef JP_ALLOC_DEBUG
void jp_alloc_diag(size_t *hits, size_t *misses)
{
	if(hits)   *hits   = 0;
	if(misses) *misses = 0;
}
#endif

/* ---- malloc/free implementation ---- */

void jp_free(void *mem)
{
	if(unlikely(mem == NULL)) return;

	union header *h = (union header *)mem - 1;
#ifdef JP_ALLOC_DEBUG
	JP_CHECK(h->s.magic == JP_UNSIZED_MAGIC || h->s.magic == 0,
		 "jp_free: double free or corruption on %p (magic=%llx)\n",
		 mem, (unsigned long long)h->s.magic);
	JP_CHECK(h->s.state == JP_STATE_LIVE || h->s.state == 0,
		 "jp_free: double free on %p (state=%llx)\n",
		 mem, (unsigned long long)h->s.state);
	h->s.state = JP_STATE_FREE;
#endif
	size_t size = h->s.size;
	if(likely(size < JP_POOL_COUNT)) {
		JP_STAT_FREE(size);
		JP_STAT_DEAD(size);
#ifndef _WIN32
		/* Keep the metadata page and discard only complete payload pages
		 * before publishing the block on a freelist. */
		size_t block_size = g_pools[size].size;
		size_t page_size = os_page_size();
		if(block_size >= JP_ALLOC_MADVISE_SIZE && block_size > page_size)
			madvise((char *)h + page_size, block_size - page_size, MADV_DONTNEED);
#endif
		pool_release(h, size);
	} else {
		JP_STAT_DIRECT_DEAD(size);
		size_t pre_padding = (size_t)mem & (os_page_size() - 1);
		os_free_pages((char *)h + pre_padding, size + pre_padding);
	}
}

void *jp_alloc(size_t size)
{
	size += sizeof(union header);
	void *mem;
	size_t pid = pool_id(size);
	if(likely(pid < JP_POOL_COUNT)) {
		tls_register();
		union header *h = pool_get(pid);
		if(h == NULL) return NULL;
		h->s.size = pid;
		JP_STAT_ALLOC(pid);
		JP_STAT_LIVE(pid);
#ifdef JP_ALLOC_DEBUG
		h->s.magic = JP_UNSIZED_MAGIC;
		h->s.state = JP_STATE_LIVE;
#endif
		mem = h + 1;
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
		union header *h = (union header *)os_alloc_pages(size);
		if(h == NULL) return NULL;
		h->s.size = size;
		JP_STAT_MMAP_DIRECT(size);
		JP_STAT_DIRECT_LIVE(size);
#ifdef JP_ALLOC_DEBUG
		h->s.magic = JP_UNSIZED_MAGIC;
		h->s.state = JP_STATE_LIVE;
#endif
		mem = h + 1;
	}
	return mem;
}

void *jp_alloc_aligned(size_t alignment, size_t size)
{
	size += sizeof(union header);
	size_t ps_mask = os_page_size() - 1;
	size_t span = (size + ps_mask) & ~ps_mask;
	void *mem = alloc_pages_aligned(alignment, size);
	if(mem == NULL) return NULL;
	JP_STAT_MMAP_DIRECT(span);
	JP_STAT_DIRECT_LIVE(span);
	union header *h = (union header *)mem;
	h->s.size = size;
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_LIVE;
#endif
	return (union header *)mem + 1;
}

void *jp_calloc(size_t num, size_t nsize)
{
	size_t size = num * nsize;
	if(num && nsize != size / num) {
		errno = ENOMEM;
		return NULL;
	}
	void *mem = jp_alloc(size);
	if(mem) memset(mem, 0, size);
	return mem;
}

void *jp_realloc(void *mem, size_t new_size)
{
	size_t size = 0;
	if(mem != NULL) {
		union header *h = (union header *)mem - 1;
#ifdef JP_ALLOC_DEBUG
		JP_CHECK(h->s.magic == JP_UNSIZED_MAGIC,
			 "jp_realloc: wrong API on %p (magic=%llx)\n",
			 mem, (unsigned long long)h->s.magic);
		JP_CHECK(h->s.state == JP_STATE_LIVE,
			 "jp_realloc: non-live block %p (state=%llx)\n",
			 mem, (unsigned long long)h->s.state);
#endif
		size = h->s.size;
		if(likely(size < JP_POOL_COUNT)) size = g_pools[size].size;
		size -= sizeof(union header);
	}
	if(new_size > size) {
		if(mem != NULL) {
			union header *h = (union header *)mem - 1;
			size_t hdr_size = h->s.size;
			if(hdr_size >= JP_POOL_COUNT) {
				size_t pre_padding = (size_t)mem & (os_page_size() - 1);
				char *base = (char *)h + pre_padding;
#ifdef __linux__
				size_t new_total = new_size + sizeof(union header) + pre_padding;
				size_t ps_mask = os_page_size() - 1;
				new_total = (new_total + ps_mask) & ~ps_mask;
				void *new_base = mremap(base, hdr_size + pre_padding, new_total, MREMAP_MAYMOVE);
				if(new_base != MAP_FAILED) {
					union header *new_h = (union header *)((char *)new_base + pre_padding);
					new_h->s.size = new_total;
					return new_h + 1;
				}
#endif
			}
		}
		void *new_mem = jp_alloc(new_size);
		if(new_mem) memcpy(new_mem, mem, size);
		jp_free(mem);
		mem = new_mem;
	} else if(new_size == 0) {
		jp_free(mem);
		mem = NULL;
	}
	return mem;
}

/* ---- libc malloc/free/calloc/realloc overrides ---- */

void free(void *mem) { jp_free(mem); }
void *malloc(size_t size) { return jp_alloc(size); }
void *calloc(size_t num, size_t nsize) { return jp_calloc(num, nsize); }
void *realloc(void *mem, size_t new_size) { return jp_realloc(mem, new_size); }

void *valloc(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
void *memalign(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }
void *pvalign(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
void *aligned_alloc(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *mem = jp_alloc_aligned(alignment, size);
	if(mem == NULL) return ENOMEM;
	*memptr = mem;
	return 0;
}

/* ---- Platform-specific malloc extensions (gated per OS) ---- */

#if defined(__GLIBC__) || defined(__linux__)

size_t malloc_usable_size(void *ptr)
{
	union header *h = (union header *)ptr - 1;
	size_t size = h->s.size;
	if(likely(size < JP_POOL_COUNT)) size = g_pools[size].size;
	return size - sizeof(union header);
}

int mallopt(int param, int value)
{
	(void)param;
	(void)value;
	return 0;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
	size_t total_size = nmemb * size;
	if(nmemb && size != total_size / nmemb) {
		errno = ENOMEM;
		return NULL;
	}
	return jp_realloc(ptr, total_size);
}

void cfree(void *mem) { jp_free(mem); }

#endif /* __GLIBC__ || __linux__ */

#if defined(__APPLE__) || defined(__BSD__)

size_t malloc_size(void *ptr) { return malloc_usable_size(ptr); }
size_t malloc_good_size(size_t size) { return jp_good_size(size); }

#endif /* __APPLE__ || __BSD__ */
