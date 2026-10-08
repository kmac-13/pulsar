/**
 * @file bench_stellyra_mutex_variants.cpp
 * @brief Internal Stellyra comparison: same architecture, same dispatch
 * machinery, only MutexType varies.  Isolates the cost of the mutex itself
 * from any other library's design differences - answers "how much does
 * RecursiveMutex cost over plain Mutex within Stellyra", separately from any
 * cross-library comparison.
 *
 * Extracted from bench_stellyra_v2.cpp: these benchmarks compare Stellyra
 * against itself across mutex backends, not against Qt, so they don't
 * belong alongside the cross-library scenarios - there is no Qt equivalent
 * to pair them with (Qt does not expose a pluggable mutex backend), and
 * mixing them into bench_stellyra_v2.cpp's output made it easy to mistake an
 * internal Mutex-vs-RecursiveMutex comparison for a Stellyra-vs-Qt one when
 * skimming benchmark names.
 *
 * platform::Mutex has no named public alias (Event/SharedEvent/
 * SingleThreadedEvent cover RecursiveMutex/SharedMutex/NullMutex only), so
 * BasicEvent<platform::Mutex,...> is used directly here.
 *
 * Not gated by STELLYRA_THREAD_SAFE - none of these require AutoDrainThread.
 */

#include "../common/benchmark_helpers.h"

#include <kmac/stellyra/event.h>

#include <benchmark/benchmark.h>

#include <vector>

using namespace bench;
namespace stellyra = kmac::stellyra;

// ============================================================================
// Fixtures
// ============================================================================

/// Minimal receiver: writes received value to global_sink.
struct Receiver : stellyra::Trackable
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

template< typename MutexType >
struct MutexTypedSender : stellyra::Trackable
{
	stellyra::BasicEvent< MutexType, EventArg > fired{ this };
};

// ============================================================================
// Emission cost per mutex backend, 1 connection
// ============================================================================

template< typename MutexType >
static void BM_Emit_1Connection( benchmark::State& state )
{
	MutexTypedSender< MutexType > sender;
	Receiver r;
	sender.fired.template connect< &Receiver::onFired >( r );

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK_TEMPLATE( BM_Emit_1Connection, stellyra::platform::Mutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, stellyra::platform::RecursiveMutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, stellyra::platform::SharedMutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, stellyra::platform::NullMutex );

// ============================================================================
// Emission cost per mutex backend, N connections
// ============================================================================

template< typename MutexType >
static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	MutexTypedSender< MutexType > sender;
	std::vector< Receiver > receivers( n );
	for ( auto& r : receivers )
	{
		sender.fired.template connect< &Receiver::onFired >( r );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK_TEMPLATE( BM_Emit_NConnections, stellyra::platform::Mutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, stellyra::platform::RecursiveMutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, stellyra::platform::SharedMutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, stellyra::platform::NullMutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );

// ============================================================================
// Connect/disconnect churn per mutex type - the lock is taken on every
// connect() and disconnect() call, not just trigger(), so this isolates a
// different part of the cost than the emit benchmarks above.
// ============================================================================

template< typename MutexType >
static void BM_ConnectDisconnect( benchmark::State& state )
{
	MutexTypedSender< MutexType > sender;

	for ( auto _ : state )
	{
		Receiver r;
		stellyra::Connection c = sender.fired.template connect< &Receiver::onFired >( r );
		c.disconnect();
	}
}
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, stellyra::platform::Mutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, stellyra::platform::RecursiveMutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, stellyra::platform::SharedMutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, stellyra::platform::NullMutex );

BENCHMARK_MAIN();
