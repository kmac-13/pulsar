cmake_minimum_required( VERSION 3.16 )
project( PulsarBenchmarks )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

if( WIN32 )
	add_definitions( -DNOMINMAX -DWIN32_LEAN_AND_MEAN )
endif()

# ============================================================================
# Pulsar include paths
#
# Benchmarks are run from the benchmarks/ subdirectory of the repo, so
# the Pulsar headers are two levels up.  Adjust PULSAR_ROOT if you build
# from a different location.
# ============================================================================

set( PULSAR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.."
	CACHE PATH "Root of the Pulsar repository" )

set( PULSAR_INCLUDE_DIR   "${PULSAR_ROOT}/libs/pulsar/include" )
set( PULSAR_EXTRAS_INCLUDE_DIR "${PULSAR_ROOT}/libs/pulsar_extras/include" )

# ============================================================================
# Google Benchmark
#
# Mirrors the pattern used by the Pulsar test suite: point at a local clone
# rather than using FetchContent, to keep the build reproducible offline and
# avoid TLS / threading issues that can surface when GTest drives benchmarks.
#
# Expected location: <repo-root>/../google-benchmark/
# Override with -DBENCHMARK_ROOT=<path> if yours is elsewhere.
# ============================================================================

set( BENCHMARK_ROOT "${PULSAR_ROOT}/../../googlebenchmark"
	CACHE PATH "Root of the google/benchmark repository" )

set( BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE )
set( BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE )
set( BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE )

add_subdirectory( "${BENCHMARK_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark" )

# ============================================================================
# Shared helper function
#
# Creates a benchmark executable with the common include paths and link
# libraries already wired in.  Per-library CMakeLists call this rather than
# repeating the boilerplate.
#
# Usage:
#   add_pulsar_benchmark( <target> <source> [EXTRA_INCLUDES dir...] [DEFINES def...] )
# ============================================================================

function( add_pulsar_benchmark target_name source_file )
	cmake_parse_arguments( ARG "" "" "EXTRA_INCLUDES;DEFINES" ${ARGN} )

	add_executable( ${target_name} ${source_file} )

	target_include_directories( ${target_name} PRIVATE
		"${PULSAR_INCLUDE_DIR}"
		"${PULSAR_EXTRAS_INCLUDE_DIR}"
		"${CMAKE_CURRENT_SOURCE_DIR}/../common"
		${ARG_EXTRA_INCLUDES}
	)

	target_link_libraries( ${target_name} PRIVATE benchmark::benchmark )

	if( WIN32 )
		# Google Benchmark on Windows needs Shlwapi for some registry queries.
		target_link_libraries( ${target_name} PRIVATE Shlwapi )
	endif()

	if( UNIX )
		target_link_libraries( ${target_name} PRIVATE pthread )
	endif()

	foreach( def IN LISTS ARG_DEFINES )
		target_compile_definitions( ${target_name} PRIVATE ${def} )
	endforeach()

	target_compile_features( ${target_name} PRIVATE cxx_std_17 )

	if( CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" )
		target_compile_options( ${target_name} PRIVATE -Wall -Wextra -O2 )
	elseif( MSVC )
		target_compile_options( ${target_name} PRIVATE /W4 /O2 /wd4127 )
	endif()
endfunction()

# ============================================================================
# Library-agnostic benchmarks
# ============================================================================

# add_subdirectory( segmented_queue )

add_subdirectory( bench_platform )

# ============================================================================
# Per-library subdirectories
# ============================================================================

add_subdirectory( pulsar )
add_subdirectory( nano_signal_slot )
add_subdirectory( sigslot )
add_subdirectory( rocket )
add_subdirectory( nod )
add_subdirectory( vdk_signals )

# libsigc++ requires a system install; built only when pkg-config finds it.
add_subdirectory( libsigcpp )

# Qt requires a local Qt6 installation; built only when Qt6 is found.
add_subdirectory( qt )

# Boost.Signals2: scaffolding present but disabled by default.
# See boost_signals2/CMakeLists.txt for enable instructions.
add_subdirectory( boost_signals2 )

# ============================================================================
# Results script
# ============================================================================

# The compare.py script is not a build target; it lives in benchmarks/ and
# is run manually after collecting JSON output from all executables.
# See benchmarks/RUNNING.md for the workflow.
