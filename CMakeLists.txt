cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# Read version from version.h so it is the single source of truth
# ============================================================================

file( READ "${CMAKE_CURRENT_SOURCE_DIR}/libs/stellyra/include/kmac/stellyra/version.h" _version_h )

string( REGEX MATCH "#define STELLYRA_VERSION_MAJOR_INT ([0-9]+)" _ ${_version_h} )
set( _ver_major ${CMAKE_MATCH_1} )

string( REGEX MATCH "#define STELLYRA_VERSION_MINOR_INT ([0-9]+)" _ ${_version_h} )
set( _ver_minor ${CMAKE_MATCH_1} )

string( REGEX MATCH "#define STELLYRA_VERSION_PATCH_INT ([0-9]+)" _ ${_version_h} )
set( _ver_patch ${CMAKE_MATCH_1} )

project( Stellyra
	VERSION ${_ver_major}.${_ver_minor}.${_ver_patch}
	LANGUAGES CXX
)

# ============================================================================
# Interface library target
# Header-only; no compilation units.
# ============================================================================

add_library( Stellyra INTERFACE )
add_library( Stellyra::Stellyra ALIAS Stellyra )

target_compile_features( Stellyra INTERFACE cxx_std_17 )

target_include_directories( Stellyra INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/stellyra/include>
	$<INSTALL_INTERFACE:include>
)

# ============================================================================
# StellyraExtras interface target
# Header-only; no compilation units.
# ============================================================================

add_library( StellyraExtras INTERFACE )
add_library( Stellyra::Extras ALIAS StellyraExtras )

target_compile_features( StellyraExtras INTERFACE cxx_std_17 )

target_include_directories( StellyraExtras INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/stellyra_extras/include>
	$<INSTALL_INTERFACE:include>
)

# Extras depends on core
target_link_libraries( StellyraExtras INTERFACE Stellyra )

# ============================================================================
# Optional compile-time feature flags
#
# Consumers can override these via target_compile_definitions or by passing
# -DSTELLYRA_ENABLE_THREAD=0 / -DSTELLYRA_LOG_CONNECTION_ISSUES=1 etc. on the
# command line.  The defaults match the library's out-of-the-box behaviour.
# ============================================================================

# Enable thread safety throughout the library (mutexes on all shared state).
# Disable for single-threaded, bare-metal, or RTOS targets where std::mutex
# is unavailable or undesirable.  When OFF, all platform::Mutex and
# platform::SharedMutex instances are zero-overhead no-op stubs and
# <mutex>/<shared_mutex> are not included.
option( STELLYRA_THREAD_SAFE "Enable thread-safe locking throughout Stellyra" ON )
if( NOT STELLYRA_THREAD_SAFE )
	target_compile_definitions( Stellyra INTERFACE STELLYRA_THREAD_SAFE=0 )
endif()

# Enable std::thread-based auto-processed EventLoop (disable for bare-metal)
option( STELLYRA_ENABLE_THREAD "Enable std::thread support in EventLoop" ON )
if( NOT STELLYRA_ENABLE_THREAD )
	target_compile_definitions( Stellyra INTERFACE STELLYRA_ENABLE_THREAD=0 )
endif()

# Enable connection misconfiguration warnings (off by default)
option( STELLYRA_LOG_CONNECTION_ISSUES "Enable connection misconfiguration warnings" OFF )
if( STELLYRA_LOG_CONNECTION_ISSUES )
	target_compile_definitions( Stellyra INTERFACE STELLYRA_LOG_CONNECTION_ISSUES=1 )
endif()

# ============================================================================
# Optional subdirectories
# ============================================================================

option( STELLYRA_BUILD_EXAMPLES "Build Stellyra examples" OFF )
if( STELLYRA_BUILD_EXAMPLES )
	add_subdirectory( examples )
endif()

option( STELLYRA_BUILD_TESTS "Build Stellyra tests" OFF )
if( STELLYRA_BUILD_TESTS )
	add_subdirectory( tests )
endif()

option( STELLYRA_BUILD_BENCHMARKS "Build Stellyra benchmarks" OFF )
if( STELLYRA_BUILD_BENCHMARKS )
	add_subdirectory( benchmarks )
endif()

# ============================================================================
# Install rules
# ============================================================================

include( GNUInstallDirs )
include( CMakePackageConfigHelpers )

install(
	TARGETS Stellyra StellyraExtras
	EXPORT  StellyraTargets
)

install(
	DIRECTORY libs/stellyra/include/
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
	DIRECTORY libs/stellyra_extras/include/
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
	EXPORT      StellyraTargets
	FILE        StellyraTargets.cmake
	NAMESPACE   Stellyra::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Stellyra
)

# Generate StellyraConfigVersion.cmake
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/StellyraConfigVersion.cmake"
	VERSION       ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)

# Generate StellyraConfig.cmake from the template in the cmake directory
configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/StellyraConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/StellyraConfig.cmake"
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Stellyra
)

install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/StellyraConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/StellyraConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Stellyra
)
