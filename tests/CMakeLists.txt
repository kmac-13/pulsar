cmake_minimum_required( VERSION 3.16 )
project( StellyraTests )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

if( WIN32 )
	add_definitions( -DNOMINMAX -DWIN32_LEAN_AND_MEAN )
endif()

# Enables EventLoop's STELLYRA_TEST_INJECT hook points (currently just
# migratePendingTo()'s), used to deterministically reproduce concurrent
# lock-contention scenarios instead of hoping the scheduler interleaves
# things a particular way.  Compiles to nothing (no member, no call site)
# in any build where this isn't defined, so this is safe to enable for the
# whole test suite - only tests that explicitly install a hook are affected.
add_definitions( -DSTELLYRA_TEST_INJECT )

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

# Collect Stellyra files
file( GLOB STELLYRA_SOURCES
	${CMAKE_SOURCE_DIR}/../libs/stellyra/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/stellyra/include/kmac/stellyra/*.h
	${CMAKE_SOURCE_DIR}/../libs/stellyra_extras/include/kmac/*.h
	${CMAKE_SOURCE_DIR}/../libs/stellyra_extras/include/kmac/stellyra/*.h
)

# Collect Stellyra documents
file( GLOB STELLYRA_DOCS
	${CMAKE_SOURCE_DIR}/../*.md
	${CMAKE_SOURCE_DIR}/../docs/*.md
)


# ---------------------------------------------------------------------------
# Helper function - creates a GTest executable and registers it with CTest
# ---------------------------------------------------------------------------
function( add_stellyra_test test_name source_file )
	add_executable( ${test_name} ${source_file} ${STELLYRA_SOURCES} ${STELLYRA_DOCS} )

	target_include_directories( ${test_name} PRIVATE
		${CMAKE_SOURCE_DIR}/../libs/stellyra/include
		${CMAKE_SOURCE_DIR}/../libs/stellyra_extras/include
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

# building blocks
add_stellyra_test( test_callable               test_callable.cpp )

# event handling
add_stellyra_test( test_basic_connections      test_basic_connections.cpp )
add_stellyra_test( test_free_functions         test_free_functions.cpp )
add_stellyra_test( test_advanced_connections   test_advanced_connections.cpp )
add_stellyra_test( test_partial_args           test_partial_args.cpp )
add_stellyra_test( test_invokers               test_invokers.cpp )
add_stellyra_test( test_sender_deferral        test_sender_deferral.cpp )
add_stellyra_test( test_thread_safety          test_thread_safety.cpp )
add_stellyra_test( test_reentrancy             test_reentrancy.cpp )
add_stellyra_test( test_predicate_context      test_predicate_context.cpp )
add_stellyra_test( test_connection_lifecycle   test_connection_lifecycle.cpp )
add_stellyra_test( test_disconnect_by_target   test_disconnect_by_target.cpp )
add_stellyra_test( test_stable_slots           test_stable_slots.cpp )
add_stellyra_test( test_receiver_lifetime_anchor test_receiver_lifetime_anchor.cpp )
add_stellyra_test( test_pending_removal_stress test_pending_removal_stress.cpp )
add_stellyra_test( test_trackconnection_race   test_trackconnection_race.cpp )
add_stellyra_test( test_forward_to             test_forward_to.cpp )

# event loop
add_stellyra_test( test_event_loops            test_event_loops.cpp )
add_stellyra_test( test_external_event_loop    test_external_event_loop.cpp )
add_stellyra_test( test_event_loop_migration   test_event_loop_migration.cpp )
add_stellyra_test( test_event_loop_direct_api  test_event_loop_direct_api.cpp )

# extras
add_stellyra_test( test_combining_and_groups   test_combining_and_groups.cpp )
add_stellyra_test( test_combiners              test_combiners.cpp )
add_stellyra_test( test_recordable_event       test_recordable_event.cpp )
add_stellyra_test( test_concurrent_event       test_concurrent_event.cpp )
add_stellyra_test( test_property               test_property.cpp )

# misc
add_stellyra_test( test_block_guard            test_block_guard.cpp )
add_stellyra_test( test_deferred_teardown_order test_deferred_teardown_order.cpp )
add_stellyra_test( test_coverage_gaps          test_coverage_gaps.cpp )
add_stellyra_test( test_edge_cases             test_edge_cases.cpp )
add_stellyra_test( test_sense_plan_act         test_sense_plan_act.cpp )
