/* jp_alloc - lock free memory allocator
 *
 * Originally from https://github.com/jp-embedded/jp_alloc
 * Relicensed to GPL-2.0-or-later for integration with tup.
 *
 * Modifications for tup:
 * - Converted from C++ to C11.
 * - Added jp_alloc_reset() for valgrind cleanup compatibility.
 * - Added Windows (VirtualAlloc) backend so jp_alloc works on all platforms.
 * - Added mremap for large reallocs on Linux.
 * - Dropped LD_PRELOAD shims (__libc_*) and C++ operator new/delete overrides.
 * - Replaced the 128-bit tagged-pointer freelist (cmpxchg16b on x86-64) with
 *   a 64-bit compare-and-swap freelist protected by a 3-epoch ring EBR plus a
 *   per-thread fixed-array cache (N=32 per size class). This removes the
 *   -mcx16 build requirement and reduces global-freelist contention. The
 *   EBR layer guarantees ABA-freedom: a popped block cannot reappear at the
 *   global freelist head until all threads that observed the prior head have
 *   left their pop critical section. The TLS cache further delays returns to
 *   the global freelist and batches them into single atomic chain pushes,
 *   amortizing EBR bookkeeping and reducing CAS contention. Thread teardown
 *   deposits each per-pool retired batch into a global limbo indexed by
 *   retire_epoch % 3, drained lazily by live threads during epoch advance;
 *   this keeps teardown ABA-safe and leak-free without per-thread locks.
 * - Removed the headerless sized API (jp_alloc_sized/jp_free_sized/
 *   jp_realloc_sized real definitions, struct sized_pool, g_sized_pools,
 *   sized_link_get/set, g_limbo_sized, sized_pool_put/get, pool_id_sized,
 *   JP_SIZED_MAGIC, struct sized_header, JP_SIZED_HDRSZ). All allocation now
 *   goes through the single unified header'd malloc/free pool path; the sized
 *   entry points remain in the header as inline libc wrappers only.
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

/* ---- Cache hit/miss instrumentation (bench-only, compiled out in release)
 * ----
 *
 * Counted only when JP_ALLOC_DEBUG is defined so release builds pay zero
 * cost. The bench declares these extern and reads them directly; the
 * functions are not part of the public jp_alloc.h interface. */
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
#define JP_ALLOC_POOL_COUNT 17  /* Gives pools of 1 - 64K */
#endif

#ifndef JP_CACHELINE
#define JP_CACHELINE 64
#endif

/* _Alignas / _Thread_local fallbacks for old compilers */
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
#define JP_UNSIZED_MAGIC 0x0BADDEA11DECULL  /* "DEALLOC" */
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
 * 64-bit CAS freelist + EBR + thread-local cache
 * ==========================================================================
 *
 * The freelist head is a single 64-bit atomic void*. ABA is prevented by a
 * 3-epoch ring EBR: each per-thread pop critical section is bracketed by
 * ebr_enter()/ebr_exit() which publishes the thread's observed epoch. A
 * retired block cannot be returned to the global freelist until every
 * thread's epoch has advanced past the retire epoch. Per-thread retired
 * batches are stored per-pool-per-epoch so drainage is an O(1) chain splice.
 *
 * On top of the global freelist sits a per-thread fixed-array cache
 * (N=32 per size class) that intercepts the hottest alloc/free paths with
 * no atomics. When a cache fills, half its entries are flushed as a single
 * atomic chain to the retire list, retiring once for the whole batch.
 *
 * Memory ordering:
 *   load        acquire  (publishes next link via dependent read)
 *   CAS success acq_rel
 *   CAS fail    acquire
 */

/* ---- Atomic primitives (use __atomic builtins; no <stdatomic.h> dependency) ---- */
static inline void *atomic_load_ptr(void * volatile *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_ptr(void * volatile *p, void *v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}
/* Returns 1 on success; on failure *old is updated with the current value. */
static inline int atomic_cas_ptr(void * volatile *p, void **old, void *desired)
{
	return __atomic_compare_exchange_n(p, old, desired,
		0 /* strong */,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
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

struct pool {
	void * volatile head;        /* atomic freelist head */
	char _pad[JP_CACHELINE - sizeof(void *)];  /* pad to one cache line */
};

static _Alignas(JP_CACHELINE) struct pool g_pools[JP_ALLOC_POOL_COUNT];
static struct pool *g_pools_last = g_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Epoch-Based Reclamation (3-epoch ring) ----
 *
 * Per thread announces (active, epoch) before entering a pop critical
 * section. The global epoch counter advances only when every registered
 * thread is either inactive or has announced the current epoch.
 *
 * Per-thread retired batches are stored per-pool and per epoch-slot
 * (epoch % 3) so drainage is an O(1) global freelist splice of a single
 * pool's chain. This keeps the drain path simple.
 *
 * Slot semantics: a block retired at retire_epoch=R lives in slot (R%3).
 * When g_epoch advances from old to old+1, slot ((old-1) % 3) is safe to
 * drain: any thread that could have observed a head pointing at a block
 * from age 2 has announced epoch >= old.
 */
#define JP_EBR_EPOCHS 3
#define JP_EBR_INACTIVE (-1L)   /* value of ebr_thread.epoch when inactive */

struct ebr_thread {
	_Atomic(int)    active;
	_Atomic(long)   epoch;
	_Atomic(struct ebr_thread *) next;
};

static _Atomic(long) g_epoch = 0;
static _Atomic(struct ebr_thread *) g_thread_list = NULL;

/* ---- Per-thread state ----
 *
 * fixed-array caches: top of stack is slot[cnt-1]. Allocations pull from
 * the top; frees push to the top. LIFO gives best locality.
 *
 * A full cache flushes half (16) entries as a single chain to EBR retire,
 * amortizing the global CAS and epoch update to one per 16 frees.
 */
#ifndef JP_CACHE_N
#define JP_CACHE_N 32
#endif
#ifndef JP_CACHE_FLUSH
#define JP_CACHE_FLUSH  (JP_CACHE_N / 2)
#endif
/* How many blocks to pop from the global freelist in one CAS when the TLS
 * cache misses. Each refill is one CAS for up to JP_REFILL blocks, amortizing
 * the global-freelist contention across ~JP_REFILL future cache-missed pops. */
#ifndef JP_REFILL
#define JP_REFILL 16
#endif

/* Per-pool retired batches for one epoch slot. */
struct retired_chain {
	void *head;     /* first block, link via hdr->s.next */
	void *tail;     /* last block, link == NULL */
};

struct tls_state {
	struct ebr_thread *self;
	/* cache */
	void *cache[JP_ALLOC_POOL_COUNT][JP_CACHE_N];
	size_t cnt[JP_ALLOC_POOL_COUNT];
	/* retired batches: per-pool, per-epoch-slot */
	struct retired_chain retired[JP_ALLOC_POOL_COUNT][JP_EBR_EPOCHS];
	int in_pop_cs;     /* re-entrancy guard for buddy-split recursion */
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
		/* Allocate the ebr_thread record out of the OS page allocator (not
		 * our own pools — we don't want a circular dependency). One page is
		 * plenty for many records; we just use the first bytes. */
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

/* Pre-declaration used by tls_destructor and drains */
static void global_push_chain(struct pool *p, void *chain_head, void *chain_tail);

/* Move this thread's retired batch for (pool pid, slot s) into the global
 * limbo of the same (pid, slot). Other threads may drain it later in
 * ebr_try_advance(). */
struct limbo_slot {
	void * volatile head;
	char _pad[JP_CACHELINE - sizeof(void *)];
};
static _Alignas(JP_CACHELINE) struct limbo_slot g_limbo[JP_ALLOC_POOL_COUNT][JP_EBR_EPOCHS];

static void deposit_to_limbo(size_t pid, int slot, void *head, void *tail)
{
	struct limbo_slot *s = &g_limbo[pid][slot];
	void *old_head = atomic_load_ptr(&s->head);
	for(;;) {
		((union header *)tail)->s.next = (union header *)old_head;
		if(atomic_cas_ptr(&s->head, &old_head, head)) break;
	}
}

/* Steal and splice a limbo slot's chain onto its global pool. */
static void drain_limbo(size_t pid, int slot)
{
	struct limbo_slot *s = &g_limbo[pid][slot];
	void *head = __atomic_load_n(&s->head, __ATOMIC_ACQUIRE);
	if(!head) return;
	if(!__atomic_compare_exchange_n(&s->head, &head, NULL,
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return;
	void *tail = head;
	while(((union header *)tail)->s.next != NULL) tail = ((union header *)tail)->s.next;
	global_push_chain(&g_pools[pid], head, tail);
}

/* ---- TLS destructor: flush caches and retire-list into the global limbo ---- */
static void tls_destructor(void *p)
{
	(void)p;
	if(!tls_registered) return;
	struct tls_state *t = &tls;

	/* Flush all caches to per-thread retired chains (this thread's epoch
	 * is still pinned to its last announced value; the new chain inherits
	 * that retire epoch). */
	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		if(t->cnt[pid] > 0) {
			void *head = t->cache[pid][0];
			void *tail = t->cache[pid][t->cnt[pid] - 1];
			for(size_t i = 0; i < t->cnt[pid] - 1; i++)
				((union header *)t->cache[pid][i])->s.next =
					(union header *)t->cache[pid][i+1];
			((union header *)tail)->s.next = NULL;
			t->retired[pid][0].head = head;
			t->retired[pid][0].tail = tail;
			t->cnt[pid] = 0;
		}
	}

	/* Deposit all per-epoch retired batches into the global limbo; live
	 * threads drain them lazily. The block cannot be reused until a
	 * future epoch advance, which is ABA-safe. */
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

	/* Mark this thread inactive so other threads stop waiting on its epoch. */
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
		/* Publish observed epoch, then set active. Advancing threads
		 * re-check active vs (old-1) before committing. */
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

/* Try to advance the global epoch by 1. Returns 1 if it advanced.
 * On success, drain the bucket at slot = ((old_epoch - 1) % JP_EBR_EPOCHS)
 * for both per-thread retired lists and the global limbo. */
static void ebr_try_advance(void)
{
	long old = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
	/* All threads must be inactive or announcing `old`. If any thread is
	 * active on an older epoch, we cannot advance. Note: a thread that is
	 * active on `old` is also OK to advance (since its observed epoch equals
	 * the new target `old+1` — no, it doesn't). The standard EBR rule: we
	 * can advance from old to old+1 only if every active thread's epoch is
	 * exactly `old` (none lagging at old-1). Threads announcing `old` are
	 * safe to advance past `old`'s predecessor, which is what drains
	 * (old-1) % 3. */
	struct ebr_thread *r = __atomic_load_n(&g_thread_list, __ATOMIC_ACQUIRE);
	while(r) {
		int active = __atomic_load_n(&r->active, __ATOMIC_ACQUIRE);
		if(active) {
			long e = __atomic_load_n(&r->epoch, __ATOMIC_ACQUIRE);
			if(e < old) return; /* someone lags behind */
		}
		r = __atomic_load_n(&r->next, __ATOMIC_ACQUIRE);
	}
	/* Try to advance. */
	if(!__atomic_compare_exchange_n(&g_epoch, &old, old + 1,
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return; /* someone else advanced first */

	/* Drained slot: blocks retired at epoch (old-1) are now safe. */
	int drain_slot = (int)(((old - 1) + JP_EBR_EPOCHS) % JP_EBR_EPOCHS);

	/* Drain per-thread retired batches for this slot into the global pools. */
	for(size_t pid = 0; pid < JP_ALLOC_POOL_COUNT; pid++) {
		if(tls.retired[pid][drain_slot].head) {
			void *h = tls.retired[pid][drain_slot].head;
			void *t = tls.retired[pid][drain_slot].tail;
			tls.retired[pid][drain_slot].head = tls.retired[pid][drain_slot].tail = NULL;
			global_push_chain(&g_pools[pid], h, t);
		}
		/* Drain the limbo (other dead threads' deposits) for this slot too. */
		drain_limbo(pid, drain_slot);
	}
}

/* ---- Header'd pool operations (cache + EBR-protected global freelist) ---- */

static void global_push_chain(struct pool *p, void *chain_head, void *chain_tail)
{
	void *old = atomic_load_ptr(&p->head);
	do {
		((union header *)chain_tail)->s.next = (union header *)old;
	} while(!atomic_cas_ptr(&p->head, &old, chain_head));
}

static void pool_put(union header *h, struct pool *p, size_t pid)
{
	(void)p;
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_FREE;
#endif
	/* Try the per-thread cache first. */
	if(likely(tls.cnt[pid] < JP_CACHE_N)) {
		tls.cache[pid][tls.cnt[pid]++] = h;
		return;
	}
	/* Cache full: flush half as a single chain (LIFO order; the bottom of
	 * the stack becomes the new head of the retired chain). */
	void *flush_head = tls.cache[pid][0];
	void *flush_tail = tls.cache[pid][JP_CACHE_FLUSH - 1];
	for(size_t i = 0; i < JP_CACHE_FLUSH - 1; i++)
		((union header *)tls.cache[pid][i])->s.next =
			(union header *)tls.cache[pid][i+1];
	((union header *)flush_tail)->s.next = NULL;
	/* Compact remaining entries down. */
	tls.cnt[pid] -= JP_CACHE_FLUSH;
	memmove(&tls.cache[pid][0],
		&tls.cache[pid][JP_CACHE_FLUSH],
		tls.cnt[pid] * sizeof(void *));
	/* Append h to the chain (cheaper than a separate retire). */
	((union header *)flush_tail)->s.next = h;
	((union header *)h)->s.next = NULL;
	flush_tail = h;
	/* Retire the chain at current epoch. */
	long e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
	int slot = (int)(e % JP_EBR_EPOCHS);
	struct retired_chain *rc = &tls.retired[pid][slot];
	if(rc->head == NULL) {
		rc->head = flush_head;
		rc->tail = flush_tail;
	} else {
		((union header *)rc->tail)->s.next = (union header *)flush_head;
		rc->tail = flush_tail;
	}
	ebr_try_advance();
}

static void *pool_get(struct pool *p, size_t pid)
	{
		/* Try cache first. */
		if(likely(tls.cnt[pid] > 0)) {
			JP_COUNT_HIT;
			return (union header *)tls.cache[pid][--tls.cnt[pid]];
		}
		/* Cache miss: batched refill from the global freelist under EBR.
		 * Pop up to JP_REFILL blocks in one CAS — the first becomes the
		 * result, the rest are installed into the TLS cache. Re-entrancy
		 * note: pool_get may recurse via buddy-split; only the outermost
		 * call should bracket ebr_enter/exit. */
		JP_COUNT_MISS;
		int outer = !tls.in_pop_cs;
		if(outer) {
			ebr_enter();
			tls.in_pop_cs = 1;
		}
		union header *result = NULL;
		{
			void *head = atomic_load_ptr(&p->head);
			for(;;) {
				if(head == NULL) break;
				/* Walk up to JP_REFILL links from the global freelist. */
				void *tail = head; size_t n = 1;
				while(n < JP_REFILL) {
					void *next = ((union header *)tail)->s.next;
					if(next == NULL) break;
					tail = next; n++;
				}
				void *new_head;
				if(n < JP_REFILL) {
					/* tail is the end of the freelist (its next is NULL). */
					new_head = NULL;
				} else {
					/* n == JP_REFILL; tail is the JP_REFILL-th block. */
					new_head = ((union header *)tail)->s.next;
				}
				if(atomic_cas_ptr(&p->head, &head, new_head)) {
					/* Won a chain of n blocks. Install n-1 into the cache
					 * (limit to JP_CACHE_N-1 to leave room for the result). */
					result = (union header *)head;
					if(n > 1) {
						void *cur = ((union header *)head)->s.next;
						((union header *)tail)->s.next = NULL;
						size_t install = n - 1;
						if(install > JP_CACHE_N - 1) install = JP_CACHE_N - 1;
						size_t i = 0;
						while(i < install) {
							tls.cache[pid][tls.cnt[pid]++] = cur;
							cur = ((union header *)cur)->s.next;
							i++;
						}
					}
					break;
				}
				/* CAS failed (head updated); retry with the new head. */
			}
		}
		if(result == NULL) {
			if(unlikely(p == g_pools_last)) {
				size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
				result = (union header *)os_alloc_pages(sz);
				if(likely(result != NULL)) {
					result->s.size = JP_ALLOC_POOL_COUNT - 1;
	#ifdef JP_ALLOC_DEBUG
					result->s.magic = JP_UNSIZED_MAGIC;
					result->s.state = JP_STATE_FREE;
	#endif
				}
			} else {
				/* Buddy split: pop a block from the next-larger pool and halve it. */
				char *mem = (char *)pool_get(p + 1, pid + 1);
				if(mem != NULL) {
					result = (union header *)mem;
					size_t sz = result->s.size - 1;
					union header *spare = (union header *)(mem + (1U << sz));
					result->s.size = sz;
					spare->s.size = sz;
	#ifdef JP_ALLOC_DEBUG
					spare->s.magic = JP_UNSIZED_MAGIC;
					spare->s.state = JP_STATE_FREE;
	#endif
					/* Recycle the spare buddy into OUR cache (not the parent's),
					 * so the spare is reused locally rather than bouncing back
					 * to the global freelist. */
					if(likely(tls.cnt[pid] < JP_CACHE_N)) {
						tls.cache[pid][tls.cnt[pid]++] = spare;
					} else {
						/* Unlikely; fall back to retiring it. */
						pool_put(spare, p, pid);
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

/* ---- Aligned page allocation (header'd path) ---- */

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
	 * error). The OS reclaims all pages at process exit. This
	 * replaces the old mempool_clear() call in tup_valgrind_cleanup(). */
}

/* ---- libc malloc/free/calloc/realloc overrides ---- */

void jp_free(void *mem)
{
	if(unlikely(mem == NULL)) return;

	union header *h = (union header *)mem - 1;
#ifdef JP_ALLOC_DEBUG
	JP_CHECK(h->s.magic == JP_UNSIZED_MAGIC,
		 "jp_free: wrong API on %p (magic=%llx, not UNSIZED)\n",
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
		if(size < JP_ALLOC_POOL_COUNT) size = 1U << size;
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