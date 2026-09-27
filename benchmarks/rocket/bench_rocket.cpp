/**
 * @file bench_rocket.cpp
 * @brief rocket (tripleslash) benchmarks - ST/TS-shared scenarios only.
 *
 * Scenarios implemented:
 * 1.  Single-threaded emission, 1 connection
 * 2.  Single-threaded emission, N connections
 * 3.  Connect / disconnect throughput
 * 4.  Scoped receiver lifetime (rocket::trackable disconnects on destruction)
 *
 * The genuinely concurrent scenarios (7a and later) live in a separate
 * file, bench_rocket_ts_2.cpp, always built thread-safe - they don't make
 * sense in an ST build at all, so keeping them here would mean either
 * duplicating this file's ST/TS-shared code or leaving dead code compiled
 * into the ST build.  See that file's own header comment for the full
 * concurrent-scenario details (including why BM_ConcurrentEmission_PerReceiver
 * and its BM_ConcurrentEmission_Serialized counterpart are both measured).
 *
 * Two executables are produced by CMake from THIS file:
 *   bench_rocket_st   - rocket::signal<> (thread_unsafe_policy, the default)
 *   bench_rocket_ts_1 - rocket::thread_safe_signal<> (thread_safe_policy),
 *                       same source file, same scenarios, built with
 *                       ROCKET_THREAD_SAFE to measure the policy's overhead
 *                       on operations that don't need real concurrency to
 *                       be meaningful
 *
 * A third executable, bench_rocket_ts_2, is produced from
 * bench_rocket_ts_2.cpp (always thread-safe, no ST variant).
 *
 * ROCKET_THREAD_SAFE selects which alias `Signal` resolves to.
 *
 * rocket uses connection handles returned from connect(); disconnect() takes
 * the handle.  Receivers inheriting rocket::trackable auto-disconnect on
 * destruction: trackable owns a scoped_connection_container member, and that
 * container's own destructor tears down a std::forward_list of
 * scoped_connection entries, each of whose destructor calls the same
 * connection::disconnect() a manual call would - there is no ST/TS branch in
 * that path, so scenario 4 lets the destructor do the work rather than
 * disconnecting explicitly first.
 */

#include "../common/benchmark_helpers.h"

#include <rocket.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

using namespace bench;

#if defined( ROCKET_THREAD_SAFE ) && ROCKET_THREAD_SAFE
using Signal = rocket::thread_safe_signal< void( EventArg ) >;
#else
using Signal = rocket::signal< void( EventArg ) >;
#endif

// ============================================================================
// Receiver
// ============================================================================

struct Receiver : public rocket::trackable
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

/// Receiver with its OWN per-instance atomic state, used only by
/// BM_ConcurrentEmission_PerReceiver/_Serialized below - isolates real
/// concurrent dispatch cost from the cache-line contention that a single
/// shared global_sink written by every receiver on every thread would otherwise
/// introduce.  Every other benchmark in this file continues to use the
/// ordinary Receiver (shared global_sink) for continuity with prior numbers.
struct LocalSinkReceiver : public rocket::trackable
{
	std::atomic< EventArg > localSink{ 0 };
	void onFired( EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	Signal sig;
	Receiver r;
	sig.connect( &r, &Receiver::onFired );

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
	sig.connect( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } );

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

	for ( auto& r : receivers )
	{
		sig.connect( &r, &Receiver::onFired );
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
		std::vector< rocket::connection > handles;
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( auto& r : receivers )
		{
			handles.push_back( sig.connect( &r, &Receiver::onFired ) );
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
// rocket::trackable severs connections in its destructor via the same
// connection::disconnect() a manual call would use, so no explicit
// disconnect needed before the Receiver goes out of scope; destruction
// alone severs the connection.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Signal sig;

	for ( auto _ : state )
	{
		{
			Receiver r;
			sig.connect( &r, &Receiver::onFired );
			sig( PAYLOAD );
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

BENCHMARK_MAIN();
