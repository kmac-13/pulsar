cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# bench_platform - Generic, library-agnostic diagnostics and micro-
# benchmarks.  Nothing here compares against another signal/event library
# or feeds compare.py; these are one-off probes of the platform/toolchain
# itself (atomic lock-freedom, raw mutex cost, etc.), useful when deciding
# how to configure Pulsar for a given target rather than when comparing
# Pulsar's results against anyone else's.
# ============================================================================

add_pulsar_benchmark( platform_atomic_check
	"${CMAKE_CURRENT_SOURCE_DIR}/atomic_check.cpp"
)

add_pulsar_benchmark( platform_mutex_comparison
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_mutex_comparison.cpp"
)

add_pulsar_benchmark( platform_windows_srwlock_bench
	"${CMAKE_CURRENT_SOURCE_DIR}/windows_srwlock_bench.cpp"
)
