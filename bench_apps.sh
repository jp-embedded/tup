#!/bin/bash
# Real-world allocator comparison
# Tests: GCC, ffmpeg, OpenSCAD, Inkscape, Blender, NumPy, PIL, SQLite3 CLI
# Runs under LD_PRELOAD with different allocators. Median of N runs (default 3).

set -e
cd /home/jp/work/tup

RUNS=${1:-3}
TMP=/tmp/opencode
OUTDIR=$TMP/alloc_results

# Build jp_alloc.so variants
echo "Building jp_alloc.so..."
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -shared \
    -o "$TMP/jp_alloc.so" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -DJP_MADVISE_PID=99 -shared \
    -o "$TMP/jp_alloc_nomadv.so" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null

JPSO=$TMP/jp_alloc.so
JPSO_NM=$TMP/jp_alloc_nomadv.so
SQLITE3_C=$TMP/sqlite-amalgamation-3460000/sqlite3.c
GCC_PARALLEL=$TMP/gcc_parallel
SVG_FILE=$TMP/complex.svg
TEST_VIDEO=$TMP/test_1080p.mp4
SCAD_MODEL=$TMP/benchmark.scad

JEMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 2>/dev/null || echo "")
TCMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4* 2>/dev/null | head -1 || echo "")
MIMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libmimalloc.so.2.1 2>/dev/null || echo "")

mkdir -p "$OUTDIR"

run_test() {
    local label="$1"; shift
    local preload="$1"; shift
    local cmd="$*"
    local times="" rss_vals=""
    for i in $(seq 1 $RUNS); do
        local tf="$OUTDIR/$(echo "$label" | tr ' /' '__')_run${i}.time"
        if [ -z "$preload" ]; then
            /usr/bin/time -f "%e %M" sh -c "$cmd 2>/dev/null" 2> "$tf" || true
        else
            /usr/bin/time -f "%e %M" sh -c "LD_PRELOAD=$preload $cmd 2>/dev/null" 2> "$tf" || true
        fi
        local t=$(awk '{print $1}' "$tf")
        local r=$(awk '{print $2}' "$tf")
        times="$times $t"
        rss_vals="$rss_vals $r"
    done
    local tm=$(echo "$times" | tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$0} END{printf "%.2f", a[int(NR/2)+1]}')
    local rm=$(echo "$rss_vals" | tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$0} END{printf "%d", a[int(NR/2)+1]}')
    printf "  %-20s | %7s s | %8s kB (%6.1f MB)\n" "$label" "$tm" "$rm" "$(echo "scale=1; $rm / 1024" | bc)"
}

header() {
    echo ""
    echo "============================================================"
    echo "=== $1"
    echo "============================================================"
    echo ""
    printf "  %-20s | %7s   | %s\n" "Allocator" "Time" "Peak RSS"
    printf "  %-20s-+----------+---------------------------\n" "--------------------"
}

# ==== 1. GCC: compile SQLite amalgamation -O2 (single-threaded) ====
header "GCC: compile SQLite amalgamation -O2 (single-threaded, ~258K lines)"
CMD="gcc -O2 -c $SQLITE3_C -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION"
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "jp_alloc(nomadv)" "$JPSO_NM" $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD

# ==== 2. GCC: 8x SQLite amalgamation -j8 (parallel, multi-process) ====
make -C "$GCC_PARALLEL" clean >/dev/null 2>&1
header "GCC: 8x SQLite amalgamation -O2 -j8 (parallel, ~2M lines total)"
CMD="make -j8 -C $GCC_PARALLEL clean; make -j8 -C $GCC_PARALLEL test_binary"
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "jp_alloc(nomadv)" "$JPSO_NM" "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 3. ffmpeg: transcode 1080p 10s video ====
header "ffmpeg: transcode 1080p 10s video (libx264 fast)"
CMD="ffmpeg -y -i $TEST_VIDEO -c:v libx264 -preset fast -crf 23 $OUTDIR/out.mp4"
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 4. OpenSCAD: render 8000-sphere boolean model ====
header "OpenSCAD: render 8000-sphere boolean model (single-threaded)"
CMD="openscad -o $OUTDIR/out.png $SCAD_MODEL"
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 5. Inkscape: 10K-path SVG to PNG ====
header "Inkscape: 10K-path SVG to PNG (Cairo/Pango)"
CMD="inkscape $SVG_FILE --export-type=png --export-filename $OUTDIR/out.png"
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 6. Blender: 1000 ico-spheres render (headless) ====
header "Blender: 1000 ico-spheres, 32 samples, 1280x720 (headless)"
CMD="blender --background --python-expr \"
import bpy, math
for i in range(1000):
    angle = i * 0.07
    r = 3 + (i % 12)
    x = r * math.sin(angle * 0.7)
    y = r * math.cos(angle * 0.7)
    z = (i % 15) * 0.3
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=0.2, location=(x, y, z))
bpy.context.scene.render.resolution_x = 1280
bpy.context.scene.render.resolution_y = 720
bpy.context.scene.cycles.samples = 32
bpy.context.scene.render.filepath = '$OUTDIR/out.png'
bpy.ops.render.render(write_still=True)
\""
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 7. NumPy: SVD on 2000x2000 matrix ====
header "Python NumPy: SVD 2000x2000 matrix (single-threaded heavy compute)"
CMD="python3 -c \"import numpy; a=numpy.random.rand(2000,2000); numpy.linalg.svd(a)\""
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

# ==== 8. SQLite3 CLI: 2M rows insert+index+query (in-memory) ====
header "SQLite3 CLI: 2M rows insert+index+query (in-memory DB, heavy malloc)"
CMD="sqlite3 :memory: \"CREATE TABLE t(id INTEGER, data BLOB); WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 2000000) INSERT INTO t SELECT x, randomblob(200) FROM cnt; SELECT count(*), sum(length(data)) FROM t; CREATE INDEX idx_data ON t(data); SELECT count(*) FROM t WHERE data > X'00'; DROP TABLE t;\""
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"

echo ""
echo "=== Done. Results in $OUTDIR ==="