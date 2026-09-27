cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# rocket (tripleslash)
#
# Single-header, public-domain library.
# bench_rocket_st - rocket::signal<> (thread_unsafe_policy, the default)
# bench_rocket_ts - rocket::thread_safe_signal<> (thread_safe_policy)
#
# thread_safe_policy protects the connection list during emission but does
# not serialise handler execution across threads - see bench_rocket.cpp's
# file-level comment for what that means for Scenario 7a.
# ============================================================================

include( FetchContent )

FetchContent_Declare(
	rocket
	GIT_REPOSITORY https://github.com/tripleslash/rocket.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable( rocket )

# rocket's own CMakeLists defines an `example` target (BUILD_EXAMPLES option,
# default ON).  Setting the cache variable before FetchContent_MakeAvailable
# is unreliable across CMake/Ninja regen cycles, so instead exclude the
# target from the default `all` build directly if it exists.  It has its own
# compile issues under some MinGW/std combinations and is irrelevant to the
# benchmarks.
if( TARGET example )
	set_target_properties( example PROPERTIES EXCLUDE_FROM_ALL TRUE )
endif()

set( ROCKET_INCLUDE_DIR "${rocket_SOURCE_DIR}" )

add_pulsar_benchmark( bench_rocket_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_rocket.cpp"
	EXTRA_INCLUDES "${ROCKET_INCLUDE_DIR}"
	# No ROCKET_THREAD_SAFE: concurrent scenarios excluded
)

add_pulsar_benchmark( bench_rocket_ts_1
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_rocket.cpp"
	EXTRA_INCLUDES "${ROCKET_INCLUDE_DIR}"
	DEFINES ROCKET_THREAD_SAFE=1
)

add_pulsar_benchmark( bench_rocket_ts_2
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_rocket_ts_2.cpp"
	EXTRA_INCLUDES "${ROCKET_INCLUDE_DIR}"
	DEFINES ROCKET_THREAD_SAFE=1
)
