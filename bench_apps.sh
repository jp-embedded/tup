#!/bin/bash
# Real-world allocator comparison: ffmpeg + OpenSCAD + GCC + Inkscape
#
# Tests actual applications under LD_PRELOAD with different allocators.
# Measures wall time and peak RSS via /usr/bin/time.
# Median of N runs (default 3).

set -e
cd /home/jp/work/tup

RUNS=${1:-3}
TMP=/tmp/opencode
OUTDIR=$TMP/alloc_results

JPSO=$TMP/jp_alloc.so
JPSO_NOMADV=$TMP/jp_alloc_nomadv.so
SQLITE3_C=$TMP/sqlite-amalgamation-3460000/sqlite3.c
GCC_BENCH=$TMP/gcc_bench
SVG_FILE=$TMP/complex.svg
TEST_VIDEO=$TMP/test_1080p.mp4
SCAD_MODEL=$TMP/benchmark.scad

# Build jp_alloc.so variants
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -shared \
    -o "$JPSO" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -DJP_MADVISE_PID=99 -shared \
    -o "$JPSO_NOMADV" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null

# Allocator .so paths
JEMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 2>/dev/null || echo "")
TCMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4* 2>/dev/null | head -1 || echo "")
MIMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libmimalloc.so.2.1 2>/dev/null || echo "")

mkdir -p "$OUTDIR"

run_test() {
    local label="$1"; shift
    local preload="$1"; shift
    local cmd="$*"
    local times=""
    local rss_vals=""

    for i in $(seq 1 $RUNS); do
        local time_file="$OUTDIR/$(echo "$label" | tr ' /' '__')_${i}.time"
        if [ -z "$preload" ]; then
            /usr/bin/time -f "%e %M" sh -c "$cmd 2>/dev/null" 2> "$time_file"
        else
            /usr/bin/time -f "%e %M" sh -c "LD_PRELOAD=$preload $cmd 2>/dev/null" 2> "$time_file"
        fi
        local t=$(awk '{print $1}' "$time_file")
        local rss=$(awk '{print $2}' "$time_file")
        times="$times $t"
        rss_vals="$rss_vals $rss"
    done

    local t_med=$(echo "$times" | tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$0} END{printf "%.2f", a[int(NR/2)+1]}')
    local r_med=$(echo "$rss_vals" | tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$0} END{printf "%d", a[int(NR/2)+1]}')

    printf "  %-20s | %7s s | %8s kB (%6.1f MB)\n" "$label" "$t_med" "$r_med" "$(echo "scale=1; $r_med / 1024" | bc)"
}

header() {
    echo ""
    echo "============================================================"
    echo "=== $1"
    echo "============================================================"
    echo ""
    printf "  %-20s | %7s   | %s\n" "Allocator" "Time" "Peak RSS"
    printf "  %-20s-+----------+---------------------------\n" "---------------------"
}

# === Test 1: GCC compile SQLite amalgamation (single-threaded, -O2) ===
header "GCC: compile SQLite amalgamation -O2 (single-threaded)"
run_test "jp_alloc(madvise)"  "$JPSO"        gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
run_test "jp_alloc(no-madv)"  "$JPSO_NOMADV" gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
run_test "glibc"              ""             gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO"  gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO"  gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO"  gcc -O2 -c "$SQLITE3_C" -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION

# === Test 2: GCC multi-file project compile+link (-j8 parallel) ===
# clean before each run inside run_test
header "GCC: compile+link 50 files -O2 -j8 (parallel)"
run_test "jp_alloc(madvise)"  "$JPSO"        make -j8 -C "$GCC_BENCH" clean && make -j8 -C "$GCC_BENCH" test_binary
run_test "jp_alloc(no-madv)"  "$JPSO_NOMADV" make -j8 -C "$GCC_BENCH" clean && make -j8 -C "$GCC_BENCH" test_binary
run_test "glibc"              ""             make -j8 -C "$GCC_BENCH" clean && make -j8 -C "$GCC_BENCH" test_binary
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO"  make -j8 -C "$GCC_BENCH" clean && make -j8 -C "$GCC_BENCH" test_binary
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO"  make -j8 -C "$GCC_BENCH" clean && make -j8 -C "$GCC_BENCH" test_binary

# === Test 3: ffmpeg transcode ===
header "ffmpeg: transcode 1080p 10s video (libx264 fast)"
run_test "jp_alloc(madvise)"  "$JPSO"        ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_jp.mp4"
run_test "jp_alloc(no-madv)"  "$JPSO_NOMADV" ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_jpnm.mp4"
run_test "glibc"              ""             ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_glibc.mp4"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO"  ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_jem.mp4"
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO"  ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_tc.mp4"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO"  ffmpeg -i "$TEST_VIDEO" -c:v libx264 -preset fast -crf 23 -y "$OUTDIR/out_mim.mp4"

# === Test 4: OpenSCAD render ===
header "OpenSCAD: render 8000-sphere boolean model"
run_test "jp_alloc(madvise)"  "$JPSO"        openscad -o "$OUTDIR/out_jp.png" "$SCAD_MODEL"
run_test "jp_alloc(no-madv)"  "$JPSO_NOMADV" openscad -o "$OUTDIR/out_jpnm.png" "$SCAD_MODEL"
run_test "glibc"              ""             openscad -o "$OUTDIR/out_glibc.png" "$SCAD_MODEL"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO"  openscad -o "$OUTDIR/out_jem.png" "$SCAD_MODEL"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO"  openscad -o "$OUTDIR/out_mim.png" "$SCAD_MODEL"

# === Test 5: Inkscape SVG export ===
header "Inkscape: render 10K-path SVG to PNG"
run_test "jp_alloc(madvise)"  "$JPSO"        inkscape "$SVG_FILE" --export-type=png --export-filename "$OUTDIR/out_jp.png"
run_test "jp_alloc(no-madv)"  "$JPSO_NOMADV" inkscape "$SVG_FILE" --export-type=png --export-filename "$OUTDIR/out_jpnm.png"
run_test "glibc"              ""             inkscape "$SVG_FILE" --export-type=png --export-filename "$OUTDIR/out_glibc.png"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO"  inkscape "$SVG_FILE" --export-type=png --export-filename "$OUTDIR/out_jem.png"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO"  inkscape "$SVG_FILE" --export-type=png --export-filename "$OUTDIR/out_mim.png"

echo ""
echo "=== Done. Results in $OUTDIR ==="