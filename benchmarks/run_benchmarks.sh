#!/usr/bin/env bash
#
# run_benchmarks.sh - run all Pulsar benchmark executables and aggregate
#                       results into BENCHMARK_RESULTS.md
#
# Usage:
#   ./run_benchmarks.sh [build-dir]
#
# build-dir defaults to the most recently modified directory under build/
# (e.g. build/Desktop_Qt_6_9_0_MinGW_64_bit-Release).  Pass it explicitly to
# be safe:
#
#   ./run_benchmarks.sh build/Desktop_Qt_6_9_0_MinGW_64_bit-Release
#
# Run from the benchmarks/ directory (where compare.py lives).
#
# Each executable is run with --benchmark_format=json and its output written
# to results/<name>.json.  Executables that were not built (e.g. bench_qt or
# bench_libsigcpp when their dependencies weren't found) are skipped with a
# warning rather than failing the whole run.
#
# At the end, compare.py aggregates results/ into BENCHMARK_RESULTS.md.

set -u

# ----------------------------------------------------------------------------
# Resolve build directory
# ----------------------------------------------------------------------------

if [ $# -ge 1 ]; then
	BUILD_DIR="$1"
else
	# Pick the most recently modified subdirectory of build/
	BUILD_DIR=$(ls -dt build/*/ 2>/dev/null | head -1)
	if [ -z "$BUILD_DIR" ]; then
		echo "Error: no build directory found under build/, and none specified." >&2
		echo "Usage: $0 [build-dir]" >&2
		exit 1
	fi
	BUILD_DIR="${BUILD_DIR%/}"
fi

if [ ! -d "$BUILD_DIR" ]; then
	echo "Error: build directory not found: $BUILD_DIR" >&2
	exit 1
fi

echo "Using build directory: $BUILD_DIR"

# ----------------------------------------------------------------------------
# Qt runtime DLLs
#
# bench_qt.exe dynamically links against Qt6Core.dll (and friends). Qt
# Creator puts Qt's bin/ directory on PATH automatically when launching a
# run configuration, but a plain shell invocation - like this script - does
# not, so bench_qt.exe fails with "error while loading shared
# libraries: Qt6Core.dll: cannot open shared object file: No such file or
# directory" even though the build itself is fine.
#
# Set QT_BIN_DIR explicitly to skip auto-detection, e.g.:
#   QT_BIN_DIR=/c/Qt/6.9.0/mingw_64/bin ./run_benchmarks.sh
# ----------------------------------------------------------------------------

find_qt_bin_dir()
{
	local cache_file="$BUILD_DIR/CMakeCache.txt"
	[ -f "$cache_file" ] || return 1

	# Look for a Qt6*_DIR cache entry, e.g.:
	#   Qt6_DIR:PATH=C:/Qt/6.9.0/mingw_64/lib/cmake/Qt6
	#   Qt6Core_DIR:PATH=C:/Qt/6.9.0/mingw_64/lib/cmake/Qt6Core
	local qt_cmake_dir
	qt_cmake_dir=$(grep -oE '^Qt6[A-Za-z]*_DIR:PATH=.*' "$cache_file" | head -1 | cut -d= -f2-)
	[ -n "$qt_cmake_dir" ] || return 1

	# .../<qt-root>/lib/cmake/Qt6[Core] -> strip back to <qt-root>/bin
	local qt_root="${qt_cmake_dir%/lib/cmake/*}"
	[ -d "$qt_root/bin" ] || return 1

	printf '%s\n' "$qt_root/bin"
}

to_posix_path()
{
	# Prefer cygpath (ships with MSYS2/Git-Bash/Cygwin) - it's the
	# authoritative converter and handles edge cases a regex can't.
	if command -v cygpath >/dev/null 2>&1; then
		cygpath -u "$1"
		return
	fi

	# Fallback: manual C:/... -> /c/... conversion for shells without
	# cygpath. Only handles the common drive-letter-prefixed case.
	printf '%s\n' "$1" | sed -E 's#^([A-Za-z]):#/\L\1#'
}

QT_BIN_DIR="${QT_BIN_DIR:-}"
if [ -z "$QT_BIN_DIR" ]; then
	QT_BIN_DIR=$(find_qt_bin_dir)
fi

if [ -n "$QT_BIN_DIR" ] && [ -d "$QT_BIN_DIR" ]; then
	# Convert to POSIX form before touching PATH - see comment above.
	# Windows-style "C:/Qt/.../bin" has a colon that collides with PATH's
	# own separator once MSYS translates PATH for a native child process,
	# silently dropping the entry even though it looks correct here.
	QT_BIN_DIR_POSIX=$(to_posix_path "$QT_BIN_DIR")
	echo "Using Qt bin directory: $QT_BIN_DIR (as $QT_BIN_DIR_POSIX on PATH)"
	export PATH="$QT_BIN_DIR_POSIX:$PATH"
else
	echo "Warning: could not locate the Qt bin directory automatically." >&2
	echo "         If qt/bench_qt fails with a missing Qt6Core.dll error," >&2
	echo "         set QT_BIN_DIR to your Qt bin folder and re-run, e.g.:" >&2
	echo "           QT_BIN_DIR=/c/Qt/6.9.0/mingw_64/bin ./run_benchmarks.sh" >&2
fi

# ----------------------------------------------------------------------------
# MinGW toolchain runtime DLLs (libstdc++-6.dll, libgcc_s_seh-1.dll,
# libwinpthread-1.dll)
#
# Qt ships its own copies of these three under QT_BIN_DIR above, but that
# copy isn't necessarily the one bench_vdk_ts_contention.exe (and
# bench_vdk_ts_perreceiver.exe) end up loading at runtime - if another
# MinGW-w64 install's bin directory (MSYS2, a different Qt Tools version,
# etc.) also happens to be on PATH ahead of Qt's, Windows' DLL search order
# can resolve one or more of the three to that other install instead,
# silently mixing runtime library versions with the toolchain these
# executables were actually compiled against. Confirmed to matter in
# practice: explicitly putting the exact compiler toolchain's own bin/ (the
# one named in CMakeCache.txt's CMAKE_CXX_COMPILER entry, e.g.
# C:/Qt/Tools/mingw1310_64/bin) on PATH ahead of everything else resolved a
# heap-corruption crash (STATUS_HEAP_CORRUPTION / exit 3221226356) in
# bench_vdk_ts_contention.exe that reproduced consistently without it -
# consistent with vdk's already-documented MinGW thread_local
# destructor issue (see the SEPARATE_PROCESS_BENCHMARKS comment below)
# being sensitive to exactly which runtime DLL build ends up loaded, not
# only to which OS threads touch it.
#
# Set MINGW_BIN_DIR explicitly to skip auto-detection, e.g.:
#   MINGW_BIN_DIR=/c/Qt/Tools/mingw1310_64/bin ./run_benchmarks.sh
# ----------------------------------------------------------------------------

find_mingw_bin_dir()
{
	local cache_file="$BUILD_DIR/CMakeCache.txt"
	[ -f "$cache_file" ] || return 1

	# e.g. CMAKE_CXX_COMPILER:FILEPATH=C:/Qt/Tools/mingw1310_64/bin/g++.exe
	local compiler_path
	compiler_path=$(grep -oE '^CMAKE_CXX_COMPILER:FILEPATH=.*' "$cache_file" | head -1 | cut -d= -f2-)
	[ -n "$compiler_path" ] || return 1

	local compiler_dir="${compiler_path%/*}"
	[ -d "$compiler_dir" ] || return 1

	printf '%s\n' "$compiler_dir"
}

MINGW_BIN_DIR="${MINGW_BIN_DIR:-}"
if [ -z "$MINGW_BIN_DIR" ]; then
	MINGW_BIN_DIR=$(find_mingw_bin_dir)
fi

if [ -n "$MINGW_BIN_DIR" ] && [ -d "$MINGW_BIN_DIR" ]; then
	MINGW_BIN_DIR_POSIX=$(to_posix_path "$MINGW_BIN_DIR")
	echo "Using MinGW toolchain bin directory: $MINGW_BIN_DIR (as $MINGW_BIN_DIR_POSIX on PATH)"
	# Ahead of everything already on PATH (including QT_BIN_DIR above) so
	# this toolchain's own libstdc++-6.dll/libgcc_s_seh-1.dll/
	# libwinpthread-1.dll win DLL search order over any other MinGW
	# install's copies, and ahead of Qt's own copies for the same reason
	# - see the comment above for why that ordering is what fixed the
	# vdk contention crash.
	export PATH="$MINGW_BIN_DIR_POSIX:$PATH"
else
	echo "Warning: could not locate the MinGW toolchain bin directory automatically." >&2
	echo "         If bench_vdk_ts_contention or bench_vdk_ts_perreceiver crash with" >&2
	echo "         STATUS_HEAP_CORRUPTION (exit 3221226356), set MINGW_BIN_DIR to your" >&2
	echo "         compiler's bin folder and re-run, e.g.:" >&2
	echo "           MINGW_BIN_DIR=/c/Qt/Tools/mingw1310_64/bin ./run_benchmarks.sh" >&2
fi

# ----------------------------------------------------------------------------
# Results directory
# ----------------------------------------------------------------------------

RESULTS_DIR="results"
mkdir -p "$RESULTS_DIR"

# ----------------------------------------------------------------------------
# Executable -> output JSON name mapping
#
# Format: "<relative path under BUILD_DIR>:<results filename>[:<timeout seconds>]"
#
# The timeout field is optional - entries without one fall back to
# TIMEOUT_SECONDS (see below). Per-executable timeouts exist because this
# suite's executables vary enormously in total runtime: a full
# --benchmark_repetitions=10 run of the lightweight ones (nano, sigslot,
# vdk_st, rocket_st) finishes in well under a minute, while the heaviest
# ones (pulsar_ts, qt) now have 25-30+ BENCHMARK() registrations across many
# Arg() combinations, several with an explicit MinTime(1.5) floor per
# repetition, and comfortably exceed 300s in total. pulsar_ts's timeout was
# raised again (1800s -> 3000s) when bench_pulsar.cpp picked up an _Event
# counterpart alongside most of its existing ConcurrentEvent-backed
# concurrent/contention benchmarks (Scenarios 3, 5, 7a, 9), roughly doubling
# the thread-heavy portion of that binary. A single global timeout is
# either wastefully long for the fast executables (defeating the point of
# catching a hang quickly) or too short for the slow ones - see the
# 300s-was-too-low incident that prompted this.
# ----------------------------------------------------------------------------

BENCHMARKS=(
	"pulsar/bench_pulsar_st:pulsar_st:500"
	"pulsar/bench_pulsar_ts:pulsar_ts:4500"
	"qt/bench_qt_1:qt_1:900"
	"qt/bench_qt_2:qt_2:600"
	"qt/bench_qt_3:qt_3:1200"
	"qt/bench_qt_4:qt_4:600"
	"nano_signal_slot/bench_nano_st:nano_st:180"
	"nano_signal_slot/bench_nano_ts_1:nano_ts_1:180"
	"nano_signal_slot/bench_nano_ts_2:nano_ts_2:300"
	"sigslot/bench_sigslot_st:sigslot_st:180"
	"sigslot/bench_sigslot_mt_1:sigslot_mt_1:180"
	"sigslot/bench_sigslot_mt_2:sigslot_mt_2:300"
	"rocket/bench_rocket_st:rocket_st:180"
	"rocket/bench_rocket_ts_1:rocket_ts_1:180"
	"rocket/bench_rocket_ts_2:rocket_ts_2:600"
	"nod/bench_nod_1:nod_1:180"
	"nod/bench_nod_2:nod_2:600"
	"vdk_signals/bench_vdk_st:vdk_st:180"
	"vdk_signals/bench_vdk_ts:vdk_ts_1:180"
	"vdk_signals/bench_vdk_ts_contention:vdk_ts_contention:450"
	"vdk_signals/bench_vdk_ts_crossthread:vdk_ts_crossthread:180"
	"vdk_signals/bench_vdk_ts_threadaffinity:vdk_ts_threadaffinity:180"
	"vdk_signals/bench_vdk_ts_direct:vdk_ts_direct:180"
	"vdk_signals/bench_vdk_ts_perreceiver:vdk_ts_perreceiver:450"
	"libsigcpp/bench_libsigcpp:libsigcpp:300"
	# "boost_signals2/bench_boost:boost:300"
)

# ----------------------------------------------------------------------------
# vdk TS: six separate executables, not runtime --benchmark_filter splits
#
# Previously, all of vdk's TS benchmarks lived in one bench_vdk_ts.exe,
# split at runtime via six separate --benchmark_filter invocations from
# this script (each writing its own JSON fragment). That worked, but only
# from this script - running bench_vdk_ts.exe directly (e.g. from Qt
# Creator) meant either the full unfiltered suite (which hangs) or manually
# passing --benchmark_filter by hand each time. bench_vdk.cpp (ST/TS-shared
# scenarios) and five dedicated per-family source files
# (bench_vdk_ts_crossthread.cpp, _threadaffinity.cpp, _direct.cpp,
# _contention.cpp, _perreceiver.cpp) now produce six separate executables
# directly, so each can be run on its own with no special arguments needed
# - see the BENCHMARKS array above.
#
# The underlying reason for the split is unchanged and still a genuine,
# confirmed root cause, not a guess: vdk lazily allocates a thread_local
# channel object the first time any given OS thread touches the signal
# system, and destroys it when that thread exits. MinGW-w64's
# std::thread_local destructor support has a long-documented, still-present
# set of bugs for exactly this shape (block-scope thread_local with a
# non-trivial destructor under the POSIX threading model - see GCC
# Bugzilla #83562 and MinGW-w64 bugs #445/#527/#727/#7096, spanning
# 2014-2024). Direct evidence from bisecting this project's own hang: any
# one thread-creating benchmark family run alone, even under
# --benchmark_repetitions=10 (repeated thread creation/teardown of the
# SAME configuration, many times, in one process), completes cleanly.
# Running several DIFFERENT thread-creating families back-to-back in one
# process reliably hangs partway through - consistent with a prior,
# independently-diagnosed case in this codebase's own logging benchmarks
# (Quill's thread_local ScopedThreadContext vs Google Benchmark's thread
# pool lifecycle), where the fix was the same one applied here: give each
# distinct thread-lifetime-owning benchmark its own process.
#
# compare.py's LIBRARIES entry for "vdk TS" lists all six output filenames
# and merges their "benchmarks" arrays before aggregating, so this split is
# invisible in BENCHMARK_RESULTS.md - it appears as a single library
# column, same as before.
# ----------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Executables that need separate-process repetition, not
# --benchmark_repetitions
#
# Confirmed for two executables so far: bench_vdk_ts_contention.exe (first
# found) and bench_vdk_ts_perreceiver.exe (found via a truncated,
# invalid-JSON result surviving in results/ - see the file-cleanup fix
# above this block's comment for why that happened silently before). Both
# fail under this script's normal invocation (--benchmark_repetitions=10,
# which calls the benchmark body 10 times WITHIN ONE PROCESS) but complete
# cleanly when invoked 5 times as 5 SEPARATE process launches instead, no
# --benchmark_repetitions at all. This is a different, distinct
# manifestation from the thread_local destructor issue documented above -
# that one is about thread creation/destruction across DIFFERENT benchmark
# families sharing a process; this one is specific to repeated in-process
# invocation of these two benchmarks' own setup/teardown - but the fix has
# the same shape: give each run its own process instead of relying on
# Google Benchmark's in-process repetition.
#
# aggregate_repeated_runs.py (see that file's own docstring) generalizes
# this workaround: it runs the executable N times as N separate processes
# and synthesizes a Google-Benchmark-compatible aggregate JSON from the
# results, so compare.py needs no special-casing to consume it.
#
# Listed by out_name (not executable path) since that's what the run loop
# below matches against. Add an out_name here if a third executable turns
# out to need the same treatment - two confirmed cases is exactly the
# "worth generalizing" threshold this was originally deferred past.
# ----------------------------------------------------------------------------

SEPARATE_PROCESS_BENCHMARKS=("vdk_ts_contention" "vdk_ts_perreceiver")
SEPARATE_PROCESS_RUNS=5

needs_separate_processes()
{
	local name="$1"
	local candidate
	for candidate in "${SEPARATE_PROCESS_BENCHMARKS[@]}"; do
		if [ "$candidate" = "$name" ]; then
			return 0
		fi
	done
	return 1
}

# Windows executables have a .exe suffix; on Linux/macOS they don't.
EXE_SUFFIX=""
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "win32" || "$OSTYPE" == "cygwin" ]]; then
	EXE_SUFFIX=".exe"
fi

# ----------------------------------------------------------------------------
# Per-benchmark timeout
#
# Without this, a hung executable (e.g. a livelock/deadlock in a concurrent
# scenario) stalls the entire suite indefinitely with no indication of which
# executable is stuck - this is what happened with bench_nano_ts.exe hanging
# partway through BM_ConcurrentEmission_PerReceiver, leaving a truncated,
# invalid results/nano_ts.json (valid entries for every scenario completed
# before the hang, no closing "]}" because Finalize() never ran) that sat
# there until someone noticed and killed the process by hand.
#
# GNU coreutils `timeout` isn't reliably present on Windows/MinGW/Git Bash,
# so this is a portable background-process-plus-watchdog implementation
# instead of depending on it.
#
# TIMEOUT_SECONDS is the fallback used for any BENCHMARKS entry above that
# doesn't specify its own timeout field. Per-executable timeouts in the
# array take precedence over this when present. Override the fallback with,
# e.g.:
#   TIMEOUT_SECONDS=600 ./run_benchmarks.sh
# To change a specific executable's timeout, edit its entry in the
# BENCHMARKS array directly rather than overriding this fallback.
# ----------------------------------------------------------------------------

TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-300}"

# Runs "$@" in the background, polls once a second, and force-kills it if it
# is still alive after $1 seconds. Returns the wrapped command's real exit
# code, or 124 (matching GNU `timeout`'s convention) if it was killed for
# running too long.
run_with_timeout()
{
	local limit="$1"
	shift

	"$@" &
	local pid=$!

	local waited=0
	while kill -0 "$pid" 2>/dev/null; do
		sleep 1
		waited=$((waited + 1))
		if [ "$waited" -ge "$limit" ]; then
			kill -9 "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null
			return 124
		fi
	done

	wait "$pid"
	return $?
}

# ----------------------------------------------------------------------------
# Run each benchmark
# ----------------------------------------------------------------------------

RAN=0
SKIPPED=0

for entry in "${BENCHMARKS[@]}"; do
	IFS=':' read -r rel_path out_name entry_timeout <<< "$entry"
	exe_timeout="${entry_timeout:-$TIMEOUT_SECONDS}"

	exe="$BUILD_DIR/${rel_path}${EXE_SUFFIX}"
	out_json="$RESULTS_DIR/${out_name}.json"

	if [ ! -f "$exe" ]; then
		echo "  skip:  $rel_path (not built)"
		rm -f "$out_json"
		SKIPPED=$((SKIPPED + 1))
		continue
	fi

	# Remove any existing result before running. Without this, a failed run
	# (e.g. bench_qt.exe failing to load Qt6Core.dll) silently leaves a
	# stale JSON from an earlier successful run in place, and compare.py -
	# which only checks that the file exists, not that it's fresh - will
	# happily aggregate that old data into a report that looks current but
	# isn't. Deleting first means a failed run produces a genuinely missing
	# file, which compare.py correctly reports as N/A instead.
	rm -f "$out_json"

	if needs_separate_processes "$out_name"; then
		# See the SEPARATE_PROCESS_BENCHMARKS comment block above this loop
		# for why these specific executables are run differently from
		# everything else.
		echo "  run:   $rel_path -> $out_json (${SEPARATE_PROCESS_RUNS}x separate processes, timeout ${exe_timeout}s total)"
		run_with_timeout "$exe_timeout" \
			python3 aggregate_repeated_runs.py "$exe" "$out_json" "$SEPARATE_PROCESS_RUNS" >/dev/null
		status=$?
	else
		echo "  run:   $rel_path -> $out_json (timeout ${exe_timeout}s)"
		# Redirect stdout to avoid pipe buffer blocking on Windows/MinGW when
		# long-running benchmarks (e.g. min_time:5.0) produce large output.
		# All benchmark data is captured in the JSON file; stdout is cosmetic.
		run_with_timeout "$exe_timeout" \
			"$exe" --benchmark_format=json --benchmark_out="$out_json" --benchmark_repetitions=10 --benchmark_report_aggregates_only=true >/dev/null
		status=$?
	fi

	if [ "$status" -eq 124 ]; then
		# Killed for running too long - whatever partial JSON Google
		# Benchmark had written (results for scenarios that finished before
		# the hang, no closing "]}" since Finalize() never ran) is invalid
		# and must not be left for compare.py to trip over.
		rm -f "$out_json"
		echo "  WARNING: $rel_path timed out after ${exe_timeout}s and was killed - results/${out_name}.json is now MISSING, not stale. If this executable's normal full runtime is close to or exceeds its configured timeout, increase its entry in the BENCHMARKS array. Otherwise, investigate the hang directly: re-run this executable alone with --benchmark_filter to isolate which scenario is stuck." >&2
	elif [ "$status" -ne 0 ]; then
		rm -f "$out_json"
		echo "  WARNING: $rel_path exited with non-zero status - results/${out_name}.json is now MISSING, not stale. This scenario will show as N/A rather than silently reusing an old run." >&2
	elif [ ! -s "$out_json" ]; then
		echo "  WARNING: $rel_path exited cleanly but produced no output at $out_json" >&2
	fi

	RAN=$((RAN + 1))
done

echo ""
echo "Ran $RAN benchmark(s), skipped $SKIPPED (not built)."

if [ "$RAN" -eq 0 ]; then
	echo "Error: no benchmarks were run; nothing to aggregate." >&2
	exit 1
fi

# ----------------------------------------------------------------------------
# Aggregate
# ----------------------------------------------------------------------------

echo ""
echo "Aggregating results -> BENCHMARK_RESULTS.md"

python compare.py "$RESULTS_DIR" > BENCHMARK_RESULTS.md

if [ $? -eq 0 ]; then
	echo "Done. See BENCHMARK_RESULTS.md"
else
	echo "Error: compare.py failed." >&2
	exit 1
fi
