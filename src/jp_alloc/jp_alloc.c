/* jp_alloc - lock-free memory allocator
 *
 * https://github.com/jp-embedded/jp_alloc
 * GPL-2.0-or-later
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
};

static _Thread_local struct tls_state tls;
static _Thread_local int tls_registered = 0;

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
	for(struct magazine *m = head; m; m = m->next) {
		for(size_t i = 0; i < JP_MAG_SIZE; i++) {
			if(m->slots[i])
				madvise(m->slots[i], sz, MADV_DONTNEED);
		}
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
	int outer = !tls.in_pop_cs;
	if(outer) {
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
			/* Largest pool: mmap one block. Size is set by jp_alloc. */
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
			result = (union header *)os_alloc_pages(sz);
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
	if(outer) {
		tls.in_pop_cs = 0;
		ebr_exit();
	}
#ifdef JP_ALLOC_DEBUG
	if(result != NULL) {
		JP_CHECK(result->s.magic == JP_UNSIZED_MAGIC,
			 "pool_get: wrong magic %llx (expected %llx)\n",
			 (unsigned long long)result->s.magic,
			 (unsigned long long)JP_UNSIZED_MAGIC);
		JP_CHECK(result->s.state == JP_STATE_FREE,
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
		pool_put(h, g_pools + size, size);
	} else {
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
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
		mem = os_alloc_pages(size);
		if(mem == NULL) return NULL;
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
	void *mem = alloc_pages_aligned(alignment, size);
	if(mem == NULL) return NULL;
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