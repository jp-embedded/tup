# jp_alloc real-world benchmark conclusions

Date: 2026-08-15
Test machine: Intel Core i5-8250U (4 cores / 8 threads), 12 GB RAM, Linux x86_64.
Full results in JP_ALLOC_REALWORLD_BENCH.md. Run bench_apps.sh to reproduce.

## 8 applications tested, 6 allocators, median of 3 runs each

### jp_alloc wins

| Application | jp_alloc vs glibc | Why |
|---|---|---|
| GCC SQLite -O2 (single-thread) | 8.4% faster (best overall) | TLS cache + magazine memcpy excel on compiler AST alloc churn |
| OpenSCAD 8000 spheres (single-thread) | 5.4% faster (best overall) | CGAL mesh allocation pattern matches cache well |
| GCC 8x SQLite -j8 (parallel) | parity (~0.3% faster) | Multi-process, allocator not bottleneck |

### jp_alloc parity

| Application | vs glibc | Why |
|---|---|---|
| ffmpeg 1080p transcode | 0.4% slower (noise) | SIMD CPU-bound, frame buffers are large (allocator irrelevant) |
| NumPy SVD 2000x2000 | 1% faster (noise) | BLAS CPU-bound, few large allocations |
| GCC -j8 parallel compile | 0.3% faster (noise) | 8 CPU-bound processes, forking dominates |

### jp_alloc loses

| Application | vs glibc | vs best | Root cause |
|---|---|---|---|
| Inkscape SVG 10K paths | 5% slower | 9% vs mimalloc | Cairo/Pango allocate many 24-48B text objects → 50-167% waste from power-of-2 rounding (32B→64B block) |
| Blender 1000 spheres | 74% slower | 84% vs jemalloc | Python PyObjects (~24B) rounded to 64B = 167% waste × thousands of objects → 9.3 GB RSS → swap thrashing |
| SQLite3 CLI 2M rows | parity (time) but +91% RSS | +8% vs jemalloc | B-tree pages vary in size; 16B header + power-of-2 rounding doubles storage of 200B blobs (→256B block) |

### The losing pattern

All three losing workloads share a common characteristic: they allocate
many objects in the 20-60 byte range where power-of-2 size classes waste
50-167%:

- Python PyObjects: ~24 bytes each → pool 5 (32B block) = 33% waste, or
  pool 6 (64B block) = 167% waste depending on exact size
- Cairo text layout objects: 24-48 bytes → pool 5-6 = 33-167% waste
- SQLite B-tree pages: variable 200-400 bytes → 16B header pushes to
  next pool class, doubling the block size

### Why jp_alloc wins on other workloads

GCC and OpenSCAD allocate medium-sized objects (128B-512B) where:
- The 16B header is proportionally small (16/128 = 12.5% vs 16/32 = 50%)
- The TLS cache + magazine memcpy path is faster than linked-list walks
- The single-threaded fast path (__libc_single_threaded skipping EBR) helps

### The fix path

Adding intermediate size classes between the power-of-2 ladder would
address all three losing workloads:

Current: 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1K, 2K, ...
Proposed: 1, 2, 4, 8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192,
          224, 256, 320, 384, 448, 512, 1K, 2K, ...

This is the jemalloc size-class approach. Adding 48B would fix
Python's PyObject, 80B would fix many Cairo objects, and the finer
classes between 128B and 512B would help SQLite B-tree pages.

The trade-off: more size classes mean more pool arrays (memory),
more magazine lists per pool (more magazines in flight), and more
cache misses (smaller per-class caches). For the synthetic 300-thread
bench, this would slightly reduce throughput (more pools to refill).
For the real-world workloads, it would dramatically reduce RSS and
improve throughput on Python/Cairo workloads.

### Recommendation

1. Keep the current power-of-2 design for the upstream jp_alloc
   (simplest, best on synthetic bench, best on compilation)
2. Document the findings: jp_alloc excels on compilation and
   computational geometry, loses on Python/Cairo/SQLite workloads
3. Future work: intermediate size classes (48B, 80B, 96B, 112B)
   as an optional configuration, not default
4. The big tup project test (next week) is the ultimate validation:
   tup's parse-graph workload allocates medium-sized structs (80-256B)
   which is exactly where jp_alloc performs best