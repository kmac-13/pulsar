/**
 * @file bench_nod_1.cpp
 * @brief Part 1 of the nod benchmark suite.
 *
 * Split into two executables (see bench_nod_2.cpp for part 2) to isolate a
 * hang observed on Windows/MinGW when running the full bench_nod suite
 * unfiltered in one process - mirroring the fix already proven necessary
 * for vdk's thread-creating benchmarks (see KNOWN_ISSUES.md).  Unlike vdk's
 * issue, a specific root cause for THIS hang hasn't been confirmed;
 * splitting is a pragmatic mitigation, not a diagnosis.  nod has no separate
 * ST/TS build (nod::signal is always thread-safe), so this split has no
 * "_st"/"_ts" naming.
 *
 * This file: the benchmarks that don't create any threads.  See
 * bench_nod_2.cpp for the thread-creating benchmarks
 * (ConcurrentEmission_PerReceiver, ContentionScaling_Direct,
 * ConcurrentEmission_PerReceiver_Serialized).
 */

#include "../common/benchmark_helpers.h"

#include <nod/nod.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <functional>
#include <vector>

using namespace bench;

using Signal = nod::signal< void( EventArg ) >;

// ============================================================================
// Receiver - plain struct, no base class needed
// ============================================================================

struct Receiver
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	Signal sig;
	Receiver r;
	auto c = sig.connect( std::bind( &Receiver::onFired, &r, std::placeholders::_1 ) );

	for ( auto _ : state )
	{
		sig( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection );

// ============================================================================
// Scenario 1 (lambda) - 1 directly-connected lambda, no receiver object
// ============================================================================

static void BM_Emit_1Connection_Lambda( benchmark::State& state )
{
	Signal sig;
	auto c = sig.connect( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } );

	for ( auto _ : state )
	{
		sig( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Lambda );

// ============================================================================
// Scenario 2 - single-threaded emission, N connections
// ============================================================================

static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< Receiver > receivers( n );
	std::vector< nod::connection > handles;
	handles.reserve( n );

	for ( auto& r : receivers )
	{
		handles.push_back( sig.connect( std::bind( &Receiver::onFired, &r, std::placeholders::_1 ) ) );
	}

	for ( auto _ : state )
	{
		sig( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 3 - connect / disconnect throughput
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	Signal sig;

	for ( auto _ : state )
	{
		std::vector< Receiver > receivers( CONNECT_DISCONNECT_BATCH );
		std::vector< nod::connection > handles;
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( auto& r : receivers )
		{
			handles.push_back( sig.connect( std::bind( &Receiver::onFired, &r, std::placeholders::_1 ) ) );
		}

		sig( PAYLOAD );

		for ( auto& c : handles )
		{
			c.disconnect();
		}
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// nod provides no automatic lifetime tracking.  The idiomatic pattern is to
// tie a nod::scoped_connection to the receiver's scope so disconnection and
// destruction are coupled.  This is the "safest correct usage" for nod.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Signal sig;

	for ( auto _ : state )
	{
		{
			Receiver r;
			// scoped_connection disconnects when it goes out of scope,
			// coinciding with the receiver's destruction
			nod::scoped_connection sc = sig.connect( std::bind( &Receiver::onFired, &r, std::placeholders::_1 ) );
			sig( PAYLOAD );
		}
		// sc and r both destroyed here; disconnect is automatic via sc
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

BENCHMARK_MAIN();
