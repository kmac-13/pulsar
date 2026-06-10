cmake_minimum_required( VERSION 3.16 )

# ============================================================================
# Read version from version.h so it is the single source of truth
# ============================================================================

file( READ "${CMAKE_CURRENT_SOURCE_DIR}/libs/pulsar/include/kmac/pulsar/version.h" _version_h )

string( REGEX MATCH "#define PULSAR_VERSION_MAJOR_INT ([0-9]+)" _ ${_version_h} )
set( _ver_major ${CMAKE_MATCH_1} )

string( REGEX MATCH "#define PULSAR_VERSION_MINOR_INT ([0-9]+)" _ ${_version_h} )
set( _ver_minor ${CMAKE_MATCH_1} )

string( REGEX MATCH "#define PULSAR_VERSION_PATCH_INT ([0-9]+)" _ ${_version_h} )
set( _ver_patch ${CMAKE_MATCH_1} )

project( Pulsar
	VERSION ${_ver_major}.${_ver_minor}.${_ver_patch}
	LANGUAGES CXX
)

# ============================================================================
# Interface library target
# Header-only; no compilation units.
# ============================================================================

add_library( Pulsar INTERFACE )
add_library( Pulsar::Pulsar ALIAS Pulsar )

target_compile_features( Pulsar INTERFACE cxx_std_17 )

target_include_directories( Pulsar INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/pulsar/include>
	$<INSTALL_INTERFACE:include>
)

# ============================================================================
# PulsarExtras interface target
# Header-only; no compilation units.
# ============================================================================

add_library( PulsarExtras INTERFACE )
add_library( Pulsar::Extras ALIAS PulsarExtras )

target_compile_features( PulsarExtras INTERFACE cxx_std_17 )

target_include_directories( PulsarExtras INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libs/pulsar_extras/include>
	$<INSTALL_INTERFACE:include>
)

# Extras depends on core
target_link_libraries( PulsarExtras INTERFACE Pulsar )

# ============================================================================
# Optional compile-time feature flags
#
# Consumers can override these via target_compile_definitions or by passing
# -DPULSAR_ENABLE_THREAD=0 / -DPULSAR_LOG_CONNECTION_ISSUES=1 etc. on the
# command line.  The defaults match the library's out-of-the-box behaviour.
# ============================================================================

# Enable thread safety throughout the library (mutexes on all shared state).
# Disable for single-threaded, bare-metal, or RTOS targets where std::mutex
# is unavailable or undesirable.  When OFF, all platform::Mutex and
# platform::SharedMutex instances are zero-overhead no-op stubs and
# <mutex>/<shared_mutex> are not included.
option( PULSAR_THREAD_SAFE "Enable thread-safe locking throughout Pulsar" ON )
if( NOT PULSAR_THREAD_SAFE )
	target_compile_definitions( Pulsar INTERFACE PULSAR_THREAD_SAFE=0 )
endif()

# Enable std::thread-based auto-processed EventLoop (disable for bare-metal)
option( PULSAR_ENABLE_THREAD "Enable std::thread support in EventLoop" ON )
if( NOT PULSAR_ENABLE_THREAD )
	target_compile_definitions( Pulsar INTERFACE PULSAR_ENABLE_THREAD=0 )
endif()

# Enable connection misconfiguration warnings (off by default)
option( PULSAR_LOG_CONNECTION_ISSUES "Enable connection misconfiguration warnings" OFF )
if( PULSAR_LOG_CONNECTION_ISSUES )
	target_compile_definitions( Pulsar INTERFACE PULSAR_LOG_CONNECTION_ISSUES=1 )
endif()

# ============================================================================
# Optional subdirectories
# ============================================================================

option( PULSAR_BUILD_EXAMPLES "Build Pulsar examples" OFF )
if( PULSAR_BUILD_EXAMPLES )
	add_subdirectory( examples )
endif()

option( PULSAR_BUILD_TESTS "Build Pulsar tests" OFF )
if( PULSAR_BUILD_TESTS )
	add_subdirectory( tests )
endif()

# ============================================================================
# Install rules
# ============================================================================

include( GNUInstallDirs )
include( CMakePackageConfigHelpers )

install(
	TARGETS Pulsar PulsarExtras
	EXPORT  PulsarTargets
)

install(
	DIRECTORY libs/pulsar/include/
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
	DIRECTORY libs/pulsar_extras/include/
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
	EXPORT      PulsarTargets
	FILE        PulsarTargets.cmake
	NAMESPACE   Pulsar::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulsar
)

# Generate PulsarConfigVersion.cmake
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/PulsarConfigVersion.cmake"
	VERSION       ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion
)

# Generate PulsarConfig.cmake from the template at the project root
configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/PulsarConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/PulsarConfig.cmake"
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulsar
)

install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/PulsarConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/PulsarConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulsar
)
