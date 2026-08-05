/* jp_alloc - lock free memory allocator
 *
 * Originally from https://github.com/jp-embedded/jp_alloc
 * Relicensed to GPL-2.0-or-later for integration with tup.
 *
 * Modifications for tup:
 * - Converted from C++ to C11.
 * - Added headerless sized API (jp_alloc_sized/jp_free_sized/jp_realloc_sized)
 *   with in-band freelist links and a 16-byte pool floor, so callers that
 *   know the size at free time skip the per-block header entirely.
 * - Added jp_alloc_reset() for valgrind cleanup compatibility.
 * - Added Windows (VirtualAlloc) backend so jp_alloc works on all platforms.
 * - Added mremap for large reallocs on Linux.
 * - Dropped LD_PRELOAD shims (__libc_*) and C++ operator new/delete overrides.
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

/* Pull in prototypes for the sized API (JP_ALLOC_COMPILED is defined
 * on the compile command line when this file is linked). This also
 * silences -Wmissing-prototypes for those entry points. */
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

#ifndef JP_ALLOC_POOL_COUNT
#define JP_ALLOC_POOL_COUNT 17  /* Gives pools of 1 - 64K */
#endif

/* ---- Debug header (enabled by -DJP_ALLOC_DEBUG) ----
 *
 * When JP_ALLOC_DEBUG is defined, every block gets a header on both
 * the sized and unsized paths. The header carries a magic number that
 * distinguishes sized from unsized allocations, a live/free state, and
 * (for the sized path) the original requested size. This catches:
 *
 *   - Wrong free API: jp_free_sized on a malloc'd pointer or vice versa
 *   - Wrong size:     jp_free_sized called with a different size than alloc
 *   - Double free:    free/jp_free_sized on a block already freed
 *   - ABA corruption: pool_get/sized_pool_get pops a block that is still LIVE
 *                     (the lock-free freelist handed out a block in use by
 *                     another thread)
 */
#ifdef JP_ALLOC_DEBUG
#define JP_SIZED_MAGIC   0x513A1DEDD01DULL  /* "SIZED"  */
#define JP_UNSIZED_MAGIC 0x0BADDEA11DECULL  /* "DEALLOC" */
#define JP_STATE_FREE    0xDEADBEEFFULL
#define JP_STATE_LIVE    0xCAFEBABEULL

#define JP_CHECK(cond, ...) do { \
	if(!(cond)) { fprintf(stderr, "jp_alloc: " __VA_ARGS__); abort(); } \
} while(0)

/* Header for the sized path. 32 bytes on 64-bit, 16-aligned. */
struct sized_header {
	uint64_t magic;
	uint64_t state;
	size_t size;
	struct sized_header *next;
};
#define JP_SIZED_HDRSZ sizeof(struct sized_header)
#else
#define JP_SIZED_HDRSZ 0
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

/* ---- Tagged pointer for ABA-free lock-free freelist ----
 *
 * The freelist head is a {pointer, tag} pair updated atomically with a
 * double-width compare-and-swap. The tag is incremented on every push
 * and pop, so even if the pointer cycles A→B→A, the tag differs and
 * the CAS fails — eliminating the ABA problem that caused duplicate
 * allocations under heavy thread contention.
 *
 * 64-bit: 128-bit CAS via __sync builtins (cmpxchg16b on x86-64 with
 *         -mcx16, LSE/LDREXD on ARM64)
 * 32-bit:  64-bit CAS via __sync builtins (cmpxchg8b on x86-32,
 *         LDREXD/STREXD on ARM32)
 */
#if defined(__SIZEOF_INT128__)
typedef unsigned __int128 tagged_val_t;
#define TAGGED_PTR(v)  ((void *)(uintptr_t)(v))
#define TAGGED_TAG(v)  ((uintptr_t)((v) >> 64))
#define TAGGED_MAKE(p, t)  ((tagged_val_t)(uintptr_t)(p) | ((tagged_val_t)(t) << 64))
#elif __SIZEOF_POINTER__ == 4
typedef uint64_t tagged_val_t;
#define TAGGED_PTR(v)  ((void *)(uint32_t)(v))
#define TAGGED_TAG(v)  ((uintptr_t)((v) >> 32))
#define TAGGED_MAKE(p, t)  ((tagged_val_t)(uint32_t)(uintptr_t)(p) | ((tagged_val_t)(uint32_t)(t) << 32))
#else
#error "Platform not supported: need 128-bit or 64-bit atomic CAS"
#endif

/* Atomically load the tagged head. Uses CAS(0,0) to avoid libatomic
 * calls on 128-bit targets. */
static inline tagged_val_t tagged_load(volatile tagged_val_t *p)
{
	return __sync_val_compare_and_swap(p, 0, 0);
}

/* Returns 1 on success. On failure, *old is updated with the current
 * value so the caller can retry without a separate load. */
static inline int tagged_cas(volatile tagged_val_t *p, tagged_val_t *old, tagged_val_t desired)
{
	tagged_val_t prev = __sync_val_compare_and_swap(p, *old, desired);
	if(prev == *old) return 1;
	*old = prev;
	return 0;
}

/* ---- Header'd path (for malloc/free override where size is unknown) ---- */

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
	volatile tagged_val_t head;
};

#ifdef DEBUG
struct {
	_Atomic(size_t) jp_alloc;
	_Atomic(size_t) jp_alloc_aligned;
	_Atomic(size_t) jp_realloc_expand;
	_Atomic(size_t) jp_realloc_relocate;
	_Atomic(size_t) mallopt;
} stat;
#endif

static struct pool g_pools[JP_ALLOC_POOL_COUNT];
static struct pool *g_pools_last = g_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Headerless sized path (caller knows size at free time) ---- */

struct sized_pool {
	volatile tagged_val_t head;
};

static struct sized_pool g_sized_pools[JP_ALLOC_POOL_COUNT];
static struct sized_pool *g_sized_pools_last = g_sized_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Header'd pool operations ---- */

static void pool_put(union header *h, struct pool *p)
{
#ifdef JP_ALLOC_DEBUG
	h->s.magic = JP_UNSIZED_MAGIC;
	h->s.state = JP_STATE_FREE;
#endif
	tagged_val_t expected = tagged_load(&p->head);
	void *next;
	uintptr_t tag;
	do {
		next = TAGGED_PTR(expected);
		tag = TAGGED_TAG(expected);
		h->s.next = (union header *)next;
	} while(!tagged_cas(&p->head, &expected, TAGGED_MAKE(h, tag + 1)));
}

static void *pool_get(struct pool *p)
{
	tagged_val_t expected = tagged_load(&p->head);
	union header *result = NULL;
	void *ptr;
	uintptr_t tag;
	while((ptr = TAGGED_PTR(expected)) != NULL) {
		union header *h = (union header *)ptr;
		tag = TAGGED_TAG(expected);
		if(tagged_cas(&p->head, &expected, TAGGED_MAKE(h->s.next, tag + 1))) {
			result = h;
			break;
		}
	}
	if(unlikely(result == NULL)) {
		if(p == g_pools_last) {
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
			result = os_alloc_pages(sz);
			if(likely(result != NULL)) {
				result->s.size = JP_ALLOC_POOL_COUNT - 1;
#ifdef JP_ALLOC_DEBUG
				result->s.magic = JP_UNSIZED_MAGIC;
				result->s.state = JP_STATE_FREE;
#endif
			}
		} else {
			char *mem = (char *)pool_get(p + 1);
			if(mem != NULL) {
				result = (union header *)mem;
				size_t sz = result->s.size - 1;
				union header *spare = (union header *)(mem + (1U << sz));
				result->s.size = sz;
				spare->s.size = sz;
				pool_put(spare, p);
			}
		}
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
		/* Equivalent to C++ std::bit_width(size - 1) */
		return 64 - __builtin_clzll(size - 1);
	}
	return 0;
}

static int is_pow2(size_t n)
{
	return (n & (n - 1)) == 0;
}

/* ---- Headerless sized pool operations ---- */

/* Floor at 16 bytes so the in-band freelist link (void*) fits
 * and every block is 16-byte aligned. In debug mode, JP_SIZED_HDRSZ
 * bytes are added for the debug header. */
static size_t pool_id_sized(size_t size)
{
	size += JP_SIZED_HDRSZ;
	if(size < 16) size = 16;
	return 64 - __builtin_clzll(size - 1);
}

static void sized_pool_put(void *mem, struct sized_pool *p)
{
#ifdef JP_ALLOC_DEBUG
	struct sized_header *h = (struct sized_header *)mem;
	h->magic = JP_SIZED_MAGIC;
	h->state = JP_STATE_FREE;
#endif
	tagged_val_t expected = tagged_load(&p->head);
	void *next;
	uintptr_t tag;
	do {
		next = TAGGED_PTR(expected);
		tag = TAGGED_TAG(expected);
#ifdef JP_ALLOC_DEBUG
		h->next = (struct sized_header *)next;
#else
		*(void **)mem = next;
#endif
	} while(!tagged_cas(&p->head, &expected, TAGGED_MAKE(mem, tag + 1)));
}

static void *sized_pool_get(size_t pid)
{
	struct sized_pool *p = g_sized_pools + pid;
	tagged_val_t expected = tagged_load(&p->head);
	void *result = NULL;
	void *ptr;
	uintptr_t tag;
	while((ptr = TAGGED_PTR(expected)) != NULL) {
		tag = TAGGED_TAG(expected);
#ifdef JP_ALLOC_DEBUG
		void *next = ((struct sized_header *)ptr)->next;
#else
		void *next = *(void **)ptr;
#endif
		if(tagged_cas(&p->head, &expected, TAGGED_MAKE(next, tag + 1))) {
			result = ptr;
			break;
		}
	}
	if(unlikely(result == NULL)) {
		if(p == g_sized_pools_last) {
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
			result = os_alloc_pages(sz);
#ifdef JP_ALLOC_DEBUG
			if(result) {
				struct sized_header *h = (struct sized_header *)result;
				h->magic = JP_SIZED_MAGIC;
				h->state = JP_STATE_FREE;
			}
#endif
		} else {
			char *mem = (char *)sized_pool_get(pid + 1);
			if(mem != NULL) {
				size_t buddy_size = 1U << pid;
				result = mem;
				void *spare = mem + buddy_size;
				sized_pool_put(spare, p);
			}
		}
	}
#ifdef JP_ALLOC_DEBUG
	if(result != NULL) {
		struct sized_header *h = (struct sized_header *)result;
		JP_CHECK(h->magic == JP_SIZED_MAGIC,
			 "sized_pool_get: wrong magic %llx (expected %llx)\n",
			 (unsigned long long)h->magic,
			 (unsigned long long)JP_SIZED_MAGIC);
		JP_CHECK(h->state == JP_STATE_FREE,
			 "sized_pool_get: ABA! block %p still LIVE (state=%llx)\n",
			 result, (unsigned long long)h->state);
	}
#endif
	return result;
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

/* Sized API (headerless) */

void *jp_alloc_sized(size_t size)
{
	size_t pid = pool_id_sized(size);
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		void *block = sized_pool_get(pid);
		if(block) {
#ifdef JP_ALLOC_DEBUG
			struct sized_header *h = (struct sized_header *)block;
			h->magic = JP_SIZED_MAGIC;
			h->state = JP_STATE_LIVE;
			h->size = size;
#endif
			return (char *)block + JP_SIZED_HDRSZ;
		}
		return NULL;
	} else {
		size_t ps_mask = os_page_size() - 1;
		size_t total = size + JP_SIZED_HDRSZ;
		size_t alloc_size = (total + ps_mask) & ~ps_mask;
		void *block = os_alloc_pages(alloc_size);
		if(block) {
#ifdef JP_ALLOC_DEBUG
			struct sized_header *h = (struct sized_header *)block;
			h->magic = JP_SIZED_MAGIC;
			h->state = JP_STATE_LIVE;
			h->size = size;
#endif
			return (char *)block + JP_SIZED_HDRSZ;
		}
		return NULL;
	}
}

void jp_free_sized(void *mem, size_t size)
{
	if(unlikely(mem == NULL)) return;

#ifdef JP_ALLOC_DEBUG
	struct sized_header *h = (struct sized_header *)((char *)mem - JP_SIZED_HDRSZ);
	JP_CHECK(h->magic == JP_SIZED_MAGIC,
		 "jp_free_sized: wrong API on %p (magic=%llx, not SIZED)\n",
		 mem, (unsigned long long)h->magic);
	JP_CHECK(h->state == JP_STATE_LIVE,
		 "jp_free_sized: double free on %p (state=%llx)\n",
		 mem, (unsigned long long)h->state);
	JP_CHECK(h->size == size,
		 "jp_free_sized: size mismatch on %p (got %zu, expected %zu)\n",
		 mem, h->size, size);
	h->state = JP_STATE_FREE;
	void *block = h;
#else
	void *block = mem;
#endif

	size_t pid = pool_id_sized(size);
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		sized_pool_put(block, g_sized_pools + pid);
	} else {
		size_t ps_mask = os_page_size() - 1;
		size_t total = size + JP_SIZED_HDRSZ;
		size_t alloc_size = (total + ps_mask) & ~ps_mask;
		os_free_pages(block, alloc_size);
	}
}

void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz)
{
	if(newsz == 0) {
		jp_free_sized(mem, oldsz);
		return NULL;
	}
	if(mem == NULL) {
		return jp_alloc_sized(newsz);
	}
#ifdef JP_ALLOC_DEBUG
	{
		struct sized_header *h = (struct sized_header *)((char *)mem - JP_SIZED_HDRSZ);
		JP_CHECK(h->magic == JP_SIZED_MAGIC,
			 "jp_realloc_sized: wrong API on %p (magic=%llx)\n",
			 mem, (unsigned long long)h->magic);
		JP_CHECK(h->state == JP_STATE_LIVE,
			 "jp_realloc_sized: non-live block %p (state=%llx)\n",
			 mem, (unsigned long long)h->state);
		JP_CHECK(h->size == oldsz,
			 "jp_realloc_sized: oldsz mismatch on %p (got %zu, expected %zu)\n",
			 mem, h->size, oldsz);
	}
#endif
	/* If both old and new fall in the same pool bucket, no copy needed */
	size_t old_pid = pool_id_sized(oldsz);
	size_t new_pid = pool_id_sized(newsz);
	if(old_pid == new_pid) {
		if(old_pid < JP_ALLOC_POOL_COUNT)
			return mem;		/* same pool bucket */
		/* both large: compare page-rounded sizes (including debug header) */
		size_t ps_mask = os_page_size() - 1;
		size_t old_total = oldsz + JP_SIZED_HDRSZ;
		size_t new_total = newsz + JP_SIZED_HDRSZ;
		if(((old_total + ps_mask) & ~ps_mask) == ((new_total + ps_mask) & ~ps_mask))
			return mem;
	}
	void *new_mem = jp_alloc_sized(newsz);
	if(new_mem) {
		size_t copy_size = oldsz < newsz ? oldsz : newsz;
		memcpy(new_mem, mem, copy_size);
		jp_free_sized(mem, oldsz);
	}
	return new_mem;
}

void jp_alloc_reset(void)
{
	/* No-op: pool memory is still reachable from the global pool head
	 * pointers, so valgrind reports it as "still reachable" (not an
	 * error). The OS reclaims all pages at process exit. This
	 * replaces the old mempool_clear() call in tup_valgrind_cleanup(). */
}

/* Header'd API (for malloc/free override) */

static void jp_free(void *mem)
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
		pool_put(h, g_pools + size);
	} else {
		size_t pre_padding = (size_t)mem & (os_page_size() - 1);
		os_free_pages((char *)h + pre_padding, size + pre_padding);
	}
}

static void *jp_alloc(size_t size)
{
	size += sizeof(union header);
	void *mem;
	size_t pid = pool_id(size);
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		mem = pool_get(g_pools + pid);
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

static void *jp_alloc_aligned(size_t alignment, size_t size)
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

static void *jp_calloc(size_t num, size_t nsize)
{
	size_t size = num * nsize;

	/* check mul overflow */
	if(num && nsize != size / num) {
		errno = ENOMEM;
		return NULL;
	}

	void *mem = jp_alloc(size);
	if(mem) memset(mem, 0, size);
	return mem;
}

static void *jp_realloc(void *mem, size_t new_size)
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
		/* Try mremap for large (mmap'd) allocations to avoid copying */
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
void *pvalloc(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
void *aligned_alloc(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *mem = jp_alloc_aligned(alignment, size);
	if(mem == NULL) return ENOMEM;
	*memptr = mem;
	return 0;
}

size_t malloc_usable_size(void *ptr)
{
	union header *h = (union header *)ptr - 1;
	size_t size = h->s.size;
	if(size < JP_ALLOC_POOL_COUNT) size = 1U << size;
	return size - sizeof(union header);
}

/* BSD/macOS extensions not declared by glibc headers. Forward
 * declarations here silence -Wmissing-prototypes. */
size_t malloc_size(void *ptr);
size_t malloc_good_size(size_t size);
void cfree(void *mem);

size_t malloc_size(void *ptr) { return malloc_usable_size(ptr); }
size_t malloc_good_size(size_t size) { return jp_good_size(size); }

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

/* cfree is removed from modern glibc headers (declared above). */
void cfree(void *mem) { jp_free(mem); }