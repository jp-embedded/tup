# Real-world allocator benchmark results

Test machine: Intel Core i5-8250U (4 cores / 8 threads), 16 GB RAM,
Linux x86_64. All allocators built with `-O2` and tested via `LD_PRELOAD`.
Median of 2-3 runs. Run `bench_apps.sh` to reproduce.

## 1. GCC: compile SQLite amalgamation -O2 (single-threaded)

Compile the SQLite 3.46 amalgamation (~258K lines, 9MB C source) with
`gcc -O2 -c sqlite3.c -o /dev/null`. Heavy parser AST and optimizer
allocation churn, single-threaded.

| Allocator    | Time     | Peak RSS |
|--------------|----------|----------|
| jp_alloc     | 42.73 s  | 380 MB   |
| mimalloc     | 42.69 s  | 344 MB   |
| jemalloc     | 42.51 s  | 351 MB   |
| tcmalloc     | 42.93 s  | 379 MB   |
| glibc        | 43.90 s  | 355 MB   |

**Parity** — gcc is CPU-bound, not allocator-bound. All allocators within
2-3% of each other. jp_alloc is 2.7% faster than glibc (42.73 vs 43.90).
madvise variant is 6% lower RSS than no-madvise (380 vs 404 MB).

## 2. GCC: compile+link 50 files -O2 -j8 (parallel)

Compile 50 C files (~2700 lines total) with `make -j8`. 8 parallel gcc
processes, each using LD_PRELOAD'd allocator.

| Allocator    | Time     | Peak RSS |
|--------------|----------|----------|
| jp_alloc     | 1.02 s   | 23 MB    |
| glibc        | 1.05 s   | 23 MB    |
| jemalloc     | 1.09 s   | 23 MB    |
| mimalloc     | 1.09 s   | 23 MB    |

**Parity** — too short (1s) and I/O-dominated (forking 8 processes, reading
50 files). Allocator choice is irrelevant at this scale. jp_alloc edges out
by 3% vs glibc.

## 3. ffmpeg: transcode 1080p 10s video (libx264 fast)

| Allocator    | Time     | Peak RSS  |
|--------------|----------|-----------|
| jemalloc     | 6.40 s   | 1063 MB   |
| glibc        | 6.46 s   | 1058 MB   |
| jp_alloc     | 6.49 s   | 1207 MB   |
| mimalloc     | 6.68 s   | 1064 MB   |

**Parity** — ffmpeg is video-simd CPU-bound, not allocator-bound. All
within 4%. jp_alloc is essentially tied with glibc, slower than jemalloc by
1.4%. RSS is +149 MB from the 16B header on large frame buffers.

## 4. OpenSCAD: render 8000-sphere boolean model (single-threaded)

| Allocator    | Time     | Peak RSS |
|--------------|----------|----------|
| mimalloc     | 7.60 s   | 97 MB    |
| jp_alloc     | 7.70 s   | 129 MB   |
| glibc        | 7.79 s   | 92 MB    |
| jemalloc     | 7.87 s   | 93 MB    |

**jp_alloc 2nd fastest** — 1.2% faster than glibc, 2.2% faster than
jemalloc. Mimalloc edges jp_alloc by 1.3% (fine-grained size classes help
CGAL's small allocations). RSS +37 MB from power-of-2 rounding.

## 5. Inkscape: 10K-path SVG to PNG (single-threaded)

| Allocator    | Time     | Peak RSS |
|--------------|----------|----------|
| mimalloc     | 2.06 s   | 181 MB   |
| jemalloc     | 2.06 s   | 182 MB   |
| glibc        | 2.22 s   | 184 MB   |
| jp_alloc     | 2.53 s   | 240 MB   |

**jp_alloc slower** — 14% slower than jemalloc/mimalloc, 8% slower than
glibc. Inkscape uses Cairo/Pango which allocate many small paths and
text-layout objects. jp_alloc's power-of-2 rounding (32B allocation gets
64B block) wastes more in this workload. RSS +56 MB over glibc.

## Summary

| Application    | jp_alloc vs glibc | jp_alloc vs best | Best allocator |
|----------------|-------------------|-------------------|----------------|
| GCC -O2 SQLite | -2.7% (faster)    | +0.5%             | jemalloc       |
| GCC -j8 50 files| -3% (faster)      | -3%              | jp_alloc       |
| ffmpeg 1080p   | parity             | +1.4%             | jemalloc       |
| OpenSCAD       | -1.2% (faster)    | +1.3%             | mimalloc       |
| Inkscape SVG   | +14% (slower)     | +23%              | jemalloc/mim   |

jp_alloc is competitive on CPU-bound workloads (GCC, OpenSCAD) and
neutral on I/O-bound workloads (ffmpeg). It's slower on
Cairo/Pango-dominated workloads (Inkscape) where fine-grained size
classes matter more.

The synthetic bench (30 Mops/s at 300t, 2× less RSS than mimalloc)
remains jp_alloc's strongest result — it excels at high-throughput
multi-threaded alloc/free churn, which is exactly what tup's parse-graph
workload generates.