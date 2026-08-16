#!/bin/bash
# Real-world allocator comparison
# Tests: GCC, ffmpeg, OpenSCAD, Inkscape, Blender, NumPy, PIL, SQLite3 CLI
# Runs under LD_PRELOAD with different allocators. Median of N runs (default 3).
#
# Fixtures (SQLite amalgamation, test video, SVG, SCAD model, gcc_parallel
# Makefile) are downloaded/generated on first run and cached in $TMP. Re-runs
# skip fixture generation. Each fixture has a fallback: if generation fails
# (no internet, missing tool, etc.) the corresponding test silently skips
# rather than aborting the whole run.
#
# Usage:
#   ./bench_apps.sh [RUNS]                         # run all apps N times each
#   BENCH_APPS=gcc1,sqlite ./bench_apps.sh 1       # only run listed apps; tags:
#                                                  #   gcc1  gcc8  ffmpeg
#                                                  #   openscad inkscape
#                                                  #   blender numpy sqlite
#
# Allocators compared: jp_alloc (default + no-madvise), glibc (default),
# jemalloc, tcmalloc, mimalloc — each only if its .so is installed on the host.

set -e
cd /home/jp/work/tup

RUNS=${1:-3}
TMP=/tmp/opencode
OUTDIR=$TMP/alloc_results

# App filter: empty/unset = run all; else comma-separated tag list.
BENCH_APPS=${BENCH_APPS:-}
app_enabled() {
  local tag=$1
  [ -z "$BENCH_APPS" ] && return 0
  case ",$BENCH_APPS," in
    *",$tag,"*) return 0;;
    *)          return 1;;
  esac
}

mkdir -p "$TMP" "$OUTDIR"

# ==== Fixture setup ====
# Each block is idempotent — re-runs skip work if the file exists.

# 1. SQLite amalgamation (3.46.0) for GCC compile tests.
SQLITE3_C=$TMP/sqlite-amalgamation-3460000/sqlite3.c
if [ ! -f "$SQLITE3_C" ]; then
  echo "[setup] Downloading SQLite 3.46.0 amalgamation..."
  ( cd "$TMP" && \
    curl -sL https://www.sqlite.org/2024/sqlite-amalgamation-3460000.zip -o sqlite.zip && \
    unzip -q sqlite.zip && rm -f sqlite.zip ) \
    || echo "[setup] WARNING: SQLite download failed; gcc1/gcc8 skipped"
fi
[ -f "$SQLITE3_C" ] || SQLITE3_C=

# 2. gcc_parallel: 8 copies of sqlite3.c + Makefile for -j8 test. Needs #1.
GCC_PARALLEL=$TMP/gcc_parallel
if { [ ! -d "$GCC_PARALLEL" ] || [ ! -f "$GCC_PARALLEL/Makefile" ]; } && [ -n "$SQLITE3_C" ]; then
  echo "[setup] Building gcc_parallel harness..."
  mkdir -p "$GCC_PARALLEL"
  for i in $(seq 1 8); do
    cp "$TMP/sqlite-amalgamation-3460000/sqlite3.c" "$GCC_PARALLEL/sqlite3_$i.c"
  done
  cat > "$GCC_PARALLEL/Makefile" <<'EOF'
CC ?= gcc
CFLAGS ?= -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
OBJS = $(addsuffix .o,$(addprefix sqlite3_,1 2 3 4 5 6 7 8))
test_binary: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o test_binary
sqlite3_%.o: sqlite3_%.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) test_binary
EOF
fi
[ -d "$GCC_PARALLEL" ] && [ -f "$GCC_PARALLEL/Makefile" ] || GCC_PARALLEL=

# 3. test_1080p.mp4 (10s, 1920x1080) for ffmpeg transcode test.
TEST_VIDEO=$TMP/test_1080p.mp4
if [ ! -f "$TEST_VIDEO" ] && command -v ffmpeg >/dev/null 2>&1; then
  echo "[setup] Generating 10s 1080p test video..."
  ffmpeg -y -f lavfi -i "testsrc=duration=10:size=1920x1080:rate=30" \
    -c:v libx264 -preset ultrafast -crf 28 "$TEST_VIDEO" 2>/dev/null \
    || echo "[setup] WARNING: video generation failed; ffmpeg skipped"
fi
[ -f "$TEST_VIDEO" ] || TEST_VIDEO=

# 4. benchmark.scad (8000 unioned spheres) for OpenSCAD test.
SCAD_MODEL=$TMP/benchmark.scad
if [ ! -f "$SCAD_MODEL" ] && command -v python3 >/dev/null 2>&1; then
  echo "[setup] Generating benchmark.scad (8000 spheres)..."
  python3 -c "
import math
lines = []
for i in range(8000):
    angle = i * 0.07
    r = 3 + (i % 12)
    x = r * math.sin(angle * 0.7)
    y = r * math.cos(angle * 0.7)
    z = (i % 15) * 0.3
    lines.append(f'translate([{x},{y},{z}]) sphere(r=0.2);')
with open('$SCAD_MODEL', 'w') as f:
    f.write('union(){\n' + '\n'.join(lines) + '\n};\n')
" 2>/dev/null || echo "[setup] WARNING: scad generation failed; openscad skipped"
fi
[ -f "$SCAD_MODEL" ] || SCAD_MODEL=

# 5. complex.svg (10K paths) for Inkscape test.
SVG_FILE=$TMP/complex.svg
if [ ! -f "$SVG_FILE" ] && command -v python3 >/dev/null 2>&1; then
  echo "[setup] Generating complex.svg (10K paths)..."
  python3 -c "
import random
random.seed(42)
parts = ['<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1000 1000\">']
for i in range(10000):
    x1, y1 = random.randint(0, 1000), random.randint(0, 1000)
    x2, y2 = x1 + random.randint(1, 50), y1 + random.randint(1, 50)
    parts.append(f'<path d=\"M{x1},{y1} L{x2},{y2}\" stroke=\"#{random.randint(0,0xffffff):06x}\" stroke-width=\"1\" fill=\"none\"/>')
parts.append('</svg>')
with open('$SVG_FILE', 'w') as f:
    f.write('\n'.join(parts))
" 2>/dev/null || echo "[setup] WARNING: svg generation failed; inkscape skipped"
fi
[ -f "$SVG_FILE" ] || SVG_FILE=

# Build jp_alloc.so variants
echo "Building jp_alloc.so..."
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -shared \
    -o "$TMP/jp_alloc.so" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null
cc -O2 -std=c11 -fpic -DJP_ALLOC_IMPLEMENTATION -DJP_MADVISE_PID=99 -shared \
    -o "$TMP/jp_alloc_nomadv.so" src/jp_alloc/jp_alloc.c -lpthread -lm 2>/dev/null

JPSO=$TMP/jp_alloc.so
JPSO_NM=$TMP/jp_alloc_nomadv.so

JEMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 2>/dev/null || echo "")
TCMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4* 2>/dev/null | head -1 || echo "")
MIMALLOC_SO=$(ls /usr/lib/x86_64-linux-gnu/libmimalloc.so.2.1 2>/dev/null || echo "")

run_test() {
    local label="$1"; shift
    local preload="$1"; shift
    local cmd="$*"
    local times="" rss_vals=""
    for i in $(seq 1 $RUNS); do
        local tf="$OUTDIR/$(echo "$label" | tr ' /' '__')_run${i}.time"
        if [ -z "$preload" ]; then
            /usr/bin/time -f "%e %M" sh -c "$cmd >/dev/null 2>&1" 2> "$tf" || true
        else
            /usr/bin/time -f "%e %M" sh -c "LD_PRELOAD=$preload $cmd >/dev/null 2>&1" 2> "$tf" || true
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
if app_enabled gcc1 && [ -n "$SQLITE3_C" ]; then
header "GCC: compile SQLite amalgamation -O2 (single-threaded, ~258K lines)"
CMD="gcc -O2 -c $SQLITE3_C -o /dev/null -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION"
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "jp_alloc(nomadv)" "$JPSO_NM" $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

# ==== 2. GCC: 8x SQLite amalgamation -j8 (parallel, multi-process) ====
if app_enabled gcc8 && [ -n "$GCC_PARALLEL" ]; then
make -C "$GCC_PARALLEL" clean >/dev/null 2>&1
header "GCC: 8x SQLite amalgamation -O2 -j8 (parallel, ~2M lines total)"
CMD="make -j8 -C $GCC_PARALLEL clean; make -j8 -C $GCC_PARALLEL test_binary"
run_test "jp_alloc"      "$JPSO"    "$CMD"
run_test "jp_alloc(nomadv)" "$JPSO_NM" "$CMD"
run_test "glibc"         ""         "$CMD"
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" "$CMD"
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" "$CMD"
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" "$CMD"
fi

# ==== 3. ffmpeg: transcode 1080p 10s video ====
if app_enabled ffmpeg && [ -n "$TEST_VIDEO" ]; then
header "ffmpeg: transcode 1080p 10s video (libx264 fast)"
CMD="ffmpeg -y -i $TEST_VIDEO -c:v libx264 -preset fast -crf 23 $OUTDIR/out.mp4"
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$TCMALLOC_SO" ]  && run_test "tcmalloc"  "$TCMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

# ==== 4. OpenSCAD: render 8000-sphere boolean model ====
if app_enabled openscad && [ -n "$SCAD_MODEL" ]; then
header "OpenSCAD: render 8000-sphere boolean model (single-threaded)"
CMD="openscad -o $OUTDIR/out.png $SCAD_MODEL"
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

# ==== 5. Inkscape: 10K-path SVG to PNG ====
if app_enabled inkscape && [ -n "$SVG_FILE" ]; then
header "Inkscape: 10K-path SVG to PNG (Cairo/Pango)"
CMD="inkscape $SVG_FILE --export-type=png --export-filename=$OUTDIR/out.png"
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

# ==== 6. Blender: 1000 ico-spheres render (headless) ====
if app_enabled blender && command -v blender >/dev/null 2>&1; then
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
fi

# ==== 7. NumPy: SVD on 2000x2000 matrix ====
if app_enabled numpy && python3 -c "import numpy" 2>/dev/null; then
header "Python NumPy: SVD 2000x2000 matrix (single-threaded heavy compute)"
CMD="python3 -c \"import numpy; a=numpy.random.rand(2000,2000); numpy.linalg.svd(a)\""
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

# ==== 8. SQLite3 CLI: 2M rows insert+index+query (in-memory) ====
if app_enabled sqlite && command -v sqlite3 >/dev/null 2>&1; then
header "SQLite3 CLI: 2M rows insert+index+query (in-memory DB, heavy malloc)"
CMD="sqlite3 :memory: \"CREATE TABLE t(id INTEGER, data BLOB); WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x < 2000000) INSERT INTO t SELECT x, randomblob(200) FROM cnt; SELECT count(*), sum(length(data)) FROM t; CREATE INDEX idx_data ON t(data); SELECT count(*) FROM t WHERE data > X'00'; DROP TABLE t;\""
run_test "jp_alloc"      "$JPSO"    $CMD
run_test "glibc"         ""         $CMD
[ -n "$JEMALLOC_SO" ]  && run_test "jemalloc"  "$JEMALLOC_SO" $CMD
[ -n "$MIMALLOC_SO" ]  && run_test "mimalloc"  "$MIMALLOC_SO" $CMD
fi

echo ""
echo "=== Done. Results in $OUTDIR ==="