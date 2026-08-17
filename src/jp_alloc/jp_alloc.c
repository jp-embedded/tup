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
 * - Per-4K-page user-held counter with deferred madvise(MADV_DONTNEED):
 *   when all user-held blocks on a 4K page are freed, the page returns
 *   to the OS. Sub-4K block fragmentation no longer leaks RSS.
 * - 8M-aligned regions (mmap 16M + trim) with a 4K counter-table page
 * - Windows (VirtualAlloc) and POSIX (mmap) backends
 * - mremap for large reallocs on Linux
 * - Portable to 32-bit and 64-bit (GCC 4.7+, Clang 3.0+, MSVC 2015+)
 *
 * Cross-thread free: when Thread B frees a block that Thread A allocated,
 * B pushes it onto B's own freelist. B can hand it out later. The
 * per-4K-page counter (in A's region's counter table) is decremented
 * atomically by B. When it hits 0, B schedules madvise for that 4K page
 * (in A's region). Any thread can madvise any virtual address.
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
#define JP_ALLOC_INTERMEDIATE_K 0
#endif

/* ---- Pool table: power-of-2 + intermediate size classes ----
 *
 * K=0: pure power-of-2 (24 pools, 1..8M).
 * K=4: 39 pools. Intermediate = 2^i * 5. malloc(128)->160B (0 waste vs 256B).
 * K=6: 37 pools. Intermediate = 2^i * 21.
 * Intermediates with small < sizeof(union header) are skipped.
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
	{2560u,0,8192u,512u},
	{4096u,1,0,0},
	{5120u,0,16384u,1024u},
	{8192u,1,0,0},
	{10240u,0,32768u,2048u},
	{16384u,1,0,0},
	{20480u,0,65536u,4096u},
	{32768u,1,0,0},
	{40960u,0,131072u,8192u},
	{65536u,1,0,0},
	{81920u,0,262144u,16384u},
	{131072u,1,0,0},
	{163840u,0,524288u,32768u},
	{262144u,1,0,0},
	{327680u,0,1048576u,65536u},
	{524288u,1,0,0},
	{655360u,0,2097152u,131072u},
	{1048576u,1,0,0},
	{1310720u,0,4194304u,262144u},
	{2097152u,1,0,0},
	{2621440u,0,8388608u,524288u},
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
	{2688u,0,8192u,128u},
	{4096u,1,0,0},
	{5376u,0,16384u,256u},
	{8192u,1,0,0},
	{10752u,0,32768u,512u},
	{16384u,1,0,0},
	{21504u,0,65536u,1024u},
	{32768u,1,0,0},
	{43008u,0,131072u,2048u},
	{65536u,1,0,0},
	{86016u,0,262144u,4096u},
	{131072u,1,0,0},
	{172032u,0,524288u,8192u},
	{262144u,1,0,0},
	{344064u,0,1048576u,16384u},
	{524288u,1,0,0},
	{688128u,0,2097152u,32768u},
	{1048576u,1,0,0},
	{1376256u,0,4194304u,65536u},
	{2097152u,1,0,0},
	{2752512u,0,8388608u,131072u},
	{4194304u,1,0,0},
	{8388608u,1,0,0},
};
#else
#error "JP_ALLOC_INTERMEDIATE_K must be 0, 4, or 6"
#endif

#define JP_POOL_COUNT (sizeof(g_pools) / sizeof(g_pools[0]))

static inline size_t pool_id_by_size(size_t size)
{
	for(size_t pid = 0; pid < JP_POOL_COUNT; pid++)
		if(g_pools[pid].size >= size) return pid;
	return JP_POOL_COUNT;
}

#define JP_PID_LUT_SIZE 257
static uint8_t g_pid_lut[JP_PID_LUT_SIZE];
static int g_pid_lut_done = 0;
static void jp_pid_lut_init(void)
{
	for(size_t pid = 0; pid < JP_POOL_COUNT; pid++) {
		size_t sz = g_pools[pid].size;
		size_t start = (pid == 0) ? 0 : g_pools[pid-1].size + 1;
		for(size_t s = start; s <= sz && s < JP_PID_LUT_SIZE; s++)
			g_pid_lut[s] = (uint8_t)pid;
	}
	g_pid_lut[0] = 0;
	g_pid_lut_done = 1;
}

/* madvise(MADV_DONTNEED) returns freed large-block pages to the OS,
 * reducing RSS. Only applies to pools where blocks are page-aligned
 * and span whole pages (pid >= JP_MADVISE_PID). Smaller blocks share
 * pages with other blocks and can't be madvise'd individually.
 * Disabled on Windows (no madvise). */
/* First page-aligned pool (>= 4K). Dynamic — depends on pool table. */
static size_t jp_madvise_pid_val(void)
{
	static size_t cached = 0;
	if(!cached) {
		for(size_t pid = 0; pid < JP_POOL_COUNT; pid++)
			if(g_pools[pid].size >= 4096) { cached = pid; break; }
	}
	return cached;
}
#define JP_MADVISE_PID jp_madvise_pid_val()

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
 * When pool N is empty and pool_get descends to pool N+1, the buddy
 * cascade commits every 4K page it touches. Without a per-4K-page
 * counter, freed small blocks can't have their pages returned to the
 * OS because their neighbors on the same 4K page may still be in use.
 *
 * Fix: maintain a per-4K-page counter of USER-HELD blocks. When the
 * counter dec-and-tests to 0, schedule madvise(MADV_DONTNEED) via a
 * per-thread deferred batch (re-check at flush time to skip hot pages).
 *
 * Storage: 4K (= 2048 uint16_t entries) per 8M region = 0.049% overhead.
 * 8M-aligned regions: required so any sub-block address can mask down
 * to its region base in O(1).
 */
#ifndef JP_ALLOC_PAGE_COUNTER
#define JP_ALLOC_PAGE_COUNTER 1
#endif

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
static _Atomic(size_t) g_page_madvise_count;
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

#if JP_ALLOC_PAGE_COUNTER
#define JP2_REGION_SIZE   (8 * 1024 * 1024)
#define JP2_TABLE_SIZE    (4 * 1024)
#define JP2_TABLE_ENTRIES (JP2_TABLE_SIZE / sizeof(uint16_t))

#ifndef _WIN32
static void *os_alloc_8m_region_counter(void)
{
	size_t region_sz = JP2_REGION_SIZE;
	size_t table_sz  = JP2_TABLE_SIZE;
	size_t span = region_sz * 2 + table_sz;
	char *mem = (char *)os_alloc_pages(span);
	if(!mem) return NULL;
	uintptr_t base = (uintptr_t)mem;
	uintptr_t aligned = (base + region_sz - 1) & ~((uintptr_t)region_sz - 1);
	if(aligned != base) {
		os_free_pages(mem, (size_t)(aligned - base));
	}
	uintptr_t kept_end = aligned + region_sz + table_sz;
	uintptr_t span_end = base + span;
	if(kept_end < span_end) {
		os_free_pages((char *)kept_end, (size_t)(span_end - kept_end));
	}
	uint16_t *table = (uint16_t *)(aligned + region_sz);
	memset(table, 0, table_sz);
	return (void *)aligned;
}
#else
static void *os_alloc_8m_region_counter(void)
{
	return os_alloc_pages(JP2_REGION_SIZE);
}
#endif

static inline uint16_t *jp_page_counter_of(void *block_addr)
{
	uintptr_t a = (uintptr_t)block_addr;
	uintptr_t base = a & ~((uintptr_t)(JP2_REGION_SIZE - 1));
	uint16_t *table = (uint16_t *)(base + JP2_REGION_SIZE);
	return table + ((a - base) >> 12);
}

static inline void jp_page_counter_inc(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	__atomic_add_fetch(cnt, 1, __ATOMIC_RELAXED);
}

static inline int jp_page_counter_dec_and_test(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	return __atomic_sub_fetch(cnt, 1, __ATOMIC_RELAXED) == 0;
}

static inline int jp_page_counter_is_still_zero(void *block_addr)
{
	uint16_t *cnt = jp_page_counter_of(block_addr);
	return __atomic_load_n(cnt, __ATOMIC_RELAXED) == 0;
}

#define JP_PC_PENDING_CAP 64
struct jp_pc_pending {
	void *pages[JP_PC_PENDING_CAP];
	size_t cnt;
};

#define JP_PC_INC(mem, pid)   do { if((pid) < JP_MADVISE_PID) jp_page_counter_inc(mem); } while(0)
#define JP_PC_DEC(h, pid, dec_hit_zero) do { \
	*(dec_hit_zero) = ((pid) < JP_MADVISE_PID) && jp_page_counter_dec_and_test(h); \
} while(0)
#else
#define JP_PC_INC(mem, pid)   ((void)0)
#define JP_PC_DEC(h, pid, dec_hit_zero) (*(dec_hit_zero) = 0)
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
 * it onto B's own freelist. B can reuse it later. The per-4K-page
 * counter (in A's region) is decremented atomically by B; on 0, B
 * schedules madvise. No data crosses between threads — just memory
 * addresses (which are process-wide valid) and atomic counter ops. */

#if JP_ALLOC_PAGE_COUNTER
struct tls_state {
	union header *freelist[JP_POOL_COUNT];
	struct jp_pc_pending pc_pending;
};
#else
struct tls_state {
	union header *freelist[JP_POOL_COUNT];
};
#endif

static _Thread_local struct tls_state tls;
static _Thread_local int tls_registered = 0;

/* ---- Pending page-madvise batch (per-thread) ---- */
#if JP_ALLOC_PAGE_COUNTER
static void jp_page_pending_flush(void)
{
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
}

static void jp_page_pending_add(void *block_addr)
{
	if(tls.pc_pending.cnt >= JP_PC_PENDING_CAP) {
		jp_page_pending_flush();
	}
	void *page = (void *)((uintptr_t)block_addr & ~((uintptr_t)4096 - 1));
	for(size_t i = 0; i < tls.pc_pending.cnt; i++) {
		if(tls.pc_pending.pages[i] == page)
			return;
	}
	tls.pc_pending.pages[tls.pc_pending.cnt++] = page;
}
#endif

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

/* TLS destructor: flush any pending madvise batch. The freelist
 * pointers are dropped — the underlying memory (8M regions) stays
 * mapped and is reclaimed by the OS at process exit. Per-4K-page
 * madvise already returned physical pages during the thread's
 * lifetime; unflushed pending pages get one last flush here. */
static void tls_destructor(void *p)
{
	(void)p;
	if(!tls_registered) return;
#if JP_ALLOC_PAGE_COUNTER
	jp_page_pending_flush();
#endif
	tls_registered = 0;
}

/* ---- Pool operations (per-thread, zero atomics) ---- */

static inline size_t pool_id(size_t size)
{
	if(unlikely(!g_pid_lut_done)) jp_pid_lut_init();
	if(likely(size <= 256)) return g_pid_lut[size];
	return pool_id_by_size(size);
}

static int is_pow2(size_t n)
{
	return (n & (n - 1)) == 0;
}

/* pool_get: pop from freelist[pid]. If empty, split from a larger pool.
 * For power-of-2 pools: binary buddy split (half + half).
 * For intermediate pools: asymmetric split from next pow2 pool
 * (1*small + 3*intermediate = pow2). */
static union header *pool_get(size_t pid)
{
	/* Fast path: pop from freelist. */
	if(likely(tls.freelist[pid] != NULL)) {
		union header *h = tls.freelist[pid];
		tls.freelist[pid] = h->s.next;
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
			spare->s.next = tls.freelist[pid];
			tls.freelist[pid] = spare;
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
				h->s.next = tls.freelist[pid];
				tls.freelist[pid] = h;
				p += inter_sz;
			}
			size_t small_pid = pool_id_by_size(small_sz);
			union header *small_h = (union header *)p;
			small_h->s.next = tls.freelist[small_pid];
			tls.freelist[small_pid] = small_h;
			union header *h = tls.freelist[pid];
			tls.freelist[pid] = h->s.next;
			return h;
		}
	}
	/* Largest pool: mmap a new 8M-aligned region. */
	size_t sz = g_pools[pid].size;
#if JP_ALLOC_PAGE_COUNTER
	union header *h = (union header *)os_alloc_8m_region_counter();
#else
	union header *h = (union header *)os_alloc_pages(sz);
#endif
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
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_FREE;
#endif
	h->s.next = tls.freelist[pid];
	tls.freelist[pid] = h;
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
	size_t page_madv = __atomic_load_n(&g_page_madvise_count, __ATOMIC_RELAXED);
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
	fprintf(f, "  page madvise (4K)  : %zu calls\n", page_madv);
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
		/* Per-4K-page user-held counter: dec-and-test before pool_put. */
		int dec_hit_zero = 0;
		JP_PC_DEC(h, size, &dec_hit_zero);
		if(dec_hit_zero) {
#if JP_ALLOC_PAGE_COUNTER
			jp_page_pending_add(h);
#endif
		}
		pool_put(h, size);
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
		JP_PC_INC(h, pid);
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