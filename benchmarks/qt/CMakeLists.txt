cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# Qt6 benchmark
#
# Requires an existing Qt6 installation.  Set CMAKE_PREFIX_PATH to your Qt
# install root if CMake cannot find it automatically, e.g.:
#   -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2019_64"
#
# bench_qt_1 - scenarios 1, 2, 2a, 2b, 2c, 3, 4, 8a, 8b
# bench_qt_2 - scenarios 5, 6
# bench_qt_3 - scenarios 7a (receiver-deferred), 7b
# bench_qt_4 - scenarios 7a (execute-now, both variants), 9
#
# Gracefully skipped when Qt6 Core is not found.
#
# MOC note: qt_bench_common.h defines the Q_OBJECT classes shared by all
# four bench_qt_*.cpp files.  AUTOMOC must be ON so CMake generates and
# compiles the moc output for each target that includes it.
# ============================================================================

find_package( Qt6 COMPONENTS Core QUIET )

if( NOT Qt6_FOUND )
	message( STATUS "bench_qt: skipped (Qt6 not found)" )
	message( STATUS "  Set CMAKE_PREFIX_PATH to your Qt6 install root and re-run CMake" )
	return()
endif()

message( STATUS "bench_qt: found Qt6 ${Qt6_VERSION}" )

add_executable( bench_qt_1
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_qt_1.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/qt_bench_common.h"
)
add_executable( bench_qt_2
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_qt_2.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/qt_bench_common.h"
)
add_executable( bench_qt_3
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_qt_3.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/qt_bench_common.h"
)
add_executable( bench_qt_4
	"${CMAKE_CURRENT_SOURCE_DIR}/bench_qt_4.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/qt_bench_common.h"
)

# Listing the header as a source above isn't strictly required for AUTOMOC in
# to find it all environments (it scans #includes of each target's .cpp
# sources regardless), but it's added so IDEs show the header as part of
# each target.
set_target_properties( bench_qt_1 PROPERTIES AUTOMOC ON )
set_target_properties( bench_qt_2 PROPERTIES AUTOMOC ON )
set_target_properties( bench_qt_3 PROPERTIES AUTOMOC ON )
set_target_properties( bench_qt_4 PROPERTIES AUTOMOC ON )

target_include_directories( bench_qt_1 PRIVATE
	"${PULSAR_INCLUDE_DIR}"
	"${PULSAR_EXTRAS_INCLUDE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../common"
)
target_include_directories( bench_qt_2 PRIVATE
	"${PULSAR_INCLUDE_DIR}"
	"${PULSAR_EXTRAS_INCLUDE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../common"
)
target_include_directories( bench_qt_3 PRIVATE
	"${PULSAR_INCLUDE_DIR}"
	"${PULSAR_EXTRAS_INCLUDE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../common"
)
target_include_directories( bench_qt_4 PRIVATE
	"${PULSAR_INCLUDE_DIR}"
	"${PULSAR_EXTRAS_INCLUDE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/../common"
)

target_link_libraries( bench_qt_1 PRIVATE
	Qt6::Core
	benchmark::benchmark
)
target_link_libraries( bench_qt_2 PRIVATE
	Qt6::Core
	benchmark::benchmark
)
target_link_libraries( bench_qt_3 PRIVATE
	Qt6::Core
	benchmark::benchmark
)
target_link_libraries( bench_qt_4 PRIVATE
	Qt6::Core
	benchmark::benchmark
)

if( WIN32 )
	target_link_libraries( bench_qt_1 PRIVATE Shlwapi )
	target_link_libraries( bench_qt_2 PRIVATE Shlwapi )
	target_link_libraries( bench_qt_3 PRIVATE Shlwapi )
	target_link_libraries( bench_qt_4 PRIVATE Shlwapi )
endif()

target_compile_features( bench_qt_1 PRIVATE cxx_std_17 )
target_compile_features( bench_qt_2 PRIVATE cxx_std_17 )
target_compile_features( bench_qt_3 PRIVATE cxx_std_17 )
target_compile_features( bench_qt_4 PRIVATE cxx_std_17 )

if( MSVC )
	target_compile_options( bench_qt_1 PRIVATE /W4 /O2 /wd4127 )
	target_compile_options( bench_qt_2 PRIVATE /W4 /O2 /wd4127 )
	target_compile_options( bench_qt_3 PRIVATE /W4 /O2 /wd4127 )
	target_compile_options( bench_qt_4 PRIVATE /W4 /O2 /wd4127 )
else()
	target_compile_options( bench_qt_1 PRIVATE -Wall -Wextra -O2 )
	target_compile_options( bench_qt_2 PRIVATE -Wall -Wextra -O2 )
	target_compile_options( bench_qt_3 PRIVATE -Wall -Wextra -O2 )
	target_compile_options( bench_qt_4 PRIVATE -Wall -Wextra -O2 )
endif()

if( WIN32 )
	add_definitions( -DNOMINMAX -DWIN32_LEAN_AND_MEAN )
endif()
