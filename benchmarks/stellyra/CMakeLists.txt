cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# bench_stellyra_ts - STELLYRA_THREAD_SAFE=1 (default; mutex active)
# bench_stellyra_st - STELLYRA_THREAD_SAFE=0 (no locking; single-threaded use)
#
# Both executables compile bench_stellyra.cpp with different preprocessor
# flags.  The ST executable omits the thread-safety scenarios (5, 6, 7a, 7b,
# 9) which are guarded by #if !defined(STELLYRA_THREAD_SAFE) || STELLYRA_THREAD_SAFE != 0.
# ============================================================================

add_stellyra_benchmark( bench_stellyra_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_stellyra.cpp"
	DEFINES STELLYRA_THREAD_SAFE=0
)

add_stellyra_benchmark( bench_stellyra_ts
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_stellyra.cpp"
	DEFINES STELLYRA_THREAD_SAFE=1
)

add_stellyra_benchmark( bench_stellyra_mutex_variants
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_stellyra_mutex_variants.cpp"
)

# ============================================================================
# bench_stellyra_dispatch_gate_cost - Isolates the sender-loop dispatch-gate
# cost in BasicEvent::operator() (shouldDispatchDirectlyOnThread() and its
# platform::Atomic<ThreadId> reads).  Useful if the sender-loop deferral
# path is modified.
# ============================================================================

add_stellyra_benchmark( bench_stellyra_dispatch_gate_cost
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_stellyra_dispatch_gate_cost.cpp"
)
