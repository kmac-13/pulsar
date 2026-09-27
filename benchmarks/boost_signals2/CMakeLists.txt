cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# Boost.Signals2 benchmark scaffold
#
# STATUS: Disabled by default.  See bench_boost.cpp for enable instructions.
#
# To enable:
#   1. Install Boost (vcpkg install boost-signals2, or add Boost to
#      CMAKE_PREFIX_PATH manually).
#   2. Uncomment the block below.
#   3. Uncomment the implementation in bench_boost.cpp.
#
# Performance reference:
#   sigslot (palacaze) is designed as a Boost.Signals2 replacement and has
#   broadly comparable single-threaded performance.  In the absence of
#   Boost.Signals2 benchmark results, bench_sigslot_st serves as an
#   approximate lower bound for the Boost figure.  See
#   COMPREHENSIVE_LIBRARY_COMPARISON.md, section 13 "Notable Differentiators"
#   for the features that distinguish Boost.Signals2 from sigslot.
# ============================================================================

message( STATUS "bench_boost: disabled (Boost.Signals2 excluded from default build)" )

# Uncomment to enable once Boost is installed:
#
# find_package( Boost QUIET COMPONENTS system )
#
# if( NOT Boost_FOUND )
#     message( STATUS "bench_boost: skipped (Boost not found)" )
#     message( STATUS "  vcpkg install boost-signals2" )
#     message( STATUS "  Then re-run CMake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake" )
#     return()
# endif()
#
# message( STATUS "bench_boost: found Boost ${Boost_VERSION}" )
#
# add_pulsar_benchmark( bench_boost
#     "${CMAKE_CURRENT_SOURCE_DIR}/bench_boost.cpp"
#     EXTRA_INCLUDES "${Boost_INCLUDE_DIRS}"
# )
# target_link_libraries( bench_boost PRIVATE ${Boost_LIBRARIES} )
