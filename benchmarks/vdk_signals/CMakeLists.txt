cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# vdk-signals (vdksoft)
#
# Two-file distribution: signals.h + signals.cpp.
# Not header-only; signals.cpp must be compiled into a static library.
#
# bench_vdk_st - vdk::lite namespace (no locking)
# bench_vdk_ts - vdk namespace       (thread-safe)
# ============================================================================

include( FetchContent )

FetchContent_Declare(
	vdk_signals
	GIT_REPOSITORY https://github.com/vdksoft/signals.git
	GIT_TAG        master
	GIT_SHALLOW    TRUE
)

# Populate only - do NOT use FetchContent_MakeAvailable here.
# vdk-signals' own CMakeLists.txt uses ${CMAKE_SOURCE_DIR}/src/signals.* (the
# *top-level* source dir) instead of ${CMAKE_CURRENT_SOURCE_DIR}, so when
# add_subdirectory'd from here, it resolves against benchmarks/src/ (which
# doesn't exist) rather than its own fetched src/.  Populating without adding
# the subdirectory avoids running that broken CMakeLists entirely; we compile
# signals.cpp ourselves below using the correct path.
FetchContent_GetProperties( vdk_signals )
if( NOT vdk_signals_POPULATED )
	FetchContent_Populate( vdk_signals )
endif()

set( VDK_SOURCE_DIR "${vdk_signals_SOURCE_DIR}/src" )

add_library( vdk_signals_lib STATIC
	"${VDK_SOURCE_DIR}/signals.cpp"
)
target_include_directories( vdk_signals_lib PUBLIC "${VDK_SOURCE_DIR}" )
target_compile_features( vdk_signals_lib PUBLIC cxx_std_17 )

find_package( Threads REQUIRED )
target_link_libraries( vdk_signals_lib PUBLIC Threads::Threads )

if( MSVC )
	target_compile_options( vdk_signals_lib PRIVATE /O2 )
else()
	target_compile_options( vdk_signals_lib PRIVATE -O2 )
endif()

# ST variant (vdk::lite)

add_stellyra_benchmark( bench_vdk_st
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk.cpp"
	# No VDK_THREAD_SAFE: bench uses vdk::lite namespace
)
target_link_libraries( bench_vdk_st PRIVATE vdk_signals_lib )

# TS variant (vdk namespace)

add_stellyra_benchmark( bench_vdk_ts
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts PRIVATE vdk_signals_lib )

# additional TS benchmarks

add_stellyra_benchmark( bench_vdk_ts_contention
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk_ts_contention.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts_contention PRIVATE vdk_signals_lib )

add_stellyra_benchmark( bench_vdk_ts_crossthread
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk_ts_crossthread.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts_crossthread PRIVATE vdk_signals_lib )

add_stellyra_benchmark( bench_vdk_ts_direct
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk_ts_direct.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts_direct PRIVATE vdk_signals_lib )

add_stellyra_benchmark( bench_vdk_ts_perreceiver
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk_ts_perreceiver.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts_perreceiver PRIVATE vdk_signals_lib )

add_stellyra_benchmark( bench_vdk_ts_threadaffinity
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_vdk_ts_threadaffinity.cpp"
	DEFINES VDK_THREAD_SAFE=1
)
target_link_libraries( bench_vdk_ts_threadaffinity PRIVATE vdk_signals_lib )
