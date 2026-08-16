/* jp_alloc - lock-free memory allocator
 *
 * https://github.com/jp-embedded/jp_alloc
 * GPL-3.0-or-later
 *
 * A lock-free, EBR-protected, thread-caching memory allocator written in
 * pure C11. Features:
 *
 * - 64-bit CAS freelist with 3-epoch-ring EBR for ABA-freedom
 * - Per-thread fixed-array cache (N=32 per size class) with no atomics
 *   on the hot path
 * - Magazine-based global freelist: refill = pop 1 magazine + memcpy 16
 *   pointers; flush = memcpy 16 pointers + 1 CAS push. No dependent-
 *   load walks, no in-band chain-building loops
 * - Magazines allocated from the pool system (pool 8 = 256B blocks),
 *   recycled via CAS-based free-list — no static arrays, no mmap
 * - Binary buddy splitting with power-of-2 size classes (1B..8MB)
 * - Demand paging: the 8MB pool reserve only commits touched pages
 * - madvise(MADV_DONTNEED) at EBR drain time for blocks >= 4KB,
 *   returning internal-fragmentation pages to the OS
 * - Windows (VirtualAlloc) and POSIX (mmap) backends
 * - mremap for large reallocs on Linux
 * - Portable to 32-bit and 64-bit (GCC 4.7+, Clang 3.0+, MSVC 2015+)
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

/* Define JP_ALLOC_IMPLEMENTATION before including the header so the real
 * (non-inline) declaration of jp_alloc_reset() is visible in this
 * translation unit. This also silences -Wmissing-prototypes. */
#ifndef JP_ALLOC_IMPLEMENTATION
#define JP_ALLOC_IMPLEMENTATION
#endif
#include "jp_alloc.h"

#ifdef JP_ALLOC_DEBUG
#include <stdio.h>
#endif

/* ---- Cache hit/miss instrumentation (bench-only, compiled out in release) ---- */
#ifdef JP_ALLOC_DEBUG
static _Atomic(size_t) g_cache_hits;
static _Atomic(size_t) g_cache_misses;
#define JP_COUNT_HIT   do { __atomic_add_fetch(&g_cache_hits,   1, __ATOMIC_RELAXED); } while(0)
#define JP_COUNT_MISS  do { __atomic_add_fetch(&g_cache_misses, 1, __ATOMIC_RELAXED); } while(0)
#else
#define JP_COUNT_HIT   ((void)0)
#define JP_COUNT_MISS  ((void)0)
#endif

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

/* ---- Portability fallbacks ---- */

#ifndef JP_ALLOC_POOL_COUNT
#define JP_ALLOC_POOL_COUNT 24  /* Gives pools of 1 - 8M */
#endif

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

/* madvise(MADV_DONTNEED) returns freed large-block pages to the OS,
 * reducing RSS. Only applies to pools where blocks are page-aligned
 * and span whole pages (pid >= JP_MADVISE_PID). Smaller blocks share
 * pages with other blocks and can't be madvise'd individually.
 * Disabled on Windows (no madvise). */
#ifndef JP_MADVISE_PID
#define JP_MADVISE_PID 12  /* pool 12 = 4KB blocks — first page-aligned pool */
#endif

/* Per-pool madvise counter — only meaningful for pools >= JP_MADVISE_PID.
 * For sub-MADVISE pools, it stays at 0, which is the structural RSS leak:
 * freed small blocks can't have their pages returned to the OS because
 * their neighbors on the same 4K page may still be in use.
 *
 * g_page_madvise_count is the v1.5 patch's per-4K-page madvise counter:
 * incremented every time the user-held-allocation count for a 4K page
 * dec-and-tests to 0 and we fire madvise(MADV_DONTNEED). Tracks the
 * new sub-4K madvise path that the per-pool counter misses. */
#ifdef JP_ALLOC_STATS
static _Atomic(size_t) g_madvise_count[JP_ALLOC_POOL_COUNT];
static _Atomic(size_t) g_madvise_bytes[JP_ALLOC_POOL_COUNT];
static _Atomic(size_t) g_page_madvise_count;  /* v1.5: sub-4K page madvise */
#define JP_STAT_MADVISE(pid, bytes) do { \
	__atomic_add_fetch(&g_madvise_count[pid], 1, __ATOMIC_RELAXED); \
	__atomic_add_fetch(&g_madvise_bytes[pid], (bytes), __ATOMIC_RELAXED); \
} while(0)
#else
#define JP_STAT_MADVISE(pid, bytes) ((void)0)
#endif

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

/* ---- Single-threaded fast path ----
 *
 * glibc 2.32+ provides __libc_single_threaded: a char that starts at 1
 * (single-threaded) and transitions to 0 exactly once when pthread_create
 * is called (never goes back to 1). This lets us skip EBR overhead
 * (epoch stores, thread-list walk, g_epoch CAS) in single-threaded mode.
 *
 * The CAS on the magazine list is always used (even single-threaded) —
 * it's uncontended but atomic, so it's safe if pthread_create fires
 * mid-operation. The EBR skip only affects per-thread state (epoch,
 * active flag) and the pointless ebr_try_advance walk.
 *
 * On older glibc or non-glibc platforms: always multi-threaded path
 * (dead branch, zero cost). */

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 32)
#include <sys/single_threaded.h>
#define JP_HAVE_SINGLE_THREADED 1
#endif
#endif

#ifdef JP_HAVE_SINGLE_THREADED
#define jp_single_threaded() (__atomic_load_n(&__libc_single_threaded, __ATOMIC_ACQUIRE))
#else
#define jp_single_threaded() (0)
#endif

/* ---- Debug header (enabled by -DJP_ALLOC_DEBUG) ---- */
#ifdef JP_ALLOC_DEBUG
#define JP_UNSIZED_MAGIC 0x0BADDEA11DECULL
#define JP_STATE_FREE    0xDEADBEEFFULL
#define JP_STATE_LIVE    0xCAFEBABEULL

#define JP_CHECK(cond, ...) do { \
	if(!(cond)) { fprintf(stderr, "jp_alloc: " __VA_ARGS__); abort(); } \
} while(0)
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

/* ==========================================================================
 * Per-4K-page user-held allocation counter (v1.5 RSS fix)
 * ==========================================================================
 *
 * The structural RSS leak: when pool N is empty and pool_get descends to
 * pool N+1, splits the bigger block, and returns one half to the caller
 * — that buddy cascade commits every 4K page it touches. jp_alloc then
 * FORGETS the buddy relationship on split, so the resulting fragments
 * sit permanently across different size-class free-lists and can never
 * be reconstituted into a 4K to madvise back to the OS. The JP_MADVISE_PID
 * = 12 cutoff means sub-4K blocks (pools 4..11) NEVER get madvise'd,
 * even when the user has freed every live allocation on the page.
 *
 * Fix: maintain a per-4K-page counter of USER-HELD blocks (blocks in a
 * user's hands between jp_alloc returning and jp_free being called).
 * Allocator-internal storage (TLS cache, magazine free-list, pool spares
 * from buddy splits) does NOT count — those are reconstructable from
 * the pool free-list and the user_header_size field written before any
 * block is handed to the user.
 *
 * When the counter dec-and-tests to 0, fire madvise(MADV_DONTNEED) on
 * the 4K page. MADV_DONTNEED zeroes the page (returns it to OS as a
 * zero page), but:
 *   - The counter table lives in a SEPARATE 4K page (immediately after
 *     the 8M region it tracks), so madvise of a user 4K page doesn't
 *     touch the counters.
 *   - Any free-list/TLS-cache/magazine blocks belonging to the madvise'd
 *     4K page will, on their next pop, have their `union header` re-
 *     written by pool_get/jp_alloc before the user sees them. malloc
 *     contract is "uninitialized memory", so a zeroed page is fine.
 *
 * Storage: 4K (= 2048 uint16_t entries) per 8M region = 0.049% overhead.
 * uint16_t covers all sub-4K pool classes — pool 4 (16B from malloc(0))
 * fits 256 blocks per 4K page, far below uint16's 65535 ceiling.
 *
 * 8M-aligned regions: required so any sub-block address can mask down
 * to its region base in O(1). os_alloc_8m_region_counter() mmaps 16M+4K
 * and trims leading+trailing to land an 8M-aligned 8M block followed by
 * an aligned 4K counter-table page.
 *
 * Race safety: atomic fetch_add / fetch_sub on the uint16_t counter.
 *   - Inc on jp_alloc (sub-4K path only).
 *   - Dec-and-test on jp_free (sub-4K path only); on 0, fire madvise.
 *
 * Benign race: T_a decs to 0 and fires madvise; T_b concurrently incs
 * (popped a free block on the same 4K page from a free-list). T_b's
 * block may or may not survive the madvise, but malloc's contract is
 * "uninitialized memory" — T_b's first write either allocates a fresh
 * page (if madvise raced first) or persists (if write raced first). The
 * kernel handles this atomically per page. No corruption.
 */
#ifndef JP_ALLOC_PAGE_COUNTER
#define JP_ALLOC_PAGE_COUNTER 1
#endif

#if JP_ALLOC_PAGE_COUNTER
#define JP2_REGION_SIZE   (8 * 1024 * 1024)   /* pool 23 = 8M block */
#define JP2_TABLE_SIZE    (4 * 1024)          /* 2048 uint16_t counters */
#define JP2_TABLE_ENTRIES (JP2_TABLE_SIZE / sizeof(uint16_t))

/* 8M-aligned region + a trailing 4K counter-table page. mmap 16M+4K
 * span, find 8M-aligned base, munmap trimmings, zero the counter table.
 * Returns the 8M-aligned base usable for pool 23, with the counter table
 * in a separate 4K page at base + 8M (one page past the usable region).
 *
 * On POSIX only — Windows uses VirtualAlloc and the existing unaligned
 * path; counters degrade to "no madvise" there (Windows has no madvise
 * anyway). */
#ifndef _WIN32
static void *os_alloc_8m_region_counter(void)
{
	size_t region_sz = JP2_REGION_SIZE;
	size_t table_sz  = JP2_TABLE_SIZE;
	size_t span = region_sz * 2 + table_sz;  /* 16M + 4K span */
	char *mem = (char *)os_alloc_pages(span);
	if(!mem) return NULL;
	uintptr_t base = (uintptr_t)mem;
	uintptr_t aligned = (base + region_sz - 1) & ~((uintptr_t)region_sz - 1);
	/* Trim leading misalignment: [base, aligned). */
	if(aligned != base) {
		os_free_pages(mem, (size_t)(aligned - base));
	}
	/* Keep [aligned, aligned + region_sz + table_sz); trim trailing. */
	uintptr_t kept_end = aligned + region_sz + table_sz;
	uintptr_t span_end = base + span;
	if(kept_end < span_end) {
		os_free_pages((char *)kept_end, (size_t)(span_end - kept_end));
	}
	/* Zero the counter table so initial counts are 0 (the page may have
	 * been mapped fresh from the OS but we don't assume it's zeroed on
	 * all platforms). */
	uint16_t *table = (uint16_t *)(aligned + region_sz);
	memset(table, 0, table_sz);
	return (void *)aligned;
}
#else
/* Windows: no madvise, fall back to plain unaligned 8M mmap. */
static void *os_alloc_8m_region_counter(void)
{
	return os_alloc_pages(JP2_REGION_SIZE);
}
#endif

/* Given any block pointer within a sub-4K pool, find its 4K page's
 * user-held-allocation counter. Returns NULL if the block's address
 * can't be attributed to a counter'd region (i.e., if it came from the
 * legacy unaligned pool-23 mmap path). In practice every pool-23 block
 * allocated after JP_ALLOC_PAGE_COUNTER was enabled comes from
 * os_alloc_8m_region_counter() and is 8M-aligned, so this mask always
 * finds the base; legacy blocks (in long-running processes that pre-
 * date the patch) would misattribute — but those are extremely rare
 * and the worst case is "missed madvise", not corruption. */
static inline uint16_t *jp_page_counter_of(void *block_addr)
{
	uintptr_t a = (uintptr_t)block_addr;
	uintptr_t base = a & ~((uintptr_t)(JP2_REGION_SIZE - 1));
	uint16_t *table = (uint16_t *)(base + JP2_REGION_SIZE);
	return table + ((a - base) >> 12);
}

/* Hot-path helpers used by jp_alloc and jp_free. Called only for sub-
 * 4K blocks (pid < JP_MADVISE_PID = 12). The madvise fired on dec-to-0
 * is amortized across all blocks on the 4K page that have been alloc'd
 * and freed since the last time the page was fully idle. */
static inline void jp_page_counter_inc(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	__atomic_add_fetch(cnt, 1, __ATOMIC_RELAXED);
}

/* Returns 1 if the counter reached 0 (caller should consider madvise),
 * 0 otherwise. */
static inline int jp_page_counter_dec_and_test(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	return __atomic_sub_fetch(cnt, 1, __ATOMIC_RELAXED) == 0;
}

/* Re-check a counter that was 0 when scheduled for madvise. If a
 * concurrent alloc came in between scheduling and now, the counter
 * is > 0 — caller should skip the madvise (page is hot again).
 * This is the cheap-by-default part of the v1.5 churn-amortization:
 * dec-to-0 schedules a madvise, but we defer the actual syscall to
 * a per-thread batch flush (see jp_page_pending_flush() call in
 * jp_free), at which point we re-check whether the page is still
 * empty. For alloc-heavy/free-heavy churn on a hot page (typical GCC
 * AST build), the page is almost always hot by the time we'd fire
 * madvise, so the syscall cost is skipped — close to zero overhead.
 *
 * For SQLite's load-then-DROP pattern: most pages stay drained after
 * DROP so the re-check confirms 0 and madvise fires, recovering
 * the ~1.6 GB observed in the bench. */
static inline int jp_page_counter_is_still_zero(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	return __atomic_load_n(cnt, __ATOMIC_RELAXED) == 0;
}

/* Forward decls: jp_page_pending_add/flush are defined after the
 * tls_state struct (they need JP_PC_PENDING_CAP and tls_state.pc_pending).
 * tls_destructor is the pthread_key destructor that drains the final
 * pending batch at thread teardown. */
static void jp_page_pending_flush(void);
static void jp_page_pending_add(void *block_addr);

/* Counter active only when both compiled in AND the block is in a
 * sub-4K pool. Callers gate on the pid check; this avoids the address
 * math on every alloc/free of large blocks.
 *
 * JP_PC_DEC: returns nonzero via dec_hit_zero if the counter reached 0
 * (caller schedules a deferred madvise via jp_page_pending_add). */
#define JP_PC_INC(mem, pid)   do { if((pid) < JP_MADVISE_PID) jp_page_counter_inc(mem); } while(0)
#define JP_PC_DEC(h, pid, dec_hit_zero) do { \
	*(dec_hit_zero) = ((pid) < JP_MADVISE_PID) && jp_page_counter_dec_and_test(h); \
} while(0)
#else
/* Counter disabled — degrade to the pre-patch behavior. */
#define JP_PC_INC(mem, pid)   ((void)0)
#define JP_PC_DEC(h, pid, dec_hit_zero) (*(dec_hit_zero) = 0)
#endif

/* ==========================================================================
 * 64-bit CAS magazine freelist + EBR + thread-local cache
 * ==========================================================================
 *
 * The global freelist for each pool class is a linked list of magazines.
 * Each magazine holds JP_MAG_SIZE block pointers in an array. Refill pops
 * one magazine and memcpys its pointers into the TLS cache. Flush memcpys
 * pointers from the TLS cache into a magazine and CAS-pushes it to the
 * global list. No dependent-load walks, no chain-building loops.
 *
 * Magazines come from a static global array — no mmap, no malloc, no mutex.
 * A CAS-based free-list hands out magazines from the array. Magazines are
 * never freed; they cycle between the TLS empty stack and the global
 * magazine lists forever.
 *
 * ABA is prevented by a 3-epoch ring EBR. Thread teardown deposits
 * magazines into a global limbo drained lazily by live threads.
 */

/* ---- Atomic primitives ---- */
static inline void *atomic_load_ptr(void * volatile *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_ptr(void * volatile *p, void *v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline int atomic_cas_ptr(void * volatile *p, void **old, void *desired)
{
	return __atomic_compare_exchange_n(p, old, desired,
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

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

/* ---- Magazine: batch container for the global freelist ----
 *
 * JP_MAG_SIZE must be <= JP_CACHE_N. A flush copies JP_MAG_SIZE blocks
 * from the bottom of the cache into a magazine, keeping the rest. The
 * cache oscillates between (JP_CACHE_N - JP_MAG_SIZE + 1) and JP_CACHE_N. */

#ifndef JP_CACHE_N
#define JP_CACHE_N 32
#endif

#ifndef JP_MAG_SIZE
#define JP_MAG_SIZE 16
#endif
#if JP_MAG_SIZE > JP_CACHE_N
#error "JP_MAG_SIZE must not exceed JP_CACHE_N"
#endif

struct magazine {
	struct magazine *next;   /* global list link / TLS empty stack link */
	void *slots[JP_MAG_SIZE];
};

/* ---- Static magazine pool ----
 *
 * A CAS-based free-list hands out magazines. When the free-list is empty,
 * a magazine is allocated from the pool system (pool 8 = 256B block, which
 * fits the ~136-byte magazine struct + 16-byte header). Magazines are never
 * freed to the OS — they cycle between the TLS empty stack, the global
 * magazine lists, and the free-list forever. 
 * Magazines come from a fixed pool count (pool 8 = 256B), which is backed
 * by the 8MB demand-paged reserve. The first magazine allocation triggers
 * one buddy-split from pool 23, populating ~16K magazines from one mmap.
 * Only the touched pages count toward RSS (~28 pages = 112 KB). */
#define JP_MAG_PID 8  /* pool 8 = 256B block, fits struct magazine + header */

static struct magazine * volatile g_mag_free = NULL;

static void mag_free(struct magazine *m)
{
	/* Return to the CAS-based free-list for reuse by other threads. */
	struct magazine *old = atomic_load_ptr((void * volatile *)&g_mag_free);
	do {
		m->next = old;
	} while(!atomic_cas_ptr((void * volatile *)&g_mag_free, (void **)&old, m));
}

/* ---- Pool types ---- */

struct pool {
	struct magazine * volatile head;  /* magazine list head */
	char _pad[JP_CACHELINE - sizeof(struct magazine *)];
};

static _Alignas(JP_CACHELINE) struct pool g_pools[JP_ALLOC_POOL_COUNT];
static struct pool *g_pools_last = g_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Statistics: per-pool live/free counts + magazine/mmap bytes.
 * Enabled with -DJP_ALLOC_STATS. Useful for diagnosing real-world RSS
 * blowup (was it internal fragmentation? magazine free lists? mmap
 * metadata? confined to one size class?). The dump is emitted at exit
 * via atexit-registered jp_alloc_stats_dump(). */
#ifdef JP_ALLOC_STATS
#include <stdio.h>
static _Atomic(size_t) g_live_blocks[JP_ALLOC_POOL_COUNT];  /* alloc'd not freed */
static _Atomic(size_t) g_alloc_count[JP_ALLOC_POOL_COUNT]; /* total allocs */
static _Atomic(size_t) g_free_count[JP_ALLOC_POOL_COUNT];  /* total frees */
static _Atomic(size_t) g_mmap_count[JP_ALLOC_POOL_COUNT];  /* mmap for this pool */
static _Atomic(size_t) g_mmap_bytes[JP_ALLOC_POOL_COUNT];  /* bytes mmap'd for this pool */
static _Atomic(size_t) g_direct_mmap_bytes;                /* large (> largest pool) */
static _Atomic(size_t) g_direct_live_bytes;                /* large-block bytes live */
static _Atomic(size_t) g_mag_alloc_count;  /* total mag_alloc() calls */
#define JP_STAT_LIVE(pid)   __atomic_add_fetch(&g_live_blocks[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_DEAD(pid)   __atomic_sub_fetch(&g_live_blocks[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_ALLOC(pid)  __atomic_add_fetch(&g_alloc_count[pid], 1, __ATOMIC_RELAXED)
#define JP_STAT_FREE(pid)   __atomic_add_fetch(&g_free_count[pid],  1, __ATOMIC_RELAXED)
#define JP_STAT_MMAP(pid, bytes) do { \
	__atomic_add_fetch(&g_mmap_count[pid], 1, __ATOMIC_RELAXED); \
	__atomic_add_fetch(&g_mmap_bytes[pid], (bytes), __ATOMIC_RELAXED); \
} while(0)
#define JP_STAT_MMAP_DIRECT(bytes) \
	__atomic_add_fetch(&g_direct_mmap_bytes, (bytes), __ATOMIC_RELAXED)
#define JP_STAT_DIRECT_LIVE(bytes) \
	__atomic_add_fetch(&g_direct_live_bytes, (bytes), __ATOMIC_RELAXED)
#define JP_STAT_DIRECT_DEAD(bytes) \
	__atomic_sub_fetch(&g_direct_live_bytes, (bytes), __ATOMIC_RELAXED)
#define JP_STAT_MAG_ALLOC() __atomic_add_fetch(&g_mag_alloc_count, 1, __ATOMIC_RELAXED)
static void jp_alloc_stats_dump(void);
static void jp_alloc_stats_register_atexit(void) __attribute__((constructor));
static void jp_alloc_stats_register_atexit(void) { atexit(jp_alloc_stats_dump); }
#else
#define JP_STAT_LIVE(pid)   ((void)0)
#define JP_STAT_DEAD(pid)   ((void)0)
#define JP_STAT_ALLOC(pid)  ((void)0)
#define JP_STAT_FREE(pid)   ((void)0)
#define JP_STAT_MMAP(pid, bytes) ((void)0)
#define JP_STAT_MMAP_DIRECT(bytes) ((void)0)
#define JP_STAT_DIRECT_LIVE(bytes) ((void)0)
#define JP_STAT_DIRECT_DEAD(bytes) ((void)0)
#define JP_STAT_MAG_ALLOC() ((void)0)
#endif

static void *pool_get(struct pool *p, size_t pid); /* forward decl */
static struct magazine *mag_alloc(void)
{
	/* Try the free-list first (recycled magazines). */
	struct magazine *m = atomic_load_ptr((void * volatile *)&g_mag_free);
	for(;;) {
		if(!m) break;
		struct magazine *next = m->next;
		if(atomic_cas_ptr((void * volatile *)&g_mag_free, (void **)&m, next))
			return m;
	}
/* Free-list empty — allocate from the pool system.
		 * The pool block includes a 16-byte union header; the magazine
		 * struct is placed after the header (like any malloc'd block). */
		union header *h = (union header *)pool_get(g_pools + JP_MAG_PID, JP_MAG_PID);
		if(!h) return NULL;
		h->s.size = JP_MAG_PID;
		JP_STAT_MAG_ALLOC();
		return (struct magazine *)(h + 1);
	}



/* ---- Epoch-Based Reclamation (3-epoch ring) ---- */

#define JP_EBR_EPOCHS 3
#define JP_EBR_INACTIVE (-1L)

struct ebr_thread {
	_Atomic(int)    active;
	_Atomic(long)   epoch;
	_Atomic(struct ebr_thread *) next;
};

static _Atomic(long) g_epoch = 0;
static _Atomic(struct ebr_thread *) g_thread_list = NULL;

/* ---- Per-thread state ---- */

struct retired_chain {
	struct magazine *head;
	struct magazine *tail;
};

/* Per-thread pending-madvise batch: list of 4K page addresses that hit
 * dec-to-0 since the last flush. Bounded by JP_PC_PENDING_CAP; when the
 * list fills (or the thread exits), jp_page_pending_flush() runs and
 * for each address re-checks jet_page_counter_is_still_zero() — if
 * the page has been re-allocated since, skip madvise (it's hot). This
 * amortizes madvise syscalls across many dec-to-0 events and avoids
 * the ~35s slowdown observed on GCC compile when madvise was fired
 * per-free (3.5M madvise calls × ~1µs each = pathological). */
#define JP_PC_PENDING_CAP 64
struct jp_pc_pending {
	void *pages[JP_PC_PENDING_CAP];
	size_t cnt;
};

struct tls_state {
	struct ebr_thread *self;
	/* cache */
	void *cache[JP_ALLOC_POOL_COUNT][JP_CACHE_N];
	size_t cnt[JP_ALLOC_POOL_COUNT];
	/* empty magazines: one unified stack per thread, shared across all
	 * pools. Grows during refills, shrinks during flushes. */
	struct magazine *mag_empty;
	/* retired batches: per-pool, per-epoch-slot */
	struct retired_chain retired[JP_ALLOC_POOL_COUNT][JP_EBR_EPOCHS];
	int in_pop_cs;
	/* per-thread pending page-madvise batch (v1.5) */
	struct jp_pc_pending pc_pending;
};

static _Thread_local struct tls_state tls;
static _Thread_local int tls_registered = 0;

/* Drain this thread's pending-madvise batch: for each 4K page scheduled
 * via jp_page_pending_add(), re-check jet_page_counter_is_still_zero().
 * If the counter is still 0 (no thread has re-allocated on this page
 * since scheduling), fire madvise(MADV_DONTNEED). Otherwise skip — the
 * page is hot and madvise'ing it would just cost us page-fault syscalls
 * on the next write.
 *
 * Re-check semantics are intentionally racy: between the load we do here
 * and the madvise syscall, another thread may bump the counter back to
 * >0. That's a benign miss-of-a-madvise opportunity (one extra churn
 * until the next dec-to-0) — correctness is preserved because the kernel
 * handles madvise-then-write races atomically (write wins → page stays
 * dirty; madvise wins → page becomes zero-on-next-write-fault). */
static void jp_page_pending_flush(void)
{
#if JP_ALLOC_PAGE_COUNTER
#ifndef _WIN32
	for(size_t i = 0; i < tls.pc_pending.cnt; i++) {
		void *page = tls.pc_pending.pages[i];
		if(jp_page_counter_is_still_zero(page)) {
			madvise(page, 4096, MADV_DONTNEED);
#ifdef JP_ALLOC_STATS
			__atomic_add_fetch(&g_page_madvise_count, 1, __ATOMIC_RELAXED);
#endif
		}
	}
#endif
	tls.pc_pending.cnt = 0;
#endif /* JP_ALLOC_PAGE_COUNTER */
}

/* Add a 4K page (by block addr) to this thread's pending-madvise batch.
 * If the batch fills, immediately drain it. Called from jp_free's
 * sub-4K path when dec-and-test hits 0. Pages already in the batch
 * (rare race on the same page) are silently dedup'd — worst case is
 * we madvise twice for the same page, which is harmless. */
static void jp_page_pending_add(void *block_addr)
{
#if JP_ALLOC_PAGE_COUNTER
	if(tls.pc_pending.cnt >= JP_PC_PENDING_CAP) {
		jp_page_pending_flush();
	}
	/* Dedup: most dec-to-0 events are on distinct pages; the linear
	 * scan is bounded by JP_PC_PENDING_CAP (=64) and skipped on the
	 * common cold-cache first fill. */
	void *page = (void *)((uintptr_t)block_addr & ~((uintptr_t)4096 - 1));
	for(size_t i = 0; i < tls.pc_pending.cnt; i++) {
		if(tls.pc_pending.pages[i] == page)
			return;  /* already pending */
	}
	tls.pc_pending.pages[tls.pc_pending.cnt++] = page;
#else
	(void)block_addr;
#endif /* JP_ALLOC_PAGE_COUNTER */
}

/* ---- pthread TLS init ---- */
#include <pthread.h>

static pthread_key_t  g_tls_key;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_destructor(void *p);
static void ebr_try_advance(void);

static void tls_init_once(void)
{
	pthread_key_create(&g_tls_key, tls_destructor);
}

static void tls_register(void)
{
	if(tls_registered) return;
	pthread_once(&g_tls_once, tls_init_once);
	if(!tls.self) {
		struct ebr_thread *r = (struct ebr_thread *)os_alloc_pages(os_page_size());
		if(!r) {
			tls.self = NULL;
		} else {
			__atomic_store_n(&r->active, 0, __ATOMIC_RELAXED);
			__atomic_store_n(&r->epoch, JP_EBR_INACTIVE, __ATOMIC_RELAXED);
			struct ebr_thread *old = __atomic_load_n(&g_thread_list, __ATOMIC_ACQUIRE);
			do {
				__atomic_store_n(&r->next, old, __ATOMIC_RELAXED);
			} while(!__atomic_compare_exchange_n(&g_thread_list, &old, r,
				0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
			tls.self = r;
		}
	}
	pthread_setspecific(g_tls_key, (void *)(uintptr_t)1);
	tls_registered = 1;
}

/* Pre-declarations */
static void global_push_magazine(struct pool *p, struct magazine *head, struct magazine *tail);
static void magazine_madvise(struct magazine *head, size_t pid);

/* ---- Limbo (for thread teardown) ---- */

struct limbo_slot {
	struct magazine * volatile head;
	char _pad[JP_CACHELINE - sizeof(struct magazine *)];
};
static _Alignas(JP_CACHELINE) struct limbo_slot g_limbo[JP_ALLOC_POOL_COUNT][JP_EBR_EPOCHS];

static void deposit_to_limbo(size_t pid, int slot, struct magazine *head, struct magazine *tail)
{
	struct limbo_slot *s = &g_limbo[pid][slot];
	struct magazine *old = atomic_load_ptr((void * volatile *)&s->head);
	for(;;) {
		tail->next = old;
		if(atomic_cas_ptr((void * volatile *)&s->head, (void **)&old, head)) break;
	}
}

static void drain_limbo(size_t pid, int slot)
{
	struct limbo_slot *s = &g_limbo[pid][slot];
	struct magazine *head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
	if(!head) return;
	if(!__atomic_compare_exchange_n(&s->head, &head, NULL,
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	struct magazine *tail = head;
	while(tail->next != NULL) tail = tail->next;
	magazine_madvise(head, pid);
	global_push_magazine(&g_pools[pid], head, tail);
}

/* ---- TLS destructor ---- */
static void tls_destructor(void *p)
{
	(void)p;
	if(!tls_registered) return;
	struct tls_state *t = &tls;

	/* Drain any pending page-madvise batch before thread tear-down.
	 * Pages still at counter=0 get madvise'd here; pages that another
	 * thread has re-touched since scheduling are skipped. This ensures
	 * we return RSS to the OS even for short-lived threads whose batch
	 * never filled during normal operation. */
	jp_page_pending_flush();

	/* Deposit partial cache contents into magazines and push to limbo. */
	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		size_t cnt = t->cnt[pid];
		if(cnt > 0) {
			struct magazine *mag = t->mag_empty;
			if(mag) {
				t->mag_empty = mag->next;
				memcpy(mag->slots, t->cache[pid], cnt * sizeof(void *));
				if(cnt < JP_MAG_SIZE)
					memset(&mag->slots[cnt], 0, (JP_MAG_SIZE - cnt) * sizeof(void *));
				mag->next = NULL;
				deposit_to_limbo(pid, 0, mag, mag);
			}
			t->cnt[pid] = 0;
		}
	}

	/* Return empty magazines from TLS to the global free-list. */
	while(t->mag_empty) {
		struct magazine *next = t->mag_empty->next;
		mag_free(t->mag_empty);
		t->mag_empty = next;
	}

	/* Deposit all per-epoch retired magazines into the global limbo. */
	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		for(int slot = 0; slot < JP_EBR_EPOCHS; slot++) {
			if(t->retired[pid][slot].head) {
				deposit_to_limbo(pid, slot,
					t->retired[pid][slot].head,
					t->retired[pid][slot].tail);
				t->retired[pid][slot].head = t->retired[pid][slot].tail = NULL;
			}
		}
	}

	if(t->self) {
		__atomic_store_n(&t->self->active, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&t->self->epoch, JP_EBR_INACTIVE, __ATOMIC_RELEASE);
	}
	tls_registered = 0;
}

/* ---- EBR critical section ---- */

static inline void ebr_enter(void)
{
	tls_register();
	if(tls.self) {
		long e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
		__atomic_store_n(&tls.self->epoch, e, __ATOMIC_RELEASE);
		__atomic_store_n(&tls.self->active, 1, __ATOMIC_RELEASE);
	}
}

static inline void ebr_exit(void)
{
	if(tls.self) {
		__atomic_store_n(&tls.self->active, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&tls.self->epoch, JP_EBR_INACTIVE, __ATOMIC_RELEASE);
	}
	ebr_try_advance();
}

static void ebr_try_advance(void)
{
	long old = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
	struct ebr_thread *r = __atomic_load_n(&g_thread_list, __ATOMIC_ACQUIRE);
	while(r) {
		int active = __atomic_load_n(&r->active, __ATOMIC_ACQUIRE);
		if(active) {
			long e = __atomic_load_n(&r->epoch, __ATOMIC_ACQUIRE);
			if(e < old) return;
		}
		r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE);
	}
	if(!__atomic_compare_exchange_n(&g_epoch, &old, old + 1,
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;

	int drain_slot = (int)(((old - 1) + JP_EBR_EPOCHS) % JP_EBR_EPOCHS);

	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		if(tls.retired[pid][drain_slot].head) {
			struct magazine *h = tls.retired[pid][drain_slot].head;
			struct magazine *t = tls.retired[pid][drain_slot].tail;
			tls.retired[pid][drain_slot].head = tls.retired[pid][drain_slot].tail = NULL;
			magazine_madvise(h, pid);
			global_push_magazine(&g_pools[pid], h, t);
		}
		drain_limbo(pid, drain_slot);
	}
}

/* ---- Pool operations (magazine-based) ---- */

/* Return large-block pages to the OS when magazines are drained to the
 * global list. Only for pools where blocks span whole pages (pid >=
 * JP_MADVISE_PID). The block's s.size header is zeroed by madvise —
 * jp_alloc re-writes it unconditionally after pool_get, so this is safe.
 * Called at EBR drain time (not per-free), so madvise syscall cost is
 * amortized across ~16 blocks per drain. */
#ifndef _WIN32
static void magazine_madvise(struct magazine *head, size_t pid)
{
	if(pid < JP_MADVISE_PID) return;
	size_t sz = 1U << pid;
	size_t cnt = 0;
	for(struct magazine *m = head; m; m = m->next) {
		for(size_t i = 0; i < JP_MAG_SIZE; i++) {
			if(m->slots[i]) {
				madvise(m->slots[i], sz, MADV_DONTNEED);
				cnt++;
			}
		}
	}
	if(cnt) {
		size_t bytes = cnt * sz;
		JP_STAT_MADVISE(pid, bytes);
	}
}
#else
#define magazine_madvise(head, pid) ((void)0)
#endif

static void global_push_magazine(struct pool *p, struct magazine *head, struct magazine *tail)
{
	struct magazine *old = atomic_load_ptr((void * volatile *)&p->head);
	do {
		tail->next = old;
	} while(!atomic_cas_ptr((void * volatile *)&p->head, (void **)&old, head));
}

static struct magazine *global_pop_magazine(struct pool *p)
{
	struct magazine *head = atomic_load_ptr((void * volatile *)&p->head);
	for(;;) {
		if(!head) return NULL;
		struct magazine *next = head->next;
		if(atomic_cas_ptr((void * volatile *)&p->head, (void **)&head, next))
			return head;
	}
}

static size_t pool_id(size_t size)
{
	if(likely(size > 0)) {
		return 64 - jp_clzll(size - 1);
	}
	return 0;
}

static int is_pow2(size_t n)
{
	return (n & (n - 1)) == 0;
}

static void pool_put(union header *h, struct pool *p, size_t pid)
{
	(void)p;
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_FREE;
#endif
	/* Hot path: push to TLS cache. */
	if(likely(tls.cnt[pid] < JP_CACHE_N)) {
		tls.cache[pid][tls.cnt[pid]++] = h;
		return;
	}
	/* Cache full: flush JP_MAG_SIZE blocks into a magazine and retire it. */
	struct magazine *mag = tls.mag_empty;
	if(mag) {
		tls.mag_empty = mag->next;
	} else {
		mag = mag_alloc();
		if(!mag) {
			/* Magazine pool exhausted — keep the block in the cache
			 * by dropping the oldest entry. Extremely rare. */
			tls.cache[pid][0] = h;
			return;
		}
	}
	memcpy(mag->slots, tls.cache[pid], JP_MAG_SIZE * sizeof(void *));
	/* Shift remaining blocks down. */
	tls.cnt[pid] -= JP_MAG_SIZE;
	memmove(&tls.cache[pid][0], &tls.cache[pid][JP_MAG_SIZE],
		tls.cnt[pid] * sizeof(void *));
	/* Put the new block into the cache. */
	tls.cache[pid][tls.cnt[pid]++] = h;
	/* Retire the magazine. */
	mag->next = NULL;
	long e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
	int slot = (int)(e % JP_EBR_EPOCHS);
	struct retired_chain *rc = &tls.retired[pid][slot];
	if(rc->head == NULL) {
		rc->head = mag;
		rc->tail = mag;
	} else {
		rc->tail->next = mag;
		rc->tail = mag;
	}
	if(!jp_single_threaded())
		ebr_try_advance();
}

static void *pool_get(struct pool *p, size_t pid)
{
	/* Hot path: pop from TLS cache. */
	if(likely(tls.cnt[pid] > 0)) {
		JP_COUNT_HIT;
		return (union header *)tls.cache[pid][--tls.cnt[pid]];
	}
	/* Cache miss: try to pop a full magazine from the global list. */
	JP_COUNT_MISS;
	int single = jp_single_threaded();
	int outer = !tls.in_pop_cs;
	if(outer && !single) {
		ebr_enter();
		tls.in_pop_cs = 1;
	}
	union header *result = NULL;
	{
		struct magazine *mag = global_pop_magazine(p);
		if(mag) {
			/* Refill: copy block pointers from magazine to cache,
			 * skipping NULL slots (partial magazines from teardown). */
			size_t n = 0;
			for(size_t i = 0; i < JP_MAG_SIZE; i++) {
				if(mag->slots[i] != NULL)
					tls.cache[pid][n++] = mag->slots[i];
			}
			tls.cnt[pid] = n;
			/* Keep the empty magazine for the next flush. */
			mag->next = tls.mag_empty;
			tls.mag_empty = mag;
			if(n > 0)
				result = (union header *)tls.cache[pid][--tls.cnt[pid]];
		}
	}
	if(result == NULL) {
		if(unlikely(p == g_pools_last)) {
			/* Largest pool: mmap one block. Size is set by jp_alloc.
			 *
			 * Use the 8M-aligned counter region allocator when JP_ALLOC_PAGE_COUNTER
			 * is enabled (default on). It returns an 8M-aligned 8M block with a 4K
			 * counter-table page sitting one page past the end. The table tracks
			 * per-4K-page user-held counts so the sub-4K madvise gap (which the
			 * stats bench showed as ~3.3 GB RSS for SQLite) can be closed.
			 *
			 * Pool 23 nominal size is 8M (1<<23); the cascade never slices into
			 * the trailing 4K counter-table page because the cascade bottom is
			 * pool 4 (16B from malloc(0)), all of which land within the 8M usable
			 * region. The 4K counter page is reserved as a hard boundary. */
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
#if JP_ALLOC_PAGE_COUNTER
			result = (union header *)os_alloc_8m_region_counter();
#else
			result = (union header *)os_alloc_pages(sz);
#endif
			if(likely(result != NULL))
				JP_STAT_MMAP(pid, sz);
#ifdef JP_ALLOC_DEBUG
			if(likely(result != NULL)) {
				result->s.magic = JP_UNSIZED_MAGIC;
				result->s.state = JP_STATE_FREE;
			}
#endif
		} else {
			/* Binary buddy: split from next-larger pool.
			 * Use pid directly (not result->s.size - 1) since the
			 * header may have been zeroed by madvise. Size is set
			 * by jp_alloc. */
			char *mem = (char *)pool_get(g_pools + pid + 1, pid + 1);
			if(mem != NULL) {
				result = (union header *)mem;
				size_t sz = pid;
				union header *spare = (union header *)(mem + (1U << sz));
#ifdef JP_ALLOC_DEBUG
				spare->s.magic = JP_UNSIZED_MAGIC;
				spare->s.state = JP_STATE_FREE;
#endif
				if(likely(tls.cnt[pid] < JP_CACHE_N)) {
					tls.cache[pid][tls.cnt[pid]++] = spare;
				} else {
					pool_put(spare, g_pools + pid, pid);
				}
			}
		}
	}
	if(outer && !single) {
		tls.in_pop_cs = 0;
		ebr_exit();
	}
#ifdef JP_ALLOC_DEBUG
	if(result != NULL) {
		/* Two valid magic values here:
		 *   JP_UNSIZED_MAGIC — block was freed (pool_put wrote it before
		 *     pushing to TLS/magazine) and has been sitting untouched.
		 *   0 — block was on a 4K page that the v1.5 per-page counter
		 *     path madvise(MADV_DONTNEED)'d. madvise zeroes the page;
		 *     the block's header now reads 0. jp_alloc will rewrite
		 *     s.size = pid and (in DEBUG) magic = JP_UNSIZED_MAGIC /
		 *     state = JP_STATE_LIVE before returning to the user, so
		 *     the zeroed state is consistent with malloc's "uninit
		 *     memory" contract — the user shouldn't read pre-write.
		 *
		 * Any other value means real corruption (double-free previously
		 * trashed the header, or a runaway pointer overwrote it). */
		JP_CHECK(result->s.magic == JP_UNSIZED_MAGIC || result->s.magic == 0,
			 "pool_get: wrong magic %llx (expected %llx or 0=madvise'd)\n",
			 (unsigned long long)result->s.magic,
			 (unsigned long long)JP_UNSIZED_MAGIC);
		/* State check is asymmetric: a freed block has state=JP_STATE_FREE.
		 * A madvise'd block has state=0 (zeroed along with magic). Either
		 * is a valid "block is not currently live" indicator. A state
		 * of JP_STATE_LIVE here means the block was freed, popped,
		 * madvise'd in between, AND another thread has already alloc'd
		 * and is using it — that would be a real ABA bug. */
		JP_CHECK(result->s.state == JP_STATE_FREE || result->s.state == 0,
			 "pool_get: ABA! block %p still LIVE (state=%llx)\n",
			 (void *)result,
			 (unsigned long long)result->s.state);
	}
#endif
	return result;
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
	char *span = os_alloc_pages(span_size_rounded);
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

size_t jp_good_size(size_t size);
size_t jp_good_size(size_t size)
{
	size_t pid = pool_id(size);
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		size = 1U << pid;
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
	}
	return size - sizeof(union header);
}

void jp_alloc_reset(void)
{
	/* No-op: pool memory is still reachable from the global pool head
	 * pointers, so valgrind reports it as "still reachable" (not an
	 * error). The OS reclaims all pages at process exit. */
}

#ifdef JP_ALLOC_STATS
/* Per-pool magazine free-list walk: count blocks sitting in the global
 * magazine free-list (returned by other threads but not reused).
 * Doesn't pop anything — just counts. */
static size_t count_magazine_blocks(struct pool *p)
{
	size_t total = 0;
	struct magazine *m = atomic_load_ptr((void * volatile *)&p->head);
	/* Walk is racy but at-exit time other threads are gone; counts are
	 * approximate upper bounds. Stop at a sane max to avoid spinning
	 * on a corrupted list. */
	size_t guard = 0;
	while(m && guard < 1000000) {
		for(size_t i = 0; i < JP_MAG_SIZE; i++) {
			if(m->slots[i] != NULL) total++;
		}
		m = m->next;
		guard++;
	}
	return total;
}

static void jp_alloc_stats_dump(void)
{
	FILE *f = stderr;
	size_t total_live_blocks = 0, total_allocs = 0, total_frees = 0;
	size_t total_mmap_bytes = 0, total_mag_blocks = 0, total_madvise = 0;
	size_t total_tls_cache_blocks = 0;

	/* TLS cache: anything still in the calling thread's cache is also
	 * "free but retained" (= not live, not returned to global). */
	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		total_tls_cache_blocks += tls.cnt[pid];
	}

fprintf(f, "\n=== jp_alloc STATS ===\n");
fprintf(f, "  %-4s %-10s %-12s %-12s %-12s %-14s %-14s %-14s %-14s %-14s\n",
	"pid", "block_sz", "live_blocks", "allocs", "frees",
	"mmap_count", "mmap_bytes", "mag_free_blk", "madvise_cnt", "total_live_B");
for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
	size_t block_sz = 1U << pid;
	size_t live = __atomic_load_n(&g_live_blocks[pid], __ATOMIC_RELAXED);
	size_t alloc = __atomic_load_n(&g_alloc_count[pid], __ATOMIC_RELAXED);
	size_t free = __atomic_load_n(&g_free_count[pid], __ATOMIC_RELAXED);
	size_t mmap_count = __atomic_load_n(&g_mmap_count[pid], __ATOMIC_RELAXED);
	size_t mmap_bytes = __atomic_load_n(&g_mmap_bytes[pid], __ATOMIC_RELAXED);
	size_t mag_free = count_magazine_blocks(&g_pools[pid]);
	size_t madv_count = __atomic_load_n(&g_madvise_count[pid], __ATOMIC_RELAXED);
	size_t live_bytes = live * block_sz;
	if(live == 0 && alloc == 0 && mmap_count == 0 && mag_free == 0 &&
	   madv_count == 0)
		continue;  /* skip dead size classes for brevity */
	fprintf(f, "  %-4zu %-10zu %-12zu %-12zu %-12zu %-14zu %-14zu %-14zu %-14zu %-14zu\n",
		pid, block_sz, live, alloc, free,
		mmap_count, mmap_bytes, mag_free, madv_count, live_bytes);
	total_live_blocks += live;
	total_allocs += alloc;
	total_frees += free;
	total_mmap_bytes += mmap_bytes;
	total_mag_blocks += mag_free;
	total_madvise += madv_count;
}
size_t direct = __atomic_load_n(&g_direct_mmap_bytes, __ATOMIC_RELAXED);
size_t direct_live = __atomic_load_n(&g_direct_live_bytes, __ATOMIC_RELAXED);
size_t mag_total = __atomic_load_n(&g_mag_alloc_count, __ATOMIC_RELAXED);
fprintf(f, "  ---\n");
fprintf(f, "  total live blocks  : %zu\n", total_live_blocks);
fprintf(f, "  total allocs/frees : %zu / %zu (delta %zu)\n",
	total_allocs, total_frees, total_allocs - total_frees);
fprintf(f, "  total mmap (pools) : %zu bytes (%.1f MB)\n",
	total_mmap_bytes, total_mmap_bytes / 1048576.0);
fprintf(f, "  direct mmap (large): %zu bytes (%.1f MB), still live: %zu bytes (%.1f MB)\n",
	direct, direct / 1048576.0, direct_live, direct_live / 1048576.0);
fprintf(f, "  magazine total     : %zu ever allocated\n", mag_total);
fprintf(f, "  magazine free-list  : %zu blocks queued globally\n", total_mag_blocks);
fprintf(f, "  madvise total      : %zu calls returned pages to OS\n", total_madvise);
{
	size_t page_madv = __atomic_load_n(&g_page_madvise_count, __ATOMIC_RELAXED);
	fprintf(f, "  page madvise (v1.5) : %zu calls (per-4K-page user-held dec-to-0)\n", page_madv);
}
fprintf(f, "  TLS cache (now)    : %zu blocks\n", total_tls_cache_blocks);
fprintf(f, "=== end STATS ===\n\n");
fflush(f);
}
#endif

#ifdef JP_ALLOC_DEBUG
/* Diagnostic counters for the bench. Not part of the public API. */
void jp_alloc_diag(size_t *hits, size_t *misses)
{
	if(hits)   *hits   = __atomic_load_n(&g_cache_hits,   __ATOMIC_RELAXED);
	if(misses) *misses = __atomic_load_n(&g_cache_misses, __ATOMIC_RELAXED);
}
#endif

/* ---- malloc/free implementation ---- */

void jp_free(void *mem)
{
	if(unlikely(mem == NULL)) return;

	union header *h = (union header *)mem - 1;
#ifdef JP_ALLOC_DEBUG
	JP_CHECK(h->s.magic == JP_UNSIZED_MAGIC,
		 "jp_free: double free or corruption on %p (magic=%llx)\n",
		 mem, (unsigned long long)h->s.magic);
	JP_CHECK(h->s.state == JP_STATE_LIVE,
		 "jp_free: double free on %p (state=%llx)\n",
		 mem, (unsigned long long)h->s.state);
	h->s.state = JP_STATE_FREE;
#endif
	size_t size = h->s.size;
	if(likely(size < JP_ALLOC_POOL_COUNT)) {
		JP_STAT_FREE(size);
		JP_STAT_DEAD(size);
		/* Per-4K-page user-held counter: dec-and-test BEFORE pool_put
		 * returns the block to the allocator-internal free-list. If the
		 * count hits 0 there are no user-held blocks left on this 4K
		 * page; schedule a deferred madvise(MADV_DONTNEED) to return
		 * the page to the OS.
		 *
		 * v1.5 amortization: madvise is deferred via jp_page_pending_add()
		 * into a per-thread batch (cap JP_PC_PENDING_CAP=64). When the
		 * batch fills, jp_page_pending_flush() runs: it re-checks each
		 * page's counter and only fires madvise on pages that are STILL
		 * at zero. This skips the madvise for hot pages (typical
		 * alloc-churn, e.g. GCC AST build) and amortizes the syscall
		 * cost across many dec-to-0 events. For SQLite-style load-then-
		 * DROP, most pages stay drained after DROP, so the re-check
		 * confirms 0 and madvise fires, recovering ~1.6 GB as measured. */
		int dec_hit_zero = 0;
		JP_PC_DEC(h, size, &dec_hit_zero);
		if(dec_hit_zero) {
			jp_page_pending_add(h);
		}
		pool_put(h, g_pools + size, size);
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
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		mem = pool_get(g_pools + pid, pid);
		if(mem == NULL) return NULL;
		/* Unconditionally set s.size — pool_get may return a block
		 * whose header was zeroed by madvise (large blocks ≥ 4KB
		 * returned to OS at EBR drain time). This single store
		 * replaces the per-path writes that were previously in
		 * pool_get's mmap and buddy-split branches. */
		((union header *)mem)->s.size = pid;
		JP_STAT_ALLOC(pid);
		JP_STAT_LIVE(pid);
		/* Per-4K-page user-held counter: inc AFTER we've decided to
		 * hand this block to the user. The counter for the 4K page
		 * containing 'mem' (a block address, not user pointer) lives
		 * in a separate 4K counter-table page; sub-4K path only. */
		JP_PC_INC(mem, pid);
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
		mem = os_alloc_pages(size);
		if(mem == NULL) return NULL;
		JP_STAT_MMAP_DIRECT(size);
		JP_STAT_DIRECT_LIVE(size);
		union header *h = (union header *)mem;
		h->s.size = size;
	}
#ifdef JP_ALLOC_DEBUG
	{
		union header *h = (union header *)mem;
		h->s.magic = JP_UNSIZED_MAGIC;
		h->s.state = JP_STATE_LIVE;
	}
#endif
	return (union header *)mem + 1;
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
		if(likely(size < JP_ALLOC_POOL_COUNT)) size = 1U << size;
		size -= sizeof(union header);
	}
	if(new_size > size) {
		if(mem != NULL) {
			union header *h = (union header *)mem - 1;
			size_t hdr_size = h->s.size;
			if(hdr_size >= JP_ALLOC_POOL_COUNT) {
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
	if(likely(size < JP_ALLOC_POOL_COUNT)) size = 1U << size;
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