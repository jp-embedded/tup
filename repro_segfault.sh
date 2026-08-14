#!/bin/bash
# jp_alloc segfault reproduction script
#
# Tests the magazine design for segfaults under:
# 1. 300-thread bench (malloc override + heavy alloc/free churn)
# 2. Bootstrap tup (FUSE server threads + updater worker pool)
# 3. JP_ALLOC_DEBUG self-checks (ABA / double-free detection)
#
# Usage: ./repro_segfault.sh [iterations]
# Default: 10 iterations. Crashes are reported with backtrace.

set -e
cd /home/jp/work/tup

ITERATIONS=${1:-10}
CRASHES=0

echo "=== Building binaries ==="
# Release bench
cc -O2 -Wall -Wextra -Wno-unused-result -DJP_ALLOC_IMPLEMENTATION -DJP_ALLOC_BENCH \
    -Isrc/jp_alloc src/jp_alloc/jp_alloc.c src/jp_alloc/jp_alloc_bench.c \
    -o /tmp/opencode/repro_bench -lpthread -lrt -lm 2>&1 | head -3

# Debug bench
cc -O0 -g -DJP_ALLOC_IMPLEMENTATION -DJP_ALLOC_DEBUG -DJP_ALLOC_BENCH \
    -Isrc/jp_alloc src/jp_alloc/jp_alloc.c src/jp_alloc/jp_alloc_bench.c \
    -o /tmp/opencode/repro_bench_dbg -lpthread -lrt -lm 2>&1 | head -3

echo ""
echo "=== Test 1: 300-thread bench (release) — $ITERATIONS runs ==="
for i in $(seq 1 $ITERATIONS); do
    JPBENCH_THREADS=300 JPBENCH_OPS=10000 /tmp/opencode/repro_bench 2>&1 | grep -q "Segmentation" && {
        echo "  CRASH on run $i!"
        CRASHES=$((CRASHES+1))
    } || echo "  run $i: OK"
done

echo ""
echo "=== Test 2: 300-thread bench (debug, ABA self-check) — $ITERATIONS runs ==="
for i in $(seq 1 $ITERATIONS); do
    JPBENCH_THREADS=300 JPBENCH_OPS=10000 timeout 120 /tmp/opencode/repro_bench_dbg 2>&1 | grep -qE "Segmentation|jp_alloc:" && {
        echo "  CRASH/ABORT on run $i!"
        CRASHES=$((CRASHES+1))
    } || echo "  run $i: OK"
done

echo ""
echo "=== Test 3: Bootstrap tup (FUSE + updater threads) — $ITERATIONS runs ==="
for i in $(seq 1 $ITERATIONS); do
    rm -rf .tup build tup tup-version.o libtup_client.a tup_client.h \
           src/lua/liblua.a src/lua/lua src/luabuiltin/luabuiltin.h
    find . -name "*.o" -type f 2>/dev/null | xargs -r rm -f 2>/dev/null
    CFLAGS="-g -O0" ./bootstrap.sh 2>&1 | grep -q "Segmentation" && {
        echo "  CRASH on bootstrap $i!"
        CRASHES=$((CRASHES+1))
    } || echo "  bootstrap $i: OK"
done

echo ""
echo "=== RESULTS: $CRASHES crashes out of $((ITERATIONS * 3)) tests ==="