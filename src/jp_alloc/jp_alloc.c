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
#include <errno.h>

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

/* ---- Atomic helpers (GCC/Clang builtins — work on all targets incl MinGW) ---- */

#define atomic_load_ptr(p) \
	__atomic_load_n(p, __ATOMIC_SEQ_CST)

#define atomic_cas_weak(p, expected, desired) \
	__atomic_compare_exchange_n(p, expected, desired, 1, \
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* ---- Header'd path (for malloc/free override where size is unknown) ---- */

union header {
	struct {
		size_t size;
		union header *next;
	} s;
	max_align_t _align;
};

struct pool {
	_Atomic(union header *) head;
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
	_Atomic(void *) head;
};

static struct sized_pool g_sized_pools[JP_ALLOC_POOL_COUNT];
static struct sized_pool *g_sized_pools_last = g_sized_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Header'd pool operations ---- */

static void pool_put(union header *h, struct pool *p)
{
	union header *expected = atomic_load_ptr(&p->head);
	do h->s.next = expected;
	while(!atomic_cas_weak(&p->head, &expected, h));
}

static void *pool_get(struct pool *p)
{
	union header *expected = atomic_load_ptr(&p->head);
	if(likely(expected != NULL)) {
		union header *next;
		do next = expected->s.next;
		while(!atomic_cas_weak(&p->head, &expected, next) && expected != NULL);
	}
	if(unlikely(expected == NULL)) {
		if(p == g_pools_last) {
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
			expected = os_alloc_pages(sz);
			if(likely(expected != NULL)) {
				expected->s.size = JP_ALLOC_POOL_COUNT - 1;
			}
		} else {
			char *mem = pool_get(p + 1);
			if(mem != NULL) {
				expected = (union header *)mem;
				size_t sz = expected->s.size - 1;
				union header *spare = (union header *)(mem + (1U << sz));
				expected->s.size = sz;
				spare->s.size = sz;
				pool_put(spare, p);
			}
		}
	}
	return expected;
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
 * and every block is 16-byte aligned. */
static size_t pool_id_sized(size_t size)
{
	if(size < 16) size = 16;
	return 64 - __builtin_clzll(size - 1);
}

static void sized_pool_put(void *mem, struct sized_pool *p)
{
	void *expected = atomic_load_ptr(&p->head);
	do *(void **)mem = expected;
	while(!atomic_cas_weak(&p->head, &expected, mem));
}

static void *sized_pool_get(size_t pid)
{
	struct sized_pool *p = g_sized_pools + pid;
	void *expected = atomic_load_ptr(&p->head);
	if(likely(expected != NULL)) {
		void *next;
		do next = *(void **)expected;
		while(!atomic_cas_weak(&p->head, &expected, next) && expected != NULL);
	}
	if(unlikely(expected == NULL)) {
		if(p == g_sized_pools_last) {
			size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
			expected = os_alloc_pages(sz);
		} else {
			char *mem = sized_pool_get(pid + 1);
			if(mem != NULL) {
				size_t buddy_size = 1U << pid;
				expected = mem;
				void *spare = mem + buddy_size;
				sized_pool_put(spare, p);
			}
		}
	}
	return expected;
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
		return sized_pool_get(pid);
	} else {
		size_t ps_mask = os_page_size() - 1;
		size_t alloc_size = (size + ps_mask) & ~ps_mask;
		return os_alloc_pages(alloc_size);
	}
}

void jp_free_sized(void *mem, size_t size)
{
	if(unlikely(mem == NULL)) return;

	size_t pid = pool_id_sized(size);
	if(likely(pid < JP_ALLOC_POOL_COUNT)) {
		sized_pool_put(mem, g_sized_pools + pid);
	} else {
		size_t ps_mask = os_page_size() - 1;
		size_t alloc_size = (size + ps_mask) & ~ps_mask;
		os_free_pages(mem, alloc_size);
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
	/* If both old and new fall in the same pool bucket, no copy needed */
	size_t old_pid = pool_id_sized(oldsz);
	size_t new_pid = pool_id_sized(newsz);
	if(old_pid == new_pid) {
		if(old_pid < JP_ALLOC_POOL_COUNT)
			return mem;		/* same pool bucket */
		/* both large: compare page-rounded sizes */
		size_t ps_mask = os_page_size() - 1;
		if(((oldsz + ps_mask) & ~ps_mask) == ((newsz + ps_mask) & ~ps_mask))
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
	} else {
		size_t ps_mask = os_page_size() - 1;
		size = (size + ps_mask) & ~ps_mask;
		mem = os_alloc_pages(size);
		if(mem == NULL) return NULL;
		union header *h = (union header *)mem;
		h->s.size = size;
	}
	return (union header *)mem + 1;
}

static void *jp_alloc_aligned(size_t alignment, size_t size)
{
	size += sizeof(union header);
	void *mem = alloc_pages_aligned(alignment, size);
	if(mem == NULL) return NULL;
	union header *h = (union header *)mem;
	h->s.size = size;
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

void cfree(void *mem) { jp_free(mem); }