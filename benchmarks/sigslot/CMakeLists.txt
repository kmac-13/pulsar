cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# sigslot (palacaze)
#
# Header-only; provides a CMake target (Pal::Sigslot) via its own
# CMakeLists.txt when fetched.
#
# bench_sigslot_st - sigslot::signal_st (no locking)
# bench_sigslot_mt - sigslot::signal    (mutex-protected)
# ============================================================================

include( FetchContent )

FetchContent_Declare(
	sigslot
	GIT_REPOSITORY https://github.com/palacaze/sigslot.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
)

# Disable sigslot's own tests and examples to keep the build fast.
set( SIGSLOT_COMPILE_EXAMPLES OFF CACHE BOOL "" FORCE )
set( SIGSLOT_COMPILE_TESTS    OFF CACHE BOOL "" FORCE )

FetchContent_MakeAvailable( sigslot )

# Link Pal::Sigslot directly (rather than only extracting its include dir) so
# its INTERFACE dependency on Threads::Threads is also propagated.

add_stellyra_benchmark( bench_sigslot_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_sigslot.cpp"
	# No SIGSLOT_THREAD_SAFE: uses signal_st
)
target_link_libraries( bench_sigslot_st PRIVATE Pal::Sigslot )

add_stellyra_benchmark( bench_sigslot_mt_1
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_sigslot.cpp"
	DEFINES SIGSLOT_THREAD_SAFE=1
)
target_link_libraries( bench_sigslot_mt_1 PRIVATE Pal::Sigslot )

add_stellyra_benchmark( bench_sigslot_mt_2
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_sigslot_mt_2.cpp"
	DEFINES SIGSLOT_THREAD_SAFE=1
)
target_link_libraries( bench_sigslot_mt_2 PRIVATE Pal::Sigslot )
