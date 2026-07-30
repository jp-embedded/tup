/* jp_alloc - lock free memory allocator
 *
 * Originally from https://github.com/jp-embedded/jp_alloc
 * Relicensed to GPL-2.0-or-later for integration with tup.
 *
 * Modifications for tup:
 * - Added headerless sized API (jp_alloc_sized/jp_free_sized/jp_realloc_sized)
 *   with in-band freelist links and a 16-byte pool floor, so callers that
 *   know the size at free time skip the per-block header entirely.
 * - Added jp_alloc_reset() for valgrind cleanup compatibility.
 * - Guarded with #ifndef _WIN32; Windows uses libc malloc via jp_alloc.h.
 */
#ifndef _WIN32

#include <cstring>
#include <cstddef>
#include <atomic>
#include <new>
#include <bit>

#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

#ifdef DEBUG
#include <iostream>
#include <fstream>
#include <iomanip>
#endif

#ifdef __GNUC__
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef JP_ALLOC_POOL_COUNT
#define JP_ALLOC_POOL_COUNT 17  // Gives pools of 1 - 64K
#endif

namespace {

size_t os_page_size()
{
   return sysconf(_SC_PAGESIZE);
}

void *os_alloc_pages(size_t size)
{
	void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) mem = nullptr;
        return mem;
}

void os_free_pages(void *mem, size_t size)
{
   munmap(mem, size);
}

/* ---- Header'd path (for malloc/free override where size is unknown) ---- */

union header
{
	struct {
		size_t size; // In principle, only size needs to be outside user area
		header *next;
	} s;
        std::max_align_t _align;
};

struct pool
{

#ifdef DEBUG
   struct
   {
      std::atomic<size_t> alloc_calls;
      std::atomic<size_t> alloc_count;
      std::atomic<size_t> free_count;
   } stat = {};
#endif

   std::atomic<header*> head;
};

#ifdef DEBUG
struct {
      std::atomic<size_t> jp_alloc;
      std::atomic<size_t> jp_alloc_aligned;
      std::atomic<size_t> jp_realloc_expand;
      std::atomic<size_t> jp_realloc_relocate;
      std::atomic<size_t> mallopt;
} stat = {};
#endif

pool g_pools[JP_ALLOC_POOL_COUNT] = {};
pool *g_pools_last = g_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Headerless sized path (caller knows size at free time) ---- */

struct sized_pool
{
   std::atomic<void*> head;
};

sized_pool g_sized_pools[JP_ALLOC_POOL_COUNT] = {};
sized_pool *g_sized_pools_last = g_sized_pools + JP_ALLOC_POOL_COUNT - 1;

/* ---- Header'd pool operations ---- */

void pool_put(header *h, pool *p)
{
#ifdef DEBUG
   p->stat.alloc_count--;
   p->stat.free_count++;
#endif
   header *expected = p->head;
   do h->s.next = expected;
   while (!p->head.compare_exchange_weak(expected, h));
}

void *pool_get(pool *p)
{
   header *expected = p->head;
   if (likely(expected != nullptr)) {
      // Normal case. Grap memory from pool
      header *next;
      do next = expected->s.next;
      while (!p->head.compare_exchange_weak(expected, next) && expected != nullptr);
   }
   if (unlikely(expected == nullptr)) {
      // Current pool was empty
      if (p == g_pools_last) {
         // Last pool. Ask OS for memory
         constexpr size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
         expected = static_cast<header*>(os_alloc_pages(sz));
         if (likely(expected != nullptr)) {
            expected->s.size = JP_ALLOC_POOL_COUNT - 1;
#ifdef DEBUG
            p->stat.alloc_count++;
#endif
         }
      }
      else {
         // Get from next pool and split
         char *mem = static_cast<char*>(pool_get(p + 1));
         if (mem != nullptr) {
            expected = reinterpret_cast<header*>(mem);
            size_t sz = expected->s.size - 1;
            header *spare = reinterpret_cast<header*>(mem + (1U << sz));
            expected->s.size = sz;
            spare->s.size = sz;
#ifdef DEBUG
            // one p+1 allocation becomes two allocated in p
            (p+1)->stat.alloc_count--;
            p->stat.alloc_count += 2;
#endif
            pool_put(spare, p);
         }
      }
   }
#ifdef DEBUG
   else {
      p->stat.alloc_count++;
      p->stat.free_count--;
   }
   p->stat.alloc_calls++;
#endif
   return expected;
}

size_t pool_id(size_t size)
{
   // sz   -> pool id
   // 1    -> 0 (size 1)
   // 2    -> 1 (size 2)
   // 3..4 -> 2 (size 4)
   // 5..8 -> 3 (size 8)
   // ...
   if (likely(size > 0)) return std::bit_width(size - 1);
   return 0;
}

bool is_pow2(size_t n)
{
   // test if n is power of 2. true for 0 also
   return (n & (n - 1)) == 0;
}

/* ---- Headerless sized pool operations ---- */

/* Floor at 16 bytes so the in-band freelist link (void*) fits
 * and every block is 16-byte aligned. */
size_t pool_id_sized(size_t size)
{
   if (size < 16) size = 16;
   return std::bit_width(size - 1);
}

void sized_pool_put(void *mem, sized_pool *p)
{
   /* In-band: store the freelist link in the first word of the
    * freed block itself (the block is not in use while on the
    * freelist, so we can reuse its memory). */
   void *expected = p->head.load();
   do *(void**)mem = expected;
   while (!p->head.compare_exchange_weak(expected, mem));
}

void *sized_pool_get(size_t pid)
{
   sized_pool *p = g_sized_pools + pid;
   void *expected = p->head.load();
   if (likely(expected != nullptr)) {
      void *next;
      do next = *(void**)expected;
      while (!p->head.compare_exchange_weak(expected, next) && expected != nullptr);
   }
   if (unlikely(expected == nullptr)) {
      if (p == g_sized_pools_last) {
         // Last pool: ask OS for a fresh 64K span
         constexpr size_t sz = 1U << (JP_ALLOC_POOL_COUNT - 1);
         expected = os_alloc_pages(sz);
      }
      else {
         // Get from next pool and split into buddies
         char *mem = static_cast<char*>(sized_pool_get(pid + 1));
         if (mem != nullptr) {
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

void *alloc_pages_aligned(size_t alignment, size_t size)
{
   if (unlikely(!is_pow2(alignment))) return nullptr;
   const size_t ps = os_page_size();
   size_t pre_padding = 0; // space needed before header to align final pointer
   size_t align_size = 0; // extra space needed to ensure we can get correct alignment in span
   if (alignment > ps) {
      pre_padding = ps - sizeof(header);
      align_size = alignment - ps; // need alignment pages - 1 to ensure alignment
   }
   else if (alignment > sizeof(header)) {
      pre_padding = alignment - sizeof(header);
   }
   else {
      alignment = sizeof(header); // minimum alignment.
   }
   size_t span_size = pre_padding + size + align_size;
   size_t span_size_rounded = (span_size + ps - 1) & ~(ps - 1); // round to whole pages
   char *span = static_cast<char*>(os_alloc_pages(span_size_rounded));
   if (unlikely(span == nullptr)) return nullptr;
   char *hdr = span + pre_padding;
   size_t offset = (alignment - (reinterpret_cast<size_t>(hdr + sizeof(header)) & (alignment - 1))) & (alignment - 1);
   hdr += offset;
   if (align_size > 0) {
      // if we have align pages, offset will be in whole pages (alignment > page size)
      // free pre and post align pages
      size_t pre_size = offset;
      size_t post_size = align_size - pre_size;
      if (pre_size > 0) os_free_pages(span, pre_size);
      if (post_size > 0) os_free_pages(span + span_size_rounded - post_size, post_size);
   }
   return hdr;
}


} // namespace

#ifdef DEBUG

void jpalloc_print_stats()
{
   std::ofstream out(std::string("/tmp/jpalloc.log-") + std::to_string(getpid()));
   out << "page size.............: " << os_page_size() << std::endl;
   out << "pool count............: " << JP_ALLOC_POOL_COUNT << std::endl;
   out << "jp_alloc..............: " << stat.jp_alloc << std::endl;
   out << "jp_alloc_aligned......: " << stat.jp_alloc_aligned << std::endl;
   out << "jp_realloc (expand)...: " << stat.jp_realloc_expand << std::endl;
   out << "jp_realloc (relocate).: " << stat.jp_realloc_relocate << std::endl;
   out << "mallopt...............: " << stat.mallopt << std::endl;
   out << std::endl;
   out << "id       size   alloc_calls     cur_alloc      cur_free" << std::endl;
   for (size_t i = 0; i < JP_ALLOC_POOL_COUNT; ++i) {
      out << std::setw(2) << i << ':'
         << std::setw(10) << (1<<i)
         << std::setw(14) << g_pools[i].stat.alloc_calls
         << std::setw(14) << g_pools[i].stat.alloc_count
         << std::setw(14) << g_pools[i].stat.free_count
         << std::endl;
   }
}
#endif

size_t jp_good_size(size_t size)
{
   size_t pid = pool_id(size);
   if (likely(pid < JP_ALLOC_POOL_COUNT)) {
      size = (1U << pid);
   }
   else {
      size_t ps_mask = os_page_size() - 1;
      size = (size + ps_mask) & ~ps_mask; // round to whole pages
   }
   return size - sizeof(header);
}

/* ---- Sized API (headerless) ---- */

extern "C" void *jp_alloc_sized(size_t size)
{
   size_t pid = pool_id_sized(size);
   if (likely(pid < JP_ALLOC_POOL_COUNT)) {
      return sized_pool_get(pid);
   }
   else {
      size_t ps_mask = os_page_size() - 1;
      size_t alloc_size = (size + ps_mask) & ~ps_mask;
      return os_alloc_pages(alloc_size);
   }
}

extern "C" void jp_free_sized(void *mem, size_t size)
{
   if (unlikely(mem == nullptr)) return;

   size_t pid = pool_id_sized(size);
   if (likely(pid < JP_ALLOC_POOL_COUNT)) {
      sized_pool_put(mem, g_sized_pools + pid);
   }
   else {
      size_t ps_mask = os_page_size() - 1;
      size_t alloc_size = (size + ps_mask) & ~ps_mask;
      os_free_pages(mem, alloc_size);
   }
}

extern "C" void *jp_realloc_sized(void *mem, size_t oldsz, size_t newsz)
{
   if (newsz == 0) {
      jp_free_sized(mem, oldsz);
      return nullptr;
   }
   if (mem == nullptr) {
      return jp_alloc_sized(newsz);
   }
   /* If both old and new fall in the same pool bucket, no copy needed */
   size_t old_pid = pool_id_sized(oldsz);
   size_t new_pid = pool_id_sized(newsz);
   if (old_pid == new_pid) {
      if (old_pid < JP_ALLOC_POOL_COUNT)
         return mem;		/* same pool bucket */
      /* both large: compare page-rounded sizes */
      size_t ps_mask = os_page_size() - 1;
      if (((oldsz + ps_mask) & ~ps_mask) == ((newsz + ps_mask) & ~ps_mask))
         return mem;
   }
   void *new_mem = jp_alloc_sized(newsz);
   if (new_mem) {
      size_t copy_size = oldsz < newsz ? oldsz : newsz;
      memcpy(new_mem, mem, copy_size);
      jp_free_sized(mem, oldsz);
   }
   return new_mem;
}

extern "C" void jp_alloc_reset(void)
{
   /* No-op: pool memory is still reachable from the global pool head
    * pointers, so valgrind reports it as "still reachable" (not an
    * error). The OS reclaims all mmap'd pages at process exit. This
    * replaces the old mempool_clear() call in tup_valgrind_cleanup(). */
}

/* ---- Header'd API (for malloc/free override) ---- */

void jp_free(void *mem)
{
   if (unlikely(mem == nullptr)) return;

   header *h = static_cast<header*>(mem) - 1;
   size_t size = h->s.size;
   if (likely(size < JP_ALLOC_POOL_COUNT)) {
      pool_put(h, g_pools + size);
   }
   else {
      size_t pre_padding = reinterpret_cast<size_t>(mem) & (os_page_size() - 1);
      os_free_pages(reinterpret_cast<char*>(h) + pre_padding, size + pre_padding);
   }
}

#ifdef DEBUG
static int _ae = std::atexit(jpalloc_print_stats);
#endif

void *jp_alloc(size_t size)
{
#ifdef DEBUG
   ++stat.jp_alloc;
#endif
   size += sizeof(header);
   void *mem;
   size_t pid = pool_id(size);
   if (likely(pid < JP_ALLOC_POOL_COUNT)) {
      mem = pool_get(g_pools + pid);
   }
   else {
      size_t ps_mask = os_page_size() - 1;
      size = (size + ps_mask) & ~ps_mask; // round to whole pages
      mem = os_alloc_pages(size);
      if (mem == nullptr) return nullptr;
      header *h = static_cast<header*>(mem);
      h->s.size = size;
   }

   return static_cast<header*>(mem) + 1;
}

void *jp_alloc_aligned(size_t alignment, size_t size)
{
#ifdef DEBUG
        ++stat.jp_alloc_aligned;
#endif
	size += sizeof(header);
	void *mem = alloc_pages_aligned(alignment, size);
        if (mem == nullptr) return nullptr;
        header *h = static_cast<header*>(mem);
        h->s.size = size;
	return static_cast<header*>(mem) + 1;
}

void *jp_calloc(size_t num, size_t nsize)
{
   size_t size = num * nsize;

   /* check mul overflow */
   if (num && nsize != size / num)  {
	   errno = ENOMEM;
	   return nullptr;
   }

   void *mem = jp_alloc(size);
   if (mem) memset(mem, 0, size);
   return mem;
}


void *jp_realloc(void *mem, size_t new_size)
{
        size_t size = 0;
        if (mem != nullptr) {
           header *h = static_cast<header*>(mem) - 1;
           size = h->s.size;
           if (size < JP_ALLOC_POOL_COUNT) size = 1U << size;
           size -= sizeof(header);
        }
        if (new_size > size) {
           /* Try mremap for large (mmap'd) allocations to avoid copying */
           if (mem != nullptr) {
              header *h = static_cast<header*>(mem) - 1;
              size_t hdr_size = h->s.size;
              if (hdr_size >= JP_ALLOC_POOL_COUNT) {
                 size_t pre_padding = reinterpret_cast<size_t>(mem) & (os_page_size() - 1);
                 char *base = reinterpret_cast<char*>(h) + pre_padding;
#ifdef __linux__
                 size_t new_total = new_size + sizeof(header) + pre_padding;
                 size_t ps_mask = os_page_size() - 1;
                 new_total = (new_total + ps_mask) & ~ps_mask;
                 void *new_base = mremap(base, hdr_size + pre_padding, new_total, MREMAP_MAYMOVE);
                 if (new_base != MAP_FAILED) {
                    header *new_h = reinterpret_cast<header*>(static_cast<char*>(new_base) + pre_padding);
                    new_h->s.size = new_total;
                    return new_h + 1;
                 }
#endif
              }
           }
#ifdef DEBUG
           ++stat.jp_realloc_relocate;
#endif
           void *new_mem = jp_alloc(new_size);
           if (new_mem) memcpy(new_mem, mem, size);
           jp_free(mem);
           mem = new_mem;
        }
        else if (new_size == 0) {
#ifdef DEBUG
           ++stat.jp_realloc_expand;
#endif
           jp_free(mem);
           mem = nullptr;
        }
#ifdef DEBUG
        else {
           ++stat.jp_realloc_expand;
        }
#endif
	return mem;
}

extern "C" void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
   size_t total_size = nmemb * size;

   // check mul overflow
   if (nmemb && size != total_size / nmemb) {
	   errno = ENOMEM;
	   return nullptr;
   }
   return jp_realloc(ptr, total_size);
}


extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *mem = jp_alloc_aligned(alignment, size);
	if (mem == nullptr) return ENOMEM;
	*memptr = mem;
	return 0;
}


extern "C" size_t malloc_usable_size (void *ptr)
{
	header *h = reinterpret_cast<header*>(ptr) - 1;
        size_t size = h->s.size;
        if (size < JP_ALLOC_POOL_COUNT) size = 1U << size;
	return size - sizeof(header);
}


extern "C" int mallopt(int param, int value)
{
	(void)param;
	(void)value;
	return 0;
}

/* ---- libc malloc/free/calloc/realloc overrides ---- */

extern "C" void free(void *mem) { jp_free(mem); }
extern "C" void cfree(void *mem) { jp_free(mem); }
extern "C" void *malloc(size_t size) { return jp_alloc(size); }
extern "C" void *calloc(size_t num, size_t nsize) { return jp_calloc(num, nsize); }
extern "C" void *valloc(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
extern "C" void *memalign(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }
extern "C" void *pvalloc(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
extern "C" void *realloc(void *mem, size_t new_size) { return jp_realloc(mem, new_size); }
extern "C" void *aligned_alloc(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }
extern "C" size_t malloc_size (void *ptr) { return malloc_usable_size(ptr); }
extern "C" size_t malloc_good_size(size_t size) { return jp_good_size(size); }

// libc symbols
extern "C" void* __libc_malloc(size_t size) { return malloc(size); }
extern "C" void __libc_free(void* ptr) { jp_free(ptr); }
extern "C" void* __libc_realloc(void* ptr, size_t size) { return jp_realloc(ptr, size); }
extern "C" void* __libc_calloc(size_t n, size_t size) { return jp_calloc(n, size); }
extern "C" void __libc_cfree(void* ptr) { jp_free(ptr); }
extern "C" void* __libc_memalign(size_t align, size_t s) { return jp_alloc_aligned(align, s); }
extern "C" void* __libc_valloc(size_t size) { return malloc(size); }
extern "C" void* __libc_pvalloc(size_t size) { return malloc(size); }
extern "C" int __posix_memalign(void** r, size_t a, size_t s) { return posix_memalign(r, a, s); }


void * operator new(std::size_t n) { return malloc(n); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return malloc(size); }
void operator delete(void * p) noexcept { jp_free(p); }
void *operator new[](std::size_t s) { return malloc(s); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return malloc(size); }
void operator delete[](void *p) noexcept { jp_free(p); }

// c++14
void operator delete(void* p, size_t) noexcept { jp_free(p); }
void operator delete[](void* p, size_t) noexcept { jp_free(p); }


// aligned new, delete
#if 0 // c++17

namespace std { enum class align_val_t : std::size_t {}; } // todo

void* operator new(size_t size, std::align_val_t al) { return jp_alloc_aligned((size_t)al, size); }
void operator delete(void* p, std::align_val_t al) noexcept { jp_free(p); }
void* operator new[](size_t size, std::align_val_t al) { return jp_alloc_aligned((size_t)al, size); }
void operator delete[](void* p, std::align_val_t al) noexcept { jp_free(p); }
void* operator new(size_t size, std::align_val_t al, const std::nothrow_t& nt) noexcept { return jp_alloc_aligned((size_t)al, size); }
void* operator new[](size_t size, std::align_val_t al, const std::nothrow_t& nt) noexcept { return jp_alloc_aligned((size_t)al, size); }
void operator delete(void* ptr, std::align_val_t al, const std::nothrow_t& nt) noexcept { jp_free(ptr); }
void operator delete[](void* ptr, std::align_val_t al, const std::nothrow_t& nt) noexcept { jp_free(ptr); }
void operator delete(void* p, size_t s, std::align_val_t al) noexcept { jp_free(p); }
void operator delete[](void* p, size_t s, std::align_val_t al) noexcept { jp_free(p); }
#endif

#if 0
// replace jemalloc

//extern "C" void replace_init(const malloc_table_t *table) { return; }
extern "C" void *replace_malloc(size_t size) { return jp_alloc(size); }
extern "C" int replace_posix_memalign(void **ptr, size_t alignment, size_t size) { return posix_memalign(ptr, alignment, size); }
extern "C" void *replace_aligned_alloc(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }
extern "C" void *replace_calloc(size_t num, size_t size) { return jp_calloc(num, size); }
extern "C" void *replace_realloc(void *ptr, size_t size) { return jp_realloc(ptr, size); }
extern "C" void replace_free(void *ptr) { jp_free(ptr); }
extern "C" void *replace_memalign(size_t alignment, size_t size) { return jp_alloc_aligned(alignment, size); }
extern "C" void *replace_valloc(size_t size) { return jp_alloc_aligned(os_page_size(), size); }
extern "C" size_t replace_malloc_usable_size(void* ptr) { return malloc_usable_size(ptr); }
extern "C" size_t replace_malloc_good_size(size_t size) { return jp_good_size(size); }
//extern "C" void replace_jemalloc_stats(jemalloc_stats_t *stats) { return; }
extern "C" void replace_jemalloc_purge_freed_pages() { return; }
extern "C" void replace_jemalloc_free_dirty_pages() { return; }
#endif

#endif /* _WIN32 */
