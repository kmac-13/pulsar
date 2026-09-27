cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# nano-signal-slot
#
# Single-header library (fetched from GitHub).
# bench_nano_st - Nano::ST_Policy (no locking)
# bench_nano_ts - Nano::TS_Policy (mutex-protected)
# ============================================================================

include( FetchContent )

FetchContent_Declare(
	nano_signal_slot
	GIT_REPOSITORY https://github.com/NoAvailableAlias/nano-signal-slot.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable( nano_signal_slot )

# nano-signal-slot is header-only; the FetchContent source dir is the include root.
set( NANO_INCLUDE_DIR "${nano_signal_slot_SOURCE_DIR}" )

add_pulsar_benchmark( bench_nano_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_nano.cpp"
	EXTRA_INCLUDES "${NANO_INCLUDE_DIR}"
	# No NANO_THREAD_SAFE: defaults to ST_Policy
)

add_pulsar_benchmark( bench_nano_ts_1
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_nano.cpp"
	EXTRA_INCLUDES "${NANO_INCLUDE_DIR}"
	DEFINES NANO_THREAD_SAFE=1
)

add_pulsar_benchmark( bench_nano_ts_2
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_nano_ts_2.cpp"
	EXTRA_INCLUDES "${NANO_INCLUDE_DIR}"
	DEFINES NANO_THREAD_SAFE=1
)
