# AGENTS.md

## Build And Verification
- Build the local `tup` binary before tests with `./bootstrap.sh`; `bootstrap.sh` creates `build/tup`, initializes `.tup` if needed, then builds the root `./tup`.
- Rebuild after source changes with `./tup` once bootstrapped.
- Active CI is `.github/workflows/all.yml` (GitHub Actions): Ubuntu runs `./bootstrap.sh` then `cd test && ./test.sh --keep-going`; `.travis.yml` is legacy.
- On Linux the default build server is `fuse3` (`linux.tup`, `build.sh`), so `pkg-config fuse3`/FUSE3 headers are needed unless you override `TUP_SERVER`.
- Use `./bootstrap-ldpreload.sh` for an LD_PRELOAD-server build; it warns if `tup.config` does not set `CONFIG_TUP_SERVER=ldpreload` for the full Tup build.
- Use `./bootstrap-nofuse.sh` only to generate and run a temporary shell build script; its resulting `tup` still needs FUSE on platforms whose server uses FUSE.
- Run all tests from `test/` with `./test.sh`; run one test with `./test.sh t0000-init.sh`; use `./test.sh --keep-going` or `-k` to continue after failures.
- Directly invoking a test such as `test/t0000-init.sh` can leave `test/tuptesttmp-*` for inspection, but `test/test.sh` gives better failure messages.

## Source Layout
- CLI entrypoint and command dispatch live in `src/tup/tup/main.c`; core parser, DB, updater, graph, variant, and file logic live directly under `src/tup/`.
- Build-server backends are selected by `TUP_SERVER`: FUSE/FUSE3 and depfile code in `src/tup/server/`, LD_PRELOAD support in `src/ldpreload/`, Windows depfile support via `windepfile`.
- The monitor implementation is selected by `TUP_MONITOR` in platform `.tup` files; Linux uses `src/tup/monitor/inotify.c`, other platforms may use `null.c`.
- Bundled third-party code is under `src/lua`, `src/sqlite3`, `src/pcre`, and `src/inih`; prefer system PCRE by default via `TUP_USE_SYSTEM_PCRE=y` unless config says otherwise.
- `src/luabuiltin/luabuiltin.h` is generated from `src/luabuiltin/builtin.lua` using the built Lua and `xxd.lua`; do not edit the generated header as source.

## Memory Allocation
- `jp_alloc` is vendored under `src/jp_alloc/` (GPL-2.0-or-later, from https://github.com/jp-embedded/jp_alloc), as pure C11. It overrides libc `malloc`/`free`/`calloc`/`realloc` globally, so tup, SQLite, Lua, and PCRE all route through it automatically. Uses `mmap` on POSIX and `VirtualAlloc` on Windows.
- Set `CONFIG_TUP_USE_JP_ALLOC=n` in `tup.config` to disable jp_alloc and fall back to libc malloc. Useful for debugging or if jp_alloc's malloc override causes issues on a platform.
- `jp_alloc_reset()` is a no-op cleanup hook (valgrind-friendly); pool memory stays "still reachable" so valgrind doesn't report errors.
- The old `src/tup/mempool.c`/`.h` was removed; former mempool callers (graph, entry, file, pel_group, tent_tree, tent_list, tupid_list) now use `malloc`/`free` directly.
- jp_alloc's freelist is **64-bit CAS + 3-epoch-ring EBR + per-thread fixed-array cache (N=32 per size class) + batched refill (16 blocks per CAS)**. The earlier 128-bit tagged-pointer (cmpxchg16b) and headerless sized API implementations were both removed; `-mcx16` is no longer required. ABA is prevented by EBR: a popped block cannot reappear at the global freelist head while any thread is still mid-pop. The TLS cache delays returns to the global freelist and batches them into single-atomic-chain retire calls, amortizing EBR bookkeeping. Thread teardown is leak-free: a `pthread_key` destructor flushes the exiting thread's caches and per-epoch retired batches into a global limbo indexed by `retire_epoch % 3`, which live threads drain lazily while advancing the global epoch.
- Portable to 32-bit and 64-bit; requires GCC 4.7+/Clang 3.0+ or MSVC 2015+ (with compiler fallbacks in `jp_alloc.c`). POSIX (pthreads) or Windows with MinGW (WinPthread). No platform-specific intrinsics.
- The standalone benchmark/correctness test `src/jp_alloc/jp_alloc_bench.c` (compiled with `-DJP_ALLOC_BENCH` and `jp_alloc.c` into a `main()` binary) spawns 300 pthreads doing tup-shaped alloc/free churn and reports ops/sec, p50/p99 latency, and peak RSS. The shell wrapper `test/t5800-jp-alloc-threads.sh` builds release + `-DJP_ALLOC_DEBUG` + 300-thread-stress variants; the debug variant aborts on ABA / double-free / corruption, so reaching "no ABA" output is the regression gate. Note that ASan/TSan are incompatible with the malloc-override and are not exercised by the wrapper.

## Tup Configuration Notes
- Top-level build rules are `Tupfile` plus `Tuprules.tup`; per-platform defaults are in `linux.tup`, `macosx.tup`, `freebsd.tup`, `netbsd.tup`, `win32.tup`, and can be overridden by `tup.config` `CONFIG_*` values.
- Linux defaults to `TUP_SERVER=fuse3`; macOS/FreeBSD use `fuse`; Windows uses `windepfile` and disables system PCRE.
- Cross-building Windows uses `local-build-configs/win32.config` (`CONFIG_TUP_PLATFORM=win32`) and the `x86_64-w64-mingw32-*`/`i686-w64-mingw32-*` tools declared in `win32.tup`.
- Useful `tup.config` build toggles: `CONFIG_TUP_SANITIZE=y` (ASan+UBSan), `CONFIG_TUP_WERROR=y`, `CONFIG_TUP_DEBUG=y`, `CONFIG_TUP_PROFILING=y`, `CONFIG_TUP_32_BIT=y`.

## Test Harness Gotchas
- Tests source `test/tup.sh`, which prepends the repo root to `PATH`, creates a fresh `tuptesttmp-$testname`, runs `tup init --no-sync --force`, and removes the directory via `eotup`.
- Use existing helpers in `test/tup.sh` (`update`, `update_fail_msg`, `parse_fail_msg`, `gitignore_good`, `tup_object_exist`, etc.) instead of open-coded assertions.
- Monitor, Windows, macOS, FreeBSD, Python, Bash, ccache, SUID, and LD_PRELOAD limitations are handled by skip helpers in `test/tup.sh`; use those for new platform-sensitive tests.
- Set `TUP_VALGRIND=1` or `TUP_HELGRIND=1` when running tests to wrap update/generate/compiledb paths through valgrind/helgrind.
- macOS CI uses FUSE-T (NFS v4 backend); a fixed set of tests is skipped and flaky tests are auto-retried up to 4× — check `.github/workflows/all.yml` before treating a single macOS failure as a real regression.

## Style
- `.editorconfig` requires LF, UTF-8, and tabs with `indent_size = 4`; existing C files also use vim `ts=8 sw=8 noet` style.
- The repo-root `.gitignore` is generated by Tup (header says "Do not edit"); don't hand-edit it. `tup`, `build/`, `tup-version.o`, `libtup_client.a`, and `tup_client.h` are gitignored build artifacts — don't commit them.
