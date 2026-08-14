# jp_alloc magazine design investigation

## Summary

Experiments with replacing the global freelist's linked-list-of-blocks design
with a magazine-based design (arrays of block pointers transferred via memcpy).
The magazine design is architecturally cleaner but has higher RSS at 300 threads.

## Two designs compared

### Old linked-list design (branch: jp_alloc, commit ff97214d)
- Global freelist is a linked list of blocks, each block linked via in-band
  `h->s.next` pointer
- Refill: walk up to 16 `h->s.next` dependent loads + 1 CAS
- Flush: walk 16 cache slots writing `h->s.next` to build a chain + memmove + 1 CAS
- Zero container overhead — blocks are their own freelist entries
- 924 lines

### Magazine design (branch: jp_alloc_magazine)
- Global freelist is a linked list of magazines, each holding JP_MAG_SIZE=16
  block pointers in an array
- Refill: pop 1 magazine from global list (1 CAS) + memcpy 16 pointers to cache
- Flush: memcpy 16 pointers from cache to magazine + 1 CAS push
- Static global array of 4096 magazines (g_mag_array), CAS-based free-list
- No in-band pointer clobbering (blocks' next field never written by allocator)
- ~920 lines

## Performance comparison (release, memset bench, 50k ops/thread)

| Threads | Magazine Mops/s | Magazine RSS | Old Mops/s | Old RSS |
|---------|----------------|-------------|------------|---------|
| 1       | 5.36           | 2.1 MB      | 4.63       | 2.1 MB  |
| 8       | 18.79          | 3.9 MB      | 18.62      | 3.9 MB  |
| 64      | 27.56          | 17.3 MB     | 28.16      | 13.1 MB |
| 300     | 27.04          | 81.7 MB     | 27.26      | 35.2 MB |

Key findings:
- At 1 thread: magazine is 16% faster (memcpy vs dependent-load walk)
- At 8 threads: parity (~18.7 Mops/s both)
- At 64+ threads: throughput parity (~27 Mops/s both)
- At 300 threads: RSS is 2.3x higher (82 MB vs 35 MB)

## Earlier results with JP_MAG_SIZE=32 (full cache flush)

Before changing to JP_MAG_SIZE=16 (half-flush), the magazine design showed
higher throughput at 300 threads:

| Setting              | 300t Mops/s | 300t RSS |
|----------------------|-------------|----------|
| Mag size=32          | 30-31       | 83 MB    |
| Mag size=16 (current)| 27          | 82 MB    |
| Old linked-list      | 27          | 35 MB    |

The +9-13% throughput at mag_size=32 came from filling the entire cache in one
memcpy (32 blocks per refill, cache oscillates 1-32). At mag_size=16, the
cache oscillates 17-32 (matching the old design's half-flush), which gives
parity on throughput but the same RSS overhead.

## Why the RSS is higher

### Confirmed: NOT cache misses
Both designs show 100% cache hit rate at 300 threads (debug counters):
- Magazine: hits=183,750,068  misses=12,185
- Old:      hits=183,749,502  misses=9,373
The miss counts are nearly identical. The RSS difference is not from extra
buddy-splits or pool mmaps.

### Confirmed: NOT the magazine array itself
- g_mag_array[4096] x ~144 bytes = 576 KB static BSS
- mag_init() touches all 4096 entries during initialization (576 KB RSS)
- In-flight magazines at 300 threads: ~4500 x 144 bytes = 648 KB
- Total magazine overhead: ~1.2 MB — far less than the 46 MB delta

### Current hypothesis: EBR retire pipeline latency
Both designs defer retired blocks for 2 epoch advances before returning them
to the global freelist. With 300 threads, epoch advances are slow (all threads
must sync). The old design drains a chain of 16 blocks directly onto the
global freelist (immediately poppable by any thread via batched refill). The
magazine design drains a magazine container (16 block pointers) onto the
global magazine list — one pop gives 16 blocks, but the magazine must be
popped first. If multiple magazines accumulate on the global list, blocks
in deeper magazines are available but not consumed until shallower magazines
are popped. This may cause more blocks to accumulate "in flight" in the
retire pipeline, requiring more pool block allocations overall.

However, both designs have 100% cache hit rate and similar miss counts, so
the mechanism is subtle and not fully understood.

### What would help diagnose further
- Add pool mmap and split counters to the RELEASE build (not just debug)
- Compare the total number of pool blocks ever allocated between the two
  designs at 300 threads
- Trace the number of magazines on the global lists vs in the retire pipeline

## Segfault history and fix

### Root cause: thread-unsafe mag_init()
The initial magazine designs crashed (49/50 at 300 threads) because
`mag_init()` used a plain non-atomic read of `g_mag_initialized` to check
if initialization was done. Two threads calling `mag_alloc()` during
glibc's `__GI___ctype_init` (which calls malloc → our pool_get/pool_put →
mag_alloc) both saw `g_mag_initialized == 0` and both wrote to
`g_mag_array[i].next`, corrupting the linked list.

### Fix: CAS-based mag_init()
`mag_init()` now uses `__atomic_compare_exchange_n` to atomically claim
the initialization. Only one thread writes the linked list; others
spin-wait. Result: 0/50 crashes.

### Other issues fixed during investigation
1. _Thread_local dynamic TLS destructor ordering: glibc may free the
   large _Thread_local struct before the pthread_key destructor runs.
   Tried heap-allocated tls_p (pointer in static TLS) but that triggered
   a different crash in __GI___ctype_init because os_alloc_pages (mmap)
   was called during glibc's thread startup. Reverted to _Thread_local
   struct (works because glibc allocates dynamic TLS before __ctype_init).

2. Magazine bump allocator race: the first design used a non-atomic bump
   pointer for magazine allocation from mmap'd chunks. Multiple threads
   racing on the bump pointer caused segfaults. Replaced with a static
   global array (no mmap, no bump pointer, no race).

3. TLS destructor mag_alloc() race: calling mag_alloc() from within
   tls_destructor (which runs during thread teardown) could race with
   other threads using mag_alloc(). Fixed by not calling mag_alloc()
   from the destructor — if no empty magazine is available in TLS,
   the destructor discards the cached blocks (OS reclaims at exit).

## Reproduction script

`repro_segfault.sh` in the repo root triggers the segfault deterministically
(without the fix) by running the bench with output redirected to /dev/null
at 64 threads. The key trigger: stdout/stderr redirection changes glibc's
initialization path, causing __GI___ctype_init to call malloc during thread
startup, which races on mag_init().

## Code architecture (magazine design)

```
g_mag_array[4096]         — static array of magazine structs
g_mag_free                — CAS-based free-list of magazines from the array
g_pools[24].head          — per-pool magazine list (CAS-based)
tls.mag_empty             — per-thread stack of empty magazines (unified across pools)
tls.cache[24][32]         — per-thread per-pool block cache
tls.retired[24][3]        — per-thread per-pool per-epoch retired magazines

pool_get:
  1. cache hit → return cache[--cnt]
  2. cache miss → pop magazine from g_pools[pid].head (1 CAS)
  3. memcpy 16 pointers from magazine to cache
  4. push empty magazine to tls.mag_empty
  5. if no magazine → binary buddy split from pool N+1

pool_put:
  1. cache not full → cache[cnt++] = block
  2. cache full → pop empty magazine from tls.mag_empty (or mag_alloc)
  3. memcpy 16 pointers from cache to magazine
  4. memmove remaining 16 cache entries down
  5. retire magazine to EBR slot
```

## Conclusions

1. The magazine design is architecturally cleaner (memcpy vs chain-walking,
   no in-band pointer clobbering) and slightly faster at 1 thread (+16%).
2. At 64+ threads, throughput is at parity with the old design.
3. At 300 threads, RSS is 2.3x higher — the root cause is not fully
   understood despite 100% cache hit rate in both designs.
4. The segfault was caused by a thread-unsafe initialization of the
   magazine free-list and is fully fixed with a CAS-based guard.
5. For tup's typical workload (1-32 threads, I/O-bound parse+update),
   both designs perform identically. The RSS difference only appears
   at 300 threads, which tup never uses in practice.
6. For the upstream jp_alloc repo (general-purpose LD_PRELOAD allocator),
   the magazine design is the better choice (cleaner code, competitive
   throughput, no in-band pointer clobbering).

## Next steps for investigation

- Compare total pool block allocations (mmap+split counts) between
  designs at 300 threads in release builds
- Test JP_MAG_SIZE=32 with the static array design (was previously only
  tested with the bump allocator that had race conditions)
- Measure the number of magazines on global lists vs in the retire
  pipeline to understand the RSS accumulation
- Consider a hybrid approach: use in-band linked-list for the EBR retire
  path (zero container overhead) but magazines for the active global
  freelist (memcpy refill/flush)