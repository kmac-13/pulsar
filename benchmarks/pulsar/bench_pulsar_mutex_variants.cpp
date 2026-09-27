/**
 * @file bench_pulsar_mutex_variants.cpp
 * @brief Internal Pulsar comparison: same architecture, same dispatch
 * machinery, only MutexType varies.  Isolates the cost of the mutex itself
 * from any other library's design differences - answers "how much does
 * RecursiveMutex cost over plain Mutex within Pulsar", separately from any
 * cross-library comparison.
 *
 * Extracted from bench_pulsar_v2.cpp: these benchmarks compare Pulsar
 * against itself across mutex backends, not against Qt, so they don't
 * belong alongside the cross-library scenarios - there is no Qt equivalent
 * to pair them with (Qt does not expose a pluggable mutex backend), and
 * mixing them into bench_pulsar_v2.cpp's output made it easy to mistake an
 * internal Mutex-vs-RecursiveMutex comparison for a Pulsar-vs-Qt one when
 * skimming benchmark names.
 *
 * platform::Mutex has no named public alias (Event/SharedEvent/
 * SingleThreadedEvent cover RecursiveMutex/SharedMutex/NullMutex only), so
 * BasicEvent<platform::Mutex,...> is used directly here.
 *
 * Not gated by PULSAR_THREAD_SAFE - none of these require AutoDrainThread.
 */

#include "../common/benchmark_helpers.h"

#include <kmac/pulsar/event.h>

#include <benchmark/benchmark.h>

#include <vector>

using namespace bench;
namespace pulsar = kmac::pulsar;

// ============================================================================
// Fixtures
// ============================================================================

/// Minimal receiver: writes received value to global_sink.
struct Receiver : pulsar::Trackable
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

template< typename MutexType >
struct MutexTypedSender : pulsar::Trackable
{
	pulsar::BasicEvent< MutexType, EventArg > fired{ this };
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
BENCHMARK_TEMPLATE( BM_Emit_1Connection, pulsar::platform::Mutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, pulsar::platform::RecursiveMutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, pulsar::platform::SharedMutex );
BENCHMARK_TEMPLATE( BM_Emit_1Connection, pulsar::platform::NullMutex );

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
BENCHMARK_TEMPLATE( BM_Emit_NConnections, pulsar::platform::Mutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, pulsar::platform::RecursiveMutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, pulsar::platform::SharedMutex )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );
BENCHMARK_TEMPLATE( BM_Emit_NConnections, pulsar::platform::NullMutex )
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
		pulsar::Connection c = sender.fired.template connect< &Receiver::onFired >( r );
		c.disconnect();
	}
}
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, pulsar::platform::Mutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, pulsar::platform::RecursiveMutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, pulsar::platform::SharedMutex );
BENCHMARK_TEMPLATE( BM_ConnectDisconnect, pulsar::platform::NullMutex );

BENCHMARK_MAIN();
