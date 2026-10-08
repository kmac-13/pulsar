# Running the Stellyra Benchmarks

## Prerequisites

| Dependency | Where to get it | Required for |
|---|---|---|
| CMake ≥ 3.16 | cmake.org | All |
| C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+) | - | All |
| [google/benchmark](https://github.com/google/benchmark) | Clone alongside repo | All |
| Qt6 | qt.io installer | `bench_qt` only |
| libsigc++ 3.0 | vcpkg (`vcpkg install libsigcpp`) | `bench_libsigcpp` only |
| Python 3 | python.org | `compare.py` result aggregation |

All other libraries (nano-signal-slot, sigslot, rocket, nod, vdk-signals) are fetched automatically by CMake at configure time via FetchContent.

---

## Directory layout expected

```
<workspace>/
    stellyra/               ← this repo
    google-benchmark/     ← clone of github.com/google/benchmark
```

If your google-benchmark clone is elsewhere, pass `-DBENCHMARK_ROOT=<path>` to CMake.

---

## Build (Windows, Git Bash or Developer Command Prompt)

```bash
cd stellyra/benchmarks

cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2019_64"   # adjust to your Qt path

cmake --build build --config Release
```

`bench_libsigcpp` is skipped silently if libsigc++ is not found.
`bench_qt` is skipped silently if Qt6 is not found.
`bench_boost` is disabled; see `boost_signals2/CMakeLists.txt` to enable.

---

## One-step: run everything and aggregate

`run_benchmarks.sh` runs every built executable with JSON output and calls `compare.py` automatically.  From `stellyra/benchmarks/`:

```bash
./run_benchmarks.sh build/Desktop_Qt_6_9_0_MinGW_64_bit-Release
```

If no build directory is given, it picks the most recently modified directory under `build/`.  Executables that weren't built (e.g. `bench_qt` or `bench_libsigcpp` when their dependencies weren't found) are skipped with
a message rather than failing the run.  Output:

- `results/<name>.json` - one file per executable
- `BENCHMARK_RESULTS.md` - aggregated Markdown table

This is the recommended way to run the full suite.  The manual steps below are useful for running a single executable or debugging one library.

Note that certain benchmarks (e.g. vdk) seem to hang when running multiple iterations and are explicitly executed as single runs in a loop through `aggregate_repeated_runs.py`.  Sometimes, though, the environment path variables (Windows) aren't picked up, so specifying them, e.g. `$env:PATH = "C:\Qt\6.9.0\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;$env:PATH"` before running `run_benchmarks.sh` often helps.

---

## Running individual benchmarks manually

From the `build/` directory, run each executable with JSON output.
Create a `results/` directory first:

```bash
mkdir -p results

./Release/bench_stellyra_ts.exe  --benchmark_format=json --benchmark_out=results/stellyra_ts.json
./Release/bench_Stellyra_st.exe  --benchmark_format=json --benchmark_out=results/stellyra_st.json
./Release/bench_nano_st.exe      --benchmark_format=json --benchmark_out=results/nano_st.json
./Release/bench_nano_ts.exe      --benchmark_format=json --benchmark_out=results/nano_ts.json
./Release/bench_sigslot_st.exe   --benchmark_format=json --benchmark_out=results/sigslot_st.json
./Release/bench_sigslot_mt.exe   --benchmark_format=json --benchmark_out=results/sigslot_mt.json
./Release/bench_rocket_st.exe    --benchmark_format=json --benchmark_out=results/rocket_st.json
./Release/bench_rocket_ts.exe    --benchmark_format=json --benchmark_out=results/rocket_ts.json
./Release/bench_nod.exe          --benchmark_format=json --benchmark_out=results/nod.json
./Release/bench_vdk_st.exe       --benchmark_format=json --benchmark_out=results/vdk_st.json
./Release/bench_vdk_ts.exe       --benchmark_format=json --benchmark_out=results/vdk_ts.json

# If Qt6 was found:
./Release/bench_qt.exe           --benchmark_format=json --benchmark_out=results/qt.json

# If libsigc++ was found:
./Release/bench_libsigcpp.exe    --benchmark_format=json --benchmark_out=results/libsigcpp.json
```

**Important:** Run benchmarks on a quiet system.  Close browsers, background apps, and antivirus scans during measurement.  Each executable runs its own warmup and timing loop internally via Google Benchmark; no special steps are needed beyond ensuring the machine is not under load.

---

## Aggregating results manually

If you ran individual executables yourself (rather than via `run_benchmarks.sh`), aggregate with:

```bash
python compare.py results/ > BENCHMARK_RESULTS.md
```

This produces a `BENCHMARK_RESULTS.md` Markdown table.  Missing libraries (those whose JSON files were not found) are silently omitted from columns.

---

## Benchmark flags

Google Benchmark accepts several useful flags:

```bash
# Run only a specific scenario:
bench_stellyra_ts.exe --benchmark_filter=BM_Emit_1Connection

# Control iteration count (default is auto-tuned):
bench_stellyra_ts.exe --benchmark_min_time=2s

# Run in-process multiple times and report min/mean/stddev:
bench_stellyra_ts.exe --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

---

## Notes on individual libraries

**vdk-signals:** Compiles `signals.cpp` into a static library at configure time.  The ST variant uses `vdk::lite`; the TS variant uses `vdk`.

**Qt:** Each `bench_qt_*` uses a custom `main()` (not `BENCHMARK_MAIN()`) because `QCoreApplication` must be created before any Qt machinery is used.  This is standard practice for Qt applications; Google Benchmark's argument handling still works normally.

**nano-signal-slot:** `TS_Policy` uses a non-reentrant mutex.  Connecting or disconnecting from within a slot under `TS_Policy` will deadlock.  The benchmarks do not exercise this path.

**Boost.Signals2:** Excluded from the default build due to dependency size.  `sigslot_st` results serve as an approximate performance lower bound for Boost.Signals2 in single-threaded scenarios.  See `boost_signals2/CMakeLists.txt` to enable.
