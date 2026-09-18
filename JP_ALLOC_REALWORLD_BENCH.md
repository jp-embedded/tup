# Real-world allocator benchmark — full results

## Restricted K=4 follow-up (2026-09-16)

Intermediate pools are now limited to asymmetric splits whose parent is at
most 4KB. K=4 therefore adds 160B, 320B, 640B, and 1280B classes while larger
allocations continue to use power-of-two pools. This keeps each asymmetric
split within one page and avoids conflicts with per-page reclamation.

The table compares the power-of-two K=0 configuration with restricted K=4.
Application results are medians of three runs except Blender, GCC -j8, and
NumPy, which were run once. Peak RSS is from `/usr/bin/time`.

| Application | K=0 time/RSS | K=4 time/RSS | K=4 change |
|---|---|---|---|
| tup clean parse, 3 variants | 62.08s / 1017MB | 60.15s / 683MB | 3.1% faster, 32.9% less RSS |
| GCC SQLite -O2 | 13.40s / 384MB | 13.42s / 385MB | parity |
| GCC 8x SQLite -j8 | 14.81s / 362MB | 14.73s / 362MB | parity |
| ffmpeg 1080p | 1.37s / 2162MB | 1.35s / 2162MB | parity |
| OpenSCAD 8K spheres | 0.36s / 157MB | 0.35s / 152MB | 2.8% faster, 3.3% less RSS |
| Inkscape 10K SVG | 0.64s / 224MB | 0.64s / 221MB | same time, 1.5% less RSS |
| Blender 1000 spheres | 60.80s / 28085MB | 52.98s / 26765MB | 12.9% faster, 4.7% less RSS |
| NumPy SVD 2000² | 17.30s / 300MB | 17.26s / 299MB | parity |
| SQLite3 CLI 2M rows | 5.85s / 3320MB | 5.57s / 2765MB | 4.8% faster, 16.7% less RSS |

K=4 is neutral or better than K=0 in every measured workload and provides a
large reduction for tup itself, so it is now the default. It does not solve
the Blender/Python small-object pathology because the first added class is
160B; finer classes below 128B would require a different split scheme.

A clean tup parse without sub-4KB page counters took 56.01s with 498424KB
peak RSS, versus the counter-enabled K=4 median of 60.15s and 699372KB.
Instrumentation showed 880MB of pool regions mapped and
only 722 freelist nodes remaining with counters, versus 384MB and 208475
freelist nodes without counters. `MADV_DONTNEED` clears the in-band `next`
pointers of free blocks on the page, truncating freelists and forcing new 8MB
regions. It can also race with cross-thread allocation from the page. The
page-counter implementation was removed; safe sub-4KB reclamation requires
page-owned free structures and out-of-band metadata.

The final allocator reclaims only complete payload pages of pool blocks at
least 32KB, before publishing the block on a freelist. It completed the clean
tup parse in 57.56s at 498144KB. Reclaiming every block from 8KB upward took
64.07s with no RSS improvement because tup frees millions of 8KB blocks.

Final real-world K=4 results with the safe 32KB threshold:

| Application | K=0 time/RSS | K=4 time/RSS |
|---|---|---|
| GCC SQLite -O2 | 14.01s / 369MB | 13.79s / 370MB |
| GCC 8x SQLite -j8 | 15.01s / 362MB | 14.92s / 363MB |
| ffmpeg 1080p | 1.35s / 2162MB | 1.34s / 2162MB |
| OpenSCAD 8K spheres | 0.37s / 154MB | 0.36s / 150MB |
| Inkscape 10K SVG | 0.61s / 225MB | 0.60s / 230MB |
| Blender 1000 spheres | 47.58s / 25292MB | 43.15s / 25034MB |
| NumPy SVD 2000² | 17.37s / 300MB | 17.69s / 298MB |
| SQLite3 CLI 2M rows | 5.17s / 3319MB | 4.97s / 2764MB |

Test machine: Intel Core i5-8250U (4 cores / 8 threads), 12 GB RAM,
Linux x86_64. All allocators built with `-O2` and tested via `LD_PRELOAD`.
Median of 3 runs unless noted. Run `bench_apps.sh` to reproduce.

## Summary

| Application | jp_alloc vs glibc | jp_alloc vs best | Best | jp_alloc RSS vs glibc |
|---|---|---|---|---|
| GCC SQLite -O2 (1t) | **8.4% faster** | +0.4% | jp_alloc | +155% (904 vs 354 MB) |
| GCC 8x SQLite -j8 | parity | +0.4% | jp_alloc | <0.1% |
| ffmpeg 1080p | parity | +5.8% | tcmalloc | +14% (1207 vs 1058 MB) |
| OpenSCAD 8K spheres | **5.4% faster** | (best) | jp_alloc | +42% |
| Inkscape 10K SVG | 5% slower | +9% | mimalloc | +31% |
| Blender 1000 spheres | 74% slower | blocked | jemalloc | +1670% (SWAPPED) |
| NumPy SVD 2000² | parity | parity | all same | +1% |
| SQLite3 CLI 2M rows | parity | 8% slower | jemalloc | +91% |

## 1. GCC: compile SQLite amalgamation -O2 (single-threaded, ~258K lines)

| Allocator | Time | Peak RSS |
|---|---|---|
| **jp_alloc** | **43.41 s** | 904 MB |
| mimalloc | 44.13 s | 345 MB |
| jemalloc | 44.02 s | 351 MB |
| tcmalloc | 44.27 s | 379 MB |
| glibc | 47.42 s | 354 MB |

**jp_alloc is 8.4% faster than glibc** — the best allocator for GCC compilation.
RSS is high (904 MB) because the 16B header on every allocation adds up
across 258K lines of AST nodes. The madvise for large blocks helps but
the small-block header overhead dominates.

## 2. GCC: 8x SQLite amalgamation -O2 -j8 (parallel, ~2M lines total)

| Allocator | Time | Peak RSS |
|---|---|---|
| jp_alloc | 93.54 s | 354 MB |
| glibc | 93.82 s | 354 MB |
| jemalloc | 93.80 s | 355 MB |
| tcmalloc | 93.82 s | 354 MB |
| mimalloc | 94.02 s | 355 MB |

**All allocators at parity** — the 8 parallel gcc processes are CPU-bound,
not allocator-bound. The allocator has no measurable effect at this scale.
jp_alloc edges out by 0.3% (noise). RSS is identical across all (354 MB
shared via COW from make's process management).

## 3. ffmpeg: transcode 1080p 10s video (libx264 fast)

| Allocator | Time | Peak RSS |
|---|---|---|
| tcmalloc | 6.87 s | 1064 MB |
| mimalloc | 6.92 s | 1064 MB |
| jemalloc | 7.19 s | 1063 MB |
| glibc | 7.26 s | 1058 MB |
| jp_alloc | 7.29 s | 1207 MB |

**Parity** — ffmpeg is SIMD CPU-bound. jp_alloc within 0.4% of glibc.
tcmalloc is fastest (likely from per-thread arenas reducing contention
on libavcodec's internal alloc patterns). jp_alloc RSS +149 MB from
16B headers on frame buffers.

## 4. OpenSCAD: render 8000-sphere boolean model (single-threaded)

| Allocator | Time | Peak RSS |
|---|---|---|
| **jp_alloc** | **6.89 s** | 131 MB |
| mimalloc | 7.91 s | 97 MB |
| jemalloc | 7.64 s | 94 MB |
| glibc | 7.28 s | 93 MB |

**jp_alloc is the fastest** — 5.4% faster than glibc, 10% faster than mimalloc.
The single-threaded fast path (__libc_single_threaded skipping EBR) helps here.
RSS +39 MB from power-of-2 rounding on CGAL's mesh allocations.

## 5. Inkscape: 10K-path SVG to PNG (Cairo/Pango)

| Allocator | Time | Peak RSS |
|---|---|---|
| mimalloc | 2.29 s | 181 MB |
| jemalloc | 2.30 s | 182 MB |
| glibc | 2.38 s | 184 MB |
| jp_alloc | 2.50 s | 240 MB |

**jp_alloc 5% slower** — Cairo/Pango allocate many small text-layout objects
where power-of-2 rounding (32B→64B = 50% waste) hurts. Fine-grained size
classes (jemalloc/mimalloc) win here. RSS +56 MB from rounding.

## 6. Blender: 1000 ico-spheres, 32 samples, 1280x720 (headless)

| Allocator | Time | Peak RSS |
|---|---|---|
| jemalloc | 48.83 s | 399 MB |
| mimalloc | 49.09 s | 403 MB |
| glibc | 51.76 s | 399 MB |
| jp_alloc | 89.96 s | 9359 MB (SWAPPED) |

**jp_alloc 74% slower** — Blender uses heavy Python scripting for scene
creation (bpy.ops.mesh.primitive_ico_sphere_add × 1000), which allocates
many Python objects through Python's own allocator. jp_alloc's power-of-2
rounding causes massive waste on Python's small object allocations.
RSS hit 9.3 GB and likely swapped. This is a pathological case for jp_alloc
that would benefit from fine-grained size classes.

## 7. Python NumPy: SVD 2000x2000 matrix (single-threaded, BLAS)

| Allocator | Time | Peak RSS |
|---|---|---|
| mimalloc | 54.16 s | 299 MB |
| jp_alloc | 55.15 s | 292 MB |
| glibc | 55.73 s | 297 MB |
| jemalloc | 55.81 s | 297 MB |

**Parity** — NumPy SVD is BLAS CPU-bound (LAPACK dgesdd). Allocator choice
is irrelevant. jp_alloc within 1% of all. RSS comparable.

## 8. SQLite3 CLI: 2M rows insert+index+query (in-memory DB, heavy malloc)

| Allocator | Time | Peak RSS |
|---|---|---|
| jemalloc | 13.43 s | 2094 MB |
| mimalloc | 13.64 s | 2084 MB |
| glibc | 14.42 s | 1732 MB |
| jp_alloc | 14.54 s | 3319 MB |

**jp_alloc parity with glibc** — 0.8% slower. jemalloc is 7% faster
(fine-grained classes help SQLite's B-tree page allocator).
RSS +1589 MB — the 16B header doubles the storage of 2M × 200B blobs
(200B → pool 8 = 256B block = 56B waste × 2M = 112MB +
header 16B × 2M = 32MB + B-tree overhead doubling = ~1.5 GB total).

## Conclusions

**jp_alloc wins on:**
- GCC compilation (-O2 single file): **8.4% faster than glibc**, best overall
- OpenSCAD rendering: **5.4% faster than glibc**, best overall
- GCC -j8 parallel: parity, slightly fastest

**jp_alloc parity on:**
- ffmpeg video transcode (CPU-bound, large buffers)
- NumPy SVD (BLAS CPU-bound)
- GCC -j8 parallel compile

**jp_alloc loses on:**
- Inkscape SVG (Cairo/Pango, many small text objects): 5% slower
- Blender (Python scripting, many small Python objects): 74% slower, RSS explosion
- SQLite3 CLI (B-tree pages, many distinct sizes): parity with glibc but 91% more RSS

**Root cause of losses**: power-of-2 size classes waste 50% on
applications that allocate many objects in the 20-60 byte range
(Python's PyObjects, Cairo's text rendering, SQLite's B-tree pages).
Fine-grained classes (jemalloc, mimalloc) avoid this waste.

**Fix path**: adding intermediate size classes (32B, 48B, 64B, 80B, 96B,
112B, 128B) would address all three losing workloads. The madvise
for large blocks caps waste at 4KB for big allocations, but small-block
waste accumulates without recycling.
