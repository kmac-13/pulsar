cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# bench_pulsar_ts - PULSAR_THREAD_SAFE=1 (default; mutex active)
# bench_pulsar_st - PULSAR_THREAD_SAFE=0 (no locking; single-threaded use)
#
# Both executables compile bench_pulsar.cpp with different preprocessor
# flags.  The ST executable omits the thread-safety scenarios (5, 6, 7a, 7b,
# 9) which are guarded by #if !defined(PULSAR_THREAD_SAFE) || PULSAR_THREAD_SAFE != 0.
# ============================================================================

add_pulsar_benchmark( bench_pulsar_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_pulsar.cpp"
	DEFINES PULSAR_THREAD_SAFE=0
)

add_pulsar_benchmark( bench_pulsar_ts
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_pulsar.cpp"
	DEFINES PULSAR_THREAD_SAFE=1
)

add_pulsar_benchmark( bench_pulsar_mutex_variants
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_pulsar_mutex_variants.cpp"
)

# ============================================================================
# bench_pulsar_dispatch_gate_cost - Isolates the sender-loop dispatch-gate
# cost in BasicEvent::operator() (shouldDispatchDirectlyOnThread() and its
# platform::Atomic<ThreadId> reads).  Useful if the sender-loop deferral
# path is modified.
# ============================================================================

add_pulsar_benchmark( bench_pulsar_dispatch_gate_cost
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_pulsar_dispatch_gate_cost.cpp"
)
