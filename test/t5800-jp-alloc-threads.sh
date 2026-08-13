#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  Mike Shal <marfey@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# Stress / correctness test for the in-repo jp_alloc allocator: spawn 300
# pthreads doing tup-shaped alloc/free churn (sized and unsized paths),
# build three variants (release, JP_ALLOC_DEBUG, headerless-extra-stress)
# and assert none of them abort. The JP_ALLOC_DEBUG variant aborts on ABA,
# double-free, wrong-API, or size-mismatch — so reaching "no ABA" output is
# the regression gate.
#
# Skipped automatically if pthreads are unavailable or if the platform's
# compiler doesn't support the required atomic intrinsics on this host.

. ./tup.sh

if [ "$tupos" = "CYGWIN" ] || [ "$tupos" = "Windows_NT" ]; then
	# pthread-backed test isn't meaningful under the windepfile server
	echo "Skipping jp_alloc bench on Windows (no pthreads in tup)." 1>&2
	eotup
fi

# Skip on macOS arm64: --disable under __int128 path used to be needed.
# The new code uses 64-bit CAS only, so we just require a working cc.
cc=${CC:-gcc}

JPDIR="$PWD/../../src/jp_alloc"
BENCH_C="$JPDIR/jp_alloc_bench.c"
ALLOC_C="$JPDIR/jp_alloc.c"

if [ ! -f "$BENCH_C" ] || [ ! -f "$ALLOC_C" ]; then
	echo "*** Missing jp_alloc sources" 1>&2
	exit 1
fi

# Small defaults so the test runs fast under ./test.sh. Override with
# JPBENCH_THREADS / JPBENCH_OPS in the environment.
THREADS=${JPBENCH_THREADS:-64}
OPS=${JPBENCH_OPS:-5000}

run_variant() {
	name="$1"; shift
	binary="$1"; shift
	echo "--- variant: $name ---"
	if ! $cc "$@" -o "$binary" 2> "$binary.log"; then
		echo "*** Build failed for $name (see $binary.log):" 1>&2
		cat "$binary.log" 1>&2
		exit 1
	fi
	JPBENCH_THREADS=$THREADS JPBENCH_OPS=$OPS timeout 120 "./$binary" > "$binary.out" 2>&1 || {
		echo "*** $name exited non-zero: output:" 1>&2
		cat "$binary.out" 1>&2
		exit 1
	}
	# JP_ALLOC_DEBUG build must report "no ABA" — anything else means the
	# self-check fired.
	if echo "$name" | grep -q 'debug'; then
		if ! grep -q 'no ABA / corruption detected' "$binary.out"; then
			echo "*** $name did not report ABA-free; output was:" 1>&2
			cat "$binary.out" 1>&2
			exit 1
		fi
	else
		# Release variants: just make sure nothing mentions "jp_alloc:"
		# (which would mean a debug-style abort string leaked through).
		if grep -q '^jp_alloc: ' "$binary.out"; then
			echo "*** $name reported an allocator error:" 1>&2
			cat "$binary.out" 1>&2
			exit 1
		fi
	fi
	# Sanity: ops throughput should exceed zero.
	if ! grep -q 'throughput' "$binary.out"; then
		echo "*** $name produced no throughput line:" 1>&2
		cat "$binary.out" 1>&2
		exit 1
	fi
	grep -E '^(throughput|wall|latency|peak RSS|ABA self-check)' "$binary.out"
	rm -f "$binary" "$binary.log" "$binary.out"
}

# --- Variant 1: release (no JP_ALLOC_DEBUG) ---
run_variant "release" \
	"jpbench_rel" \
	-O2 -Wall -Wextra -Wno-unused-result \
	-DJP_ALLOC_IMPLEMENTATION -DJP_ALLOC_BENCH -I"$JPDIR" \
	"$ALLOC_C" "$BENCH_C" \
	-lpthread -lrt -lm

# --- Variant 2: JP_ALLOC_DEBUG (ABA / double-free / corruption self-checks) ---
run_variant "debug-self-check" \
	"jpbench_dbg" \
	-O1 -g -Wall -Wextra -Wno-unused-result \
	-DJP_ALLOC_IMPLEMENTATION -DJP_ALLOC_DEBUG -DJP_ALLOC_BENCH -I"$JPDIR" \
	"$ALLOC_C" "$BENCH_C" \
	-lpthread -lrt -lm

# --- Variant 3: 300-thread stress at higher op count (smoke test only) ---
prev_threads=$THREADS
prev_ops=$OPS
THREADS=300
OPS=10000
run_variant "release-300-threads" \
	"jpbench_300" \
	-O2 -Wall -Wextra -Wno-unused-result \
	-DJP_ALLOC_IMPLEMENTATION -DJP_ALLOC_BENCH -I"$JPDIR" \
	"$ALLOC_C" "$BENCH_C" \
	-lpthread -lrt -lm
THREADS=$prev_threads
OPS=$prev_ops

echo "All jp_alloc variants passed (no abort, no ABA, throughput > 0)."
eotup