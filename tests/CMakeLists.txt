cmake_minimum_required( VERSION 3.16 )
project( PulsarTests )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

if( WIN32 )
	add_definitions( -DNOMINMAX -DWIN32_LEAN_AND_MEAN )
endif()

# ---------------------------------------------------------------------------
# GoogleTest
# Fetched from source so there is no external dependency to install.
# ---------------------------------------------------------------------------
# include(FetchContent)

# FetchContent_Declare(
#     googletest
#     GIT_REPOSITORY https://github.com/google/googletest.git
#     GIT_TAG        v1.14.0
# )
add_subdirectory( "../../../googletest/" ${CMAKE_CURRENT_BINARY_DIR}/googletest )

# prevent GTest from overriding our compiler/linker settings on Windows
set( gtest_force_shared_crt ON CACHE BOOL "" FORCE )
# FetchContent_MakeAvailable(googletest)

include( GoogleTest )
enable_testing()

# Collect Pulsar files
file( GLOB PULSAR_SOURCES
	${CMAKE_SOURCE_DIR}/../libs/pulsar/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar/include/kmac/pulsar/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar_extras/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/pulsar_extras/include/kmac/pulsar/*.h
)

# Collect Pulsar documents
file( GLOB PULSAR_DOCS
	${CMAKE_SOURCE_DIR}/../*.md
	${CMAKE_SOURCE_DIR}/../docs/*.md
)


# ---------------------------------------------------------------------------
# Helper function - creates a GTest executable and registers it with CTest
# ---------------------------------------------------------------------------
function( add_pulsar_test test_name source_file )
	add_executable( ${test_name} ${source_file} ${PULSAR_SOURCES} ${PULSAR_DOCS} )

	target_include_directories( ${test_name} PRIVATE
		${CMAKE_SOURCE_DIR}/../libs/pulsar/include
		${CMAKE_SOURCE_DIR}/../libs/pulsar_extras/include
		${CMAKE_SOURCE_DIR}/..
	)

	target_link_libraries( ${test_name} PRIVATE GTest::gtest GTest::gtest_main )

	if( UNIX )
		target_link_libraries( ${test_name} PRIVATE pthread )
	endif()

	target_compile_features( ${test_name} PRIVATE cxx_std_17 )

	if( CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" )
		target_compile_options( ${test_name} PRIVATE -Wall -Wextra -Wpedantic )
	elseif( MSVC )
		target_compile_options( ${test_name} PRIVATE /W4 /wd4127 )
	endif()

	gtest_discover_tests( ${test_name} )
endfunction()

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------
add_pulsar_test( test_basic_connections      test_basic_connections.cpp )
add_pulsar_test( test_advanced_connections   test_advanced_connections.cpp )
add_pulsar_test( test_combining_and_groups   test_combining_and_groups.cpp )
add_pulsar_test( test_free_functions         test_free_functions.cpp )
add_pulsar_test( test_event_loops            test_event_loops.cpp )
add_pulsar_test( test_external_event_loop    test_external_event_loop.cpp )
add_pulsar_test( test_thread_safety          test_thread_safety.cpp )
add_pulsar_test( test_edge_cases             test_edge_cases.cpp )
add_pulsar_test( test_sender_deferral        test_sender_deferral.cpp )
add_pulsar_test( test_recordable_event       test_recordable_event.cpp )
add_pulsar_test( test_property               test_property.cpp )
add_pulsar_test( test_coverage_gaps          test_coverage_gaps.cpp )
