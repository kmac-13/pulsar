cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# libsigc++
#
# libsigc++'s own .cc files are compiled as direct sources of the
# bench_libsigcpp executable itself - one add_executable() call, no
# separate library target at all (not even a static one).  Everything
# lands in a single translation-unit set, same as every other
# single-source benchmark in this project, just with a few extra sources.
#
# This deliberately bypasses libsigc++'s own CMakeLists.txt entirely (and
# its unconditional add_subdirectory(tests)/add_subdirectory(examples),
# which we don't want either) rather than trying to talk it into building
# differently than it's written to - its own CMakeLists.txt hardcodes a
# SHARED library with no static-build toggle.
#
# LIBSIGCXX_STATIC is defined for the whole target rather than SIGC_BUILD
# for just libsigc++'s own files.  Per sigc++config.h.cmake: SIGC_API (the
# dllexport/dllimport macro) only does anything if SIGC_DLL is defined, and
# SIGC_DLL is only ever auto-defined under `#if defined (_MSC_VER)`,
# guarded by `#ifndef LIBSIGCXX_STATIC`.  Defining LIBSIGCXX_STATIC for
# every file in this target (harmless for bench_libsigcpp.cpp itself,
# which never references the macro) guarantees SIGC_API is empty
# everywhere, on every compiler - the semantically correct state given
# there's no DLL boundary anywhere in this build, not just a MinGW-specific
# accident (SIGC_DLL already happens to never be defined under
# `__MINGW32__` either way, but this makes it robust for a hypothetical
# MSVC build of the same layout too).
#
# Set LIBSIGCPP_SOURCE_DIR to the root of your libsigc++ checkout - the
# directory containing its own top-level CMakeLists.txt.
#
# bench_libsigcpp - scenarios 1, 2, 2c, 3, 4, and 8b (approximate); see
# bench_libsigcpp.cpp's own header for exactly what's covered and what
# isn't (no thread safety, no deferred dispatch, no 8a)
#
# Gracefully skipped when LIBSIGCPP_SOURCE_DIR isn't set.
# ============================================================================

# NOTE: This is a relative path to a version of libsigc++ obtained from github.
# Replace LIBSIGCPP_SOURCE_DIR with the correct path for your environment, or
# don't define it at all to disable libsigc++ benchmarks.
set( LIBSIGCPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../libsigcpp/libsigcplusplus-master" CACHE PATH
#set( LIBSIGCPP_SOURCE_DIR "" CACHE PATH
	"Path to a local libsigc++ source checkout (the directory containing its own top-level CMakeLists.txt)" )

if( LIBSIGCPP_SOURCE_DIR STREQUAL "" )
	message( STATUS "bench_libsigcpp: skipped (LIBSIGCPP_SOURCE_DIR not set - "
		"pass -DLIBSIGCPP_SOURCE_DIR=<path to your libsigc++ checkout>)" )
	return()
endif()

if( NOT EXISTS "${LIBSIGCPP_SOURCE_DIR}/sigc++config.h.cmake" )
	message( STATUS "bench_libsigcpp: skipped (no sigc++config.h.cmake found "
		"at ${LIBSIGCPP_SOURCE_DIR} - check LIBSIGCPP_SOURCE_DIR points at "
		"the repository root, not a subdirectory)" )
	return()
endif()

message( STATUS "bench_libsigcpp: compiling libsigc++ sources directly into "
	"bench_libsigcpp from ${LIBSIGCPP_SOURCE_DIR}" )

# Same version numbers libsigc++'s own top-level CMakeLists.txt sets -
# needed by sigc++config.h.cmake's @...@ substitutions. Update these if you
# ever point LIBSIGCPP_SOURCE_DIR at a different libsigc++ release.
set( SIGCXX_MAJOR_VERSION 3 )
set( SIGCXX_MINOR_VERSION 8 )
set( SIGCXX_MICRO_VERSION 1 )

configure_file(
	"${LIBSIGCPP_SOURCE_DIR}/sigc++config.h.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/sigc++config.h"
	@ONLY
)

# Matches SOURCE_FILES in libsigc++'s own sigc++/CMakeLists.txt exactly.
# Listed explicitly rather than globbed, matching this project's own
# convention elsewhere (and so a future libsigc++ release adding a new
# source file doesn't silently go unbuilt).
set( SIGCPP_SOURCES
	"${LIBSIGCPP_SOURCE_DIR}/sigc++/connection.cc"
	"${LIBSIGCPP_SOURCE_DIR}/sigc++/scoped_connection.cc"
	"${LIBSIGCPP_SOURCE_DIR}/sigc++/signal_base.cc"
	"${LIBSIGCPP_SOURCE_DIR}/sigc++/trackable.cc"
	"${LIBSIGCPP_SOURCE_DIR}/sigc++/functors/slot_base.cc"
)

# A single list combining the benchmark source with libsigc++'s sources -
# add_stellyra_benchmark's source_file parameter accepts this unmodified:
# CMake splits any unquoted semicolon-separated list into multiple
# add_executable() sources automatically, so every file below becomes its
# own translation unit in the SAME target, verified directly.
set( BENCH_LIBSIGCPP_ALL_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_libsigcpp.cpp"
	${SIGCPP_SOURCES}
)

add_stellyra_benchmark( bench_libsigcpp
	"${BENCH_LIBSIGCPP_ALL_SOURCES}"
	EXTRA_INCLUDES
		"${LIBSIGCPP_SOURCE_DIR}"
		"${CMAKE_CURRENT_BINARY_DIR}"
	DEFINES LIBSIGCXX_STATIC
)
