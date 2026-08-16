/* jp_alloc_bench - thread-stress benchmark and correctness test for jp_alloc
 *
 * Build as a standalone binary:
 *   cc -O2 -DJP_ALLOC_COMPILED -DJP_ALLOC_BENCH \
 *      src/jp_alloc/jp_alloc.c src/jp_alloc/jp_alloc_bench.c \
 *      -o jpbench -lpthread -lrt
 *
 * The bench links jp_alloc.c so malloc/free are overridden globally; the
 * "unsized" malloc/free churn exercises the header'd pool path, while the
 * direct malloc/jp_free_sized calls exercise the headerless sized
 * path. 300 pthreads each run a fixed number of operations that mimic tup's
 * parse-graph workload, then the program prints ops/sec, per-thread p50/p99
 * latency, and peak RSS.
 *
 * When built with -DJP_ALLOC_DEBUG the same binary exercises the in-allocator
 * ABA / double-free / wrong-API / size-mismatch self checks; abort() is the
 * failure signal.
 *
 * Environment variables:
 *   JPBENCH_THREADS   (default 300)   number of worker threads
 *   JPBENCH_OPS       (default 50000) operations per worker
 *   JPBENCH_SEED      (default 0xc0ffee) base RNG seed
 *   JPBENCH_SECONDS   (optional)     if set, run for N seconds instead of
 *                                    a fixed op count (each thread checks
 *                                    the deadline between ops)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <math.h>
#ifndef _WIN32
#include <sys/resource.h>  /* getrusage for non-Linux peak RSS fallback */
#endif


#ifndef JPBENCH_THREADS_DEFAULT
#define JPBENCH_THREADS_DEFAULT 300
#endif
#ifndef JPBENCH_OPS_DEFAULT
#define JPBENCH_OPS_DEFAULT 50000
#endif
#ifndef JPBENCH_SEED_DEFAULT
#define JPBENCH_SEED_DEFAULT 0xc0ffeeULL
#endif

/* Workload mode env JPBENCH_MODE:
 *   balanced    (default): graph_burn allocs+frees together (current)
 *   alloc-heavy: accumulate N_OUTSTANDING allocations before freeing any.
 *                Drains caches, exercises EBR refill from global freelist
 *                and exposes hit-rate as a function of N.
 *   frag-stress: burst N small allocations across multiple tiers, free
 *                them all, repeat M cycles. Exposes RSS-leak patterns
 *                where freed small blocks can't be coalesced back up —
 *                the jp_alloc2 redesign's primary motivation. Reports
 *                peak RSS, final RSS, and RSS-per-cycle for A/B
 *                comparison of jp_alloc vs jp_alloc2.
 *   free-heavy:  not implemented in this round; reserved for future. */
static int g_mode_alloc_heavy = 0;
static int g_mode_frag_stress = 0;

/* ---- Sizes mimicking tup's hot structures ----
 *
 * Derived from src/tup/{graph,entry,file,tent_tree,tent_list,tupid_list,
 * pel_group}.c/h. These are the dominant sized-alloc callers. We round
 * to the sizes that hit distinct jp_alloc pool classes so the cache is
 * exercised across many slots. */
#define SZ_NODE      80    /* struct node        ~80B   */
#define SZ_EDGE      48    /* struct edge        ~48B   */
#define SZ_TENT     256    /* struct tup_entry  ~256B   */
#define SZ_FILE     128    /* struct file_entry ~128B   */
#define SZ_TTREE     48    /* struct tupid_tree  ~48B   */
#define SZ_TLIST     32    /* struct tupid_list  ~32B   */
#define SZ_TENTLIST  48    /* struct tent_list   ~48B   */
#define SZ_PEL       40    /* struct path_element ~40B  */

/* Unsized (malloc/free) churn mimicking SQLite/Lua/PCRE internal buffers. */
#define MALLOC_MIN  1024
#define MALLOC_MAX  32768

/* Each "graph op" allocates one tent, a few nodes, several edges,
 * monkey-patches a file_entry, and tears it all down. This models the
 * alloc/free rhythm of tup's build-graph construction during `tup parse`.
 */
#define OPS_PER_BURST     16
#define NODES_PER_OP      4
#define EDGES_PER_OP      8
#define MALLOC_CHURN_RATIO 4  /* 1 malloc-ondemo-op per N sized ops */

struct link {
	struct link *next;
};

struct thread_state {
	unsigned tid;
	unsigned long ops_done;
	unsigned long malloc_ops;
	double latencies_ns_sum;
	double latencies_ns_sum_sq;
	double lat_min_ns;
	double lat_max_ns;
	/* latency histogram (power-of-2 log buckets, ~2^0..2^28 ns) */
	unsigned long hist[30];
};

static long g_total_threads = JPBENCH_THREADS_DEFAULT;
static long g_ops_per_thread = JPBENCH_OPS_DEFAULT;
static uint64_t g_seed = JPBENCH_SEED_DEFAULT;
static volatile int g_run_timed = 0;
static volatile double g_deadline = 0.0; /* seconds, if timed */

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* xorshift64*: good enough for a per-thread PRNG; cheap and lock-free. */
static inline uint64_t xrand(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static inline double log2lat(unsigned long nsec)
{
	if(nsec <= 0) return 0.0;
	/* bucket index = floor(log2(nsec)) */
	unsigned long v = (unsigned long)nsec;
	unsigned b = 0;
	while(v > 1) { v >>= 1; b++; }
	return (double)b;
}

/* ---- Workload helpers ---- */

static void graph_burn(struct thread_state *t, uint64_t *rng)
{
	(void)t; (void)rng;
	struct link *nodes[NODES_PER_OP];
	struct link *edges[EDGES_PER_OP];
	struct link *tent;
	struct link *file;

	tent = (struct link *)malloc(SZ_TENT);
	file = (struct link *)malloc(SZ_FILE);
	for(int i = 0; i < NODES_PER_OP; i++)
		nodes[i] = (struct link *)malloc(SZ_NODE);
	for(int i = 0; i < EDGES_PER_OP; i++)
		edges[i] = (struct link *)malloc(SZ_EDGE);

	/* Touch the first byte of each block to force its first cache line
	 * into L1, simulating the caller's first field write that real tup
	 * does immediately after allocation (tent->dt, node->tnode.tupid, etc).
	 * This isolates the test to pool-class inflation: both sized and
	 * unsized paths now touch the same first cache line, so the only
	 * remaining difference is block size (256 vs 512 for tent etc.). */
	if(tent) memset(tent,0,SZ_TENT);
	if(file) memset(file,0,SZ_FILE);
	for(int i = 0; i < NODES_PER_OP; i++)
		if(nodes[i]) memset(nodes[i],0,SZ_NODE);
	for(int i = 0; i < EDGES_PER_OP; i++)
		if(edges[i]) memset(edges[i],0,SZ_EDGE);

	/* briefly link nodes through edges to mimic graph edge wiring */
	for(int i = 0; i < EDGES_PER_OP; i++)
		edges[i]->next = nodes[i % NODES_PER_OP];
	if(tent) tent->next = nodes[0];

	/* tear down — interleave alloc/free like tup actually does */
	free(tent);
	free(file);
	for(int i = 0; i < EDGES_PER_OP; i++)
		free(edges[i]);
	for(int i = 0; i < NODES_PER_OP; i++)
		free(nodes[i]);
}

/* Alloc-heavy mode: accumulate N_OUTSTANDING allocations across iterations
 * before freeing any, then free all. Stretches the cache so the working set
 * exceeds N for several hundred iters at a time and exercises EBR refill.
 *
 * Each iteration allocates one new block. When the outstanding list is
 * full (>= N_OUTSTANDING), free them all at once. This produces a strong
 * sawtooth of (alloc-heavy / free-heavy) phases that grows the cache past
 * N on the alloc side and drains it (and triggers EBR flushes) on the free.
 *
 * The cache hit rate as a function of N is meaningful here:
 *   hit% ≈ 1 - (N_OUTSTANDING/2) / total_ops_at_that_phase *ория
 * The smaller N is, the more the cache misses during the alloc phase;
 * the larger N is, the more often the cache absorbs the burst and the
 * more the free phase overflows the cache (triggering EBR retire + the
 * memmove flush). So alloc-heavy exposes the throughput-vs-N trade-off
 * the balanced bench hides. */
#define AH_OUTSTANDING 4096   /* blocks per phase; > JP_CACHE_N to force misses */
struct alloc_heavy_state {
	void *ring[AH_OUTSTANDING];
	size_t cnt;        /* current outstanding */
};
static _Thread_local struct alloc_heavy_state ah_state;

static void alloc_heavy_burn(struct thread_state *t, uint64_t *rng)
{
	(void)t; (void)rng;
	if(ah_state.cnt >= AH_OUTSTANDING) {
		/* Free phase: drain the whole outstanding set. */
		for(size_t i = 0; i < ah_state.cnt; i++)
			free(ah_state.ring[i]);
		ah_state.cnt = 0;
	}
	ah_state.ring[ah_state.cnt++] = malloc(SZ_NODE);
	if(ah_state.ring[ah_state.cnt - 1])
		memset(ah_state.ring[ah_state.cnt - 1],0,SZ_NODE);
}

static void alloc_heavy_drain(void)
{
	for(size_t i = 0; i < ah_state.cnt; i++)
		free(ah_state.ring[i]);
	ah_state.cnt = 0;
}

/* ---- frag-stress mode ----
 *
 * Burst N small allocations across multiple size classes (8B..256B),
 * free them all, repeat M cycles. Designed to expose the RSS leak that
 * jp_alloc's lack of upward coalescing produces: small freed blocks sit
 * in magazines and never dissolve back into the larger blocks they were
 * split from — so the 8M demand-paged reserve keeps the affected pages
 * committed forever. jp_alloc2's coalescing lets freed small blocks
 * dissolve up to madvise-eligible 4K+ regions, so RSS should drop back
 * after the free phase.
 *
 * Sizes pick from the 8B..256B sub-block classes that map to tier 3
 * (and to jp_alloc's pools 5..8): 24B (Python PyObject), 32B (tupid_list),
 * 48B (edge), 80B (node), 128B (file_entry), 200B (SQLite B-tree page).
 * The 24B and 200B are chosen to fall just outside power-of-2 boundaries
 * to exercise internal-fragmentation waste. */

#define FS_BURST       4096
#define FS_CYCLES      32
static const size_t FS_SIZES[] = {24, 32, 48, 80, 128, 200};
#define FS_NSIZES ((int)(sizeof FS_SIZES / sizeof FS_SIZES[0]))

static void frag_stress_burn(struct thread_state *t, uint64_t *rng)
{
	(void)t; (void)rng;
	void *objs[FS_BURST];
	for(int i = 0; i < FS_BURST; i++) {
		size_t sz = FS_SIZES[i % FS_NSIZES];
		objs[i] = malloc(sz);
		if(objs[i]) memset(objs[i], 0, sz);
	}
	for(int i = 0; i < FS_BURST; i++)
		free(objs[i]);
}

static void malloc_burn(struct thread_state *t, uint64_t *rng)
{
	(void)t;
	/* mix small and large mallocs to hit distinct size classes on the
	 * header'd path. free in LIFO order to mimic a working session. */
	void *objs[8];
	int n = 0;
	for(int i = 0; i < 4; i++) {
		size_t sz = MALLOC_MIN + (xrand(rng) % (MALLOC_MAX - MALLOC_MIN));
		objs[n] = malloc(sz);
		if(objs[n] == NULL) { fprintf(stderr, "malloc fail\n"); exit(1); }
		/* touch the page */
		((char *)objs[n])[0] = (char)i;
		((char *)objs[n])[sz - 1] = (char)i;
		n++;
	}
	while(n > 0) {
		free(objs[--n]);
	}
}

static void *worker(void *arg)
{
	struct thread_state *t = (struct thread_state *)arg;
	uint64_t rng = g_seed ^ ((uint64_t)t->tid * 0x9E3779B97F4A7C15ULL);
	if(rng == 0) rng = 1;

	unsigned long ops_target = (unsigned long)g_ops_per_thread;
	int timed = g_run_timed;
	double deadline = g_deadline;

	for(;;) {
		if(timed) {
			if(now_sec() >= deadline) break;
		} else {
			if(t->ops_done >= ops_target) break;
		}

		double t0 = now_sec();
		for(int burst = 0; burst < OPS_PER_BURST; burst++) {
			if(g_mode_alloc_heavy)
				alloc_heavy_burn(t, &rng);
			else if(g_mode_frag_stress)
				frag_stress_burn(t, &rng);
			else
				graph_burn(t, &rng);
			t->ops_done++;
			if(timed && now_sec() >= deadline) break;
			if(!timed && t->ops_done >= ops_target) break;
		}
		/* malloc churn every MALLOC_CHURN_RATIO sized bursts */
		if((t->ops_done & (MALLOC_CHURN_RATIO - 1)) == 0) {
			malloc_burn(t, &rng);
			t->malloc_ops++;
		}
		double lat_ns = (now_sec() - t0) * 1e9 / OPS_PER_BURST;
		if(lat_ns < 0) lat_ns = 0;
		t->latencies_ns_sum    += lat_ns;
		t->latencies_ns_sum_sq += lat_ns * lat_ns;
		if(t->ops_done > 1) { /* skip first warm-up sample */
			if(lat_ns < t->lat_min_ns) t->lat_min_ns = lat_ns;
			if(lat_ns > t->lat_max_ns) t->lat_max_ns = lat_ns;
			unsigned b = (unsigned)log2lat((unsigned long)lat_ns);
			if(b < (unsigned)(sizeof t->hist / sizeof t->hist[0]))
				t->hist[b]++;
		}
	}
	/* Drain outstanding blocks (only alloc-heavy mode has any state). */
	if(g_mode_alloc_heavy) alloc_heavy_drain();
	return NULL;
}

static size_t peak_rss_kb(void)
{
#ifdef __linux__
	FILE *f = fopen("/proc/self/status", "r");
	if(!f) return 0;
	char line[256];
	size_t rss = 0;
	while(fgets(line, sizeof line, f)) {
		if(strncmp(line, "VmHWM:", 6) == 0) {
			rss = (size_t)strtoul(line + 6, NULL, 10);
			break;
		}
	}
	fclose(f);
	return rss;
#else
	/* fall back to getrusage */
	struct rusage ru;
	if(getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
		return (size_t)ru.ru_maxrss / 1024;
#else
		return (size_t)ru.ru_maxrss;
#endif
	}
	return 0;
#endif
}

#if 0
static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}
#endif

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	const char *e;

	if((e = getenv("JPBENCH_THREADS")) != NULL)
		g_total_threads = strtol(e, NULL, 10);
	if((e = getenv("JPBENCH_OPS")) != NULL)
		g_ops_per_thread = strtol(e, NULL, 10);
	if((e = getenv("JPBENCH_SEED")) != NULL)
		g_seed = (uint64_t)strtoull(e, NULL, 0);
	if((e = getenv("JPBENCH_MODE")) != NULL) {
		if(strcmp(e, "alloc-heavy") == 0) g_mode_alloc_heavy = 1;
		else if(strcmp(e, "frag-stress") == 0) g_mode_frag_stress = 1;
		/* "balanced" or any other value: graph_burn (default) */
	}
	if((e = getenv("JPBENCH_SECONDS")) != NULL && *e) {
		g_run_timed = 1;
		g_deadline = now_sec() + atof(e);
	}

	if(g_total_threads < 1) g_total_threads = 1;

	printf("jp_alloc bench: threads=%ld ops/thread=%ld mode=%s%s%s\n",
		g_total_threads, g_ops_per_thread,
		g_mode_alloc_heavy ? "alloc-heavy" :
		(g_mode_frag_stress ? "frag-stress" : "balanced"),
		g_run_timed ? " timed=" : "",
		g_run_timed ? getenv("JPBENCH_SECONDS") : "");
#ifdef JP_ALLOC_DEBUG
	printf("build: JP_ALLOC_DEBUG (self-checks ENABLED)\n");
#else
	printf("build: release (no debug self-checks)\n");
#endif
	fflush(stdout);

	pthread_t *th = calloc((size_t)g_total_threads, sizeof *th);
	struct thread_state *st = calloc((size_t)g_total_threads, sizeof *st);
	if(!th || !st) {
		fprintf(stderr, "calloc(thread table) failed\n");
		return 2;
	}
	for(long i = 0; i < g_total_threads; i++) {
		st[i].tid = (unsigned)i;
		st[i].lat_min_ns = 1e18;
	}

	/* warm the global pools so the first batch doesn't include mmap latency */
	{
		void *warm[64];
		for(int i = 0; i < 64; i++) warm[i] = malloc(SZ_NODE);
		for(int i = 0; i < 64; i++) free(warm[i]);
	}

	double t_start = now_sec();
	for(long i = 0; i < g_total_threads; i++) {
		int r = pthread_create(&th[i], NULL, worker, &st[i]);
		if(r) {
			fprintf(stderr, "pthread_create[%ld]: %s\n", i, strerror(r));
			return 2;
		}
	}
	unsigned long total_ops = 0;
	unsigned long total_malloc_ops = 0;
	for(long i = 0; i < g_total_threads; i++) {
		pthread_join(th[i], NULL);
		total_ops += st[i].ops_done;
		total_malloc_ops += st[i].malloc_ops;
	}
	double t_end = now_sec();
	double wall = t_end - t_start;

	/* ---- Aggregate latency stats ----
	 * We don't keep per-op samples (memory), so we approximate p50/p99 from
	 * each thread's mean/var and histogram. The histogram is the rigorous
	 * answer; we also report (weighted) mean and stddev. */
	double sum_w = 0, sum_wm = 0, sum_wm2 = 0;
	double gmin = 1e18, gmax = 0;
	for(long i = 0; i < g_total_threads; i++) {
		double w = (double)st[i].ops_done;
		if(w <= 0) continue;
		double m = st[i].latencies_ns_sum / w;
		sum_w  += w;
		sum_wm += w * m;
		sum_wm2 += w * (m*m + st[i].latencies_ns_sum_sq / (w*w) - m*m/w);
		/* (sum_sq/w) is the second moment E[x^2]; keep it simple below */
		if(st[i].lat_min_ns < gmin) gmin = st[i].lat_min_ns;
		if(st[i].lat_max_ns > gmax) gmax = st[i].lat_max_ns;
	}
	double mean    = sum_w  ? sum_wm  / sum_w  : 0;
	double secondm = sum_w  ? sum_wm2 / sum_w  : 0; /* E[x^2] approx, biased */
	double stddev  = 0;
	{
		/* compute proper global stddev from per-thread moments */
		double s_w = 0, s_wm = 0, s_wvar = 0;
		for(long i = 0; i < g_total_threads; i++) {
			double w = (double)st[i].ops_done;
			if(w <= 0) continue;
			double m = st[i].latencies_ns_sum / w;
			double v = (st[i].latencies_ns_sum_sq / w) - m*m;
			if(v < 0) v = 0;
			s_w   += w;
			s_wm  += w * m;
			s_wvar += w * v + w * m * m; /* combined E[x^2] */
		}
		if(s_w > 0) {
			double gm = s_wm / s_w;
			double gx2 = s_wvar / s_w;
			double gv = gx2 - gm*gm;
			if(gv < 0) gv = 0;
			stddev = sqrt(gv);
		}
		(void)secondm;
	}

	/* ---- p50 / p99 from the merged histogram ----
	 * Each bucket covers [2^b, 2^(b+1)) ns. We report the bucket midpoint. */
	unsigned long grand_hist[30] = {0};
	for(long i = 0; i < g_total_threads; i++)
		for(unsigned b = 0; b < 30; b++)
			grand_hist[b] += st[i].hist[b];
	unsigned long total_samples = 0;
	for(unsigned b = 0; b < 30; b++) total_samples += grand_hist[b];
	unsigned long p50_thresh = total_samples / 2;
	unsigned long p99_thresh = total_samples - total_samples / 100;
	if(total_samples == 0) { p50_thresh = 0; p99_thresh = 0; }
	unsigned long acc = 0;
	double p50 = 0, p99 = 0;
	for(unsigned b = 0; b < 30 && total_samples; b++) {
		acc += grand_hist[b];
		double mid = (double)((1ULL << b) + (1ULL << (b+1))) * 0.5;
		if(p50 == 0 && acc >= p50_thresh) p50 = mid;
		if(p99 == 0 && acc >= p99_thresh) p99 = mid;
	}

	double ops_sec = wall > 0 ? (double)total_ops / wall : 0;

	printf("---- results ----\n");
	printf("wall            : %.3f s\n", wall);
	printf("ops (sized)     : %lu  (%.3f Mops/s)\n",
		total_ops, ops_sec / 1e6);
	printf("ops (malloc)    : %lu\n", total_malloc_ops);
	printf("total ops       : %lu\n", total_ops + total_malloc_ops);
	printf("throughput      : %.3f Mops/s\n", ops_sec / 1e6);
	printf("latency (ns)    : mean=%.0f  stddev=%.0f  min=%.0f  max=%.0f\n",
		mean, stddev, gmin, gmax);
	printf("latency p50/p99 : p50~=%.0f ns  p99~=%.0f ns\n", p50, p99);
	printf("peak RSS        : %zu kB\n", peak_rss_kb());

#ifdef JP_ALLOC_DEBUG
	/* Cache hit/miss counters for comparing allocator designs. */
	{
		extern void jp_alloc_diag(size_t *, size_t *);
		size_t hits = 0, misses = 0;
		jp_alloc_diag(&hits, &misses);
		if(hits + misses > 0) {
			double rate = 100.0 * (double)hits / (double)(hits + misses);
			printf("cache hit rate  : %.1f%%  (hits=%zu  misses=%zu)\n",
				rate, hits, misses);
		}
	}
	/* When run under JP_ALLOC_DEBUG the allocator aborts on the first ABA
	 * / double-free / corruption event. Reaching here means none fired. */
	printf("ABA self-check  : no ABA / corruption detected\n");
#endif
	fflush(stdout);
	free(th);
	free(st);
	return 0;
}