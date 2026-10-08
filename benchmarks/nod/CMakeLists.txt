cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# nod
#
# Single-header, no base class requirement.  multithread_policy (nod's
# default nod::signal) protects the connection list during emission but does
# not serialise handler execution across threads - see bench_nod_2.cpp's
# file-level comment for what that means for Scenario 7a.
# bench_nod_1 - scenarios 1, 2, 2c, 3, 4
# bench_nod_2 - scenario 7a (two variants: receiver-deferred and
# execute-now-serialised), scenario 9
# ============================================================================

include( FetchContent )

FetchContent_Declare(
	nod
	GIT_REPOSITORY https://github.com/fr00b0/nod.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable( nod )

set( NOD_INCLUDE_DIR "${nod_SOURCE_DIR}/include" )

add_stellyra_benchmark( bench_nod_1
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_nod_1.cpp"
	EXTRA_INCLUDES "${NOD_INCLUDE_DIR}"
)

add_stellyra_benchmark( bench_nod_2
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_nod_2.cpp"
	EXTRA_INCLUDES "${NOD_INCLUDE_DIR}"
)
