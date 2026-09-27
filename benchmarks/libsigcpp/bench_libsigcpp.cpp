/**
 * @file bench_libsigcpp.cpp
 * @brief libsigc++ benchmarks.
 *
 * Scenarios implemented (see SCENARIOS.md for what each one tests):
 * 1, 2 - single-threaded emission, 1 and N connections
 * 2c   - connection-syntax comparison: member-function (sigc::mem_fun,
 *        the baseline) vs. free-function (sigc::ptr_fun) vs. a directly
 *        owned lambda.  libsigc++ has no NTTP-style connection syntax,
 *        so this is member-vs-free-vs-lambda only, not compile-time-vs-
 *        runtime the way Pulsar's 2c/2d also compares.
 * 3    - connect/disconnect throughput, via sigc::connection handles
 * 4    - scoped receiver lifetime, via sigc::trackable
 * 8b (approximate) - bulk disconnect via sigc::trackable::notify_callbacks(),
 *        which disconnects a receiver from every signal it's connected to
 *        (no way to scope to one signal, the way Pulsar's
 *        extractConnectionsTo(target) does).  This benchmark connects one
 *        receiver to N different signals and disconnects from all of them
 *        at once, to exercise the same "cost scales with N tracked
 *        connections" question at a different granularity than 8b's exact
 *        shape elsewhere.
 *
 * Not implemented:
 * - Scenarios 2a/2b (Auto connection type): no thread-affinity concept
 *   exists to resolve between, so "Auto" has no meaning here.
 * - Scenarios 5, 6, 7a, 7b (any cross-thread or concurrent dispatch):
 *   libsigc++ has no thread-safety option of any kind, not even opt-in.
 *   Concurrent emission is undefined behaviour here, not just unbenchmarked.
 * - Scenario 8a (disconnect-by-target linear scan): there is no signal-side
 *   API that searches its own connection list by receiver identity.  Every
 *   disconnect() requires already holding the specific sigc::connection
 *   handle from connect().  The trackable mechanism above disconnects from
 *   the receiver's own side instead, which covers 8b's shape, not 8a's.
 *
 * Also present in the library, not benchmarked here because no other library
 * file in this project benchmarks the equivalent capability either:
 * connection blocking (signal::block()/unblock()/blocked()) and return-value
 * accumulation across multiple handlers (sigc::signal_with_accumulator -
 * libsigc++'s own term for what rocket calls a Collector).
 *
 * Build from a local source checkout - see libsigcpp/CMakeLists.txt.
 * CMake target: bench_libsigcpp (built only when LIBSIGCPP_SOURCE_DIR is set).
 */

#include "../common/benchmark_helpers.h"

#include <sigc++/sigc++.h>

#include <benchmark/benchmark.h>

#include <vector>

using namespace bench;

// ============================================================================
// Receiver
//
// sigc::trackable provides automatic disconnect on destruction in
// single-threaded use.  Not safe against concurrent emission + destruction.
// ============================================================================

struct Receiver : public sigc::trackable
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	sigc::signal< void( EventArg ) > sig;
	Receiver r;
	sig.connect( sigc::mem_fun( r, &Receiver::onFired ) );

	for ( auto _ : state )
	{
		sig.emit( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection );

// ============================================================================
// Scenario 2 - single-threaded emission, N connections
// ============================================================================

static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	sigc::signal< void( EventArg ) > sig;
	std::vector< Receiver > receivers( n );

	for ( auto& r : receivers )
	{
		sig.connect( sigc::mem_fun( r, &Receiver::onFired ) );
	}

	for ( auto _ : state )
	{
		sig.emit( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 2c - single-threaded emission, 1 connection, syntax comparison
//
// BM_Emit_1Connection above is the mem_fun (member-function) baseline.
// These compare a free function (ptr_fun) and a directly-connected lambda
// against that same baseline.
// ============================================================================

static void onFiredFree( EventArg v )
{
	global_sink.store( v, std::memory_order_relaxed );
}

static void BM_Emit_1Connection_Free( benchmark::State& state )
{
	sigc::signal< void( EventArg ) > sig;
	sig.connect( sigc::ptr_fun( &onFiredFree ) );

	for ( auto _ : state )
	{
		sig.emit( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Free );

static void BM_Emit_1Connection_Lambda( benchmark::State& state )
{
	sigc::signal< void( EventArg ) > sig;
	sig.connect( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } );

	for ( auto _ : state )
	{
		sig.emit( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Lambda );

// ============================================================================
// Scenario 3 - connect / disconnect throughput
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	sigc::signal< void( EventArg ) > sig;

	for ( auto _ : state )
	{
		std::vector< Receiver > receivers( CONNECT_DISCONNECT_BATCH );
		std::vector< sigc::connection > handles;
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( auto& r : receivers )
		{
			handles.push_back( sig.connect( sigc::mem_fun( r, &Receiver::onFired ) ) );
		}

		sig.emit( PAYLOAD );

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
// sigc::trackable disconnects in its destructor (single-threaded only).
// We use explicit disconnect before destruction to model safe usage.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	sigc::signal< void( EventArg ) > sig;

	for ( auto _ : state )
	{
		{
			Receiver r;
			auto c = sig.connect( sigc::mem_fun( r, &Receiver::onFired ) );
			sig.emit( PAYLOAD );
		}
		// receiver's destructor (sigc::trackable) disconnects it here
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

// ============================================================================
// Scenario 8b (approximate) - bulk disconnect via
// sigc::trackable::notify_callbacks()
//
// See the file-level comment above for the granularity caveat versus
// Pulsar's extractConnectionsTo(target): this disconnects the receiver
// from EVERY signal it's connected to, not just one.  One receiver is
// connected to n separate signals; the timed operation is disconnecting
// from all n at once via a single notify_callbacks() call.
// ============================================================================

static void BM_DisconnectTracker_Batch( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	for ( auto _ : state )
	{
		state.PauseTiming();
		Receiver r;
		std::vector< sigc::signal< void( EventArg ) > > signals( n );
		for ( auto& sig : signals )
		{
			sig.connect( sigc::mem_fun( r, &Receiver::onFired ) );
		}
		state.ResumeTiming();

		r.notify_callbacks();
	}
}
BENCHMARK( BM_DisconnectTracker_Batch )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

BENCHMARK_MAIN();
