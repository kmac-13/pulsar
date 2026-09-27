/**
 * @file bench_nano.cpp
 * @brief nano-signal-slot benchmarks.
 *
 * Scenarios implemented:
 * 1.  Single-threaded emission, 1 connection
 * 2.  Single-threaded emission, N connections
 * 3.  Connect / disconnect throughput
 * 4.  Scoped receiver lifetime (Observer destructor auto-disconnects)
 * 7a. Concurrent emission - TS_Policy variant only
 *
 * Scenarios 5, 6, 7b: N/A - nano-signal-slot has no built-in event loop or
 * deferred dispatch mechanism.
 *
 * Scenario 2c (lambda variant): N/A - nano's connect(L& instance) takes a
 * reference to an externally-owned functor object and stores a pointer to
 * it, rather than moving/copying the callable into internally-owned storage
 * the way Pulsar/Qt/libsigc++/rocket/nod/sigslot all do for their own
 * direct-lambda connect.  A temporary lambda passed this way would dangle
 * immediately - there's no equivalent operation here to benchmark, not just
 * an unwritten one.
 *
 * Two executables are produced by CMake:
 *   bench_nano_st - Nano::ST_Policy (no locking)
 *   bench_nano_ts - Nano::TS_Policy (mutex-protected emission)
 *
 * The NANO_THREAD_SAFE preprocessor flag selects the policy.
 */

#include "../common/benchmark_helpers.h"

#include <nano_signal_slot.hpp>

#include <benchmark/benchmark.h>

#include <vector>

using namespace bench;

// ============================================================================
// Policy selection
// ============================================================================

#if defined( NANO_THREAD_SAFE ) && NANO_THREAD_SAFE
	using Policy = Nano::TS_Policy<>;
#else
	using Policy = Nano::ST_Policy;
#endif

using Signal = Nano::Signal< void( EventArg ), Policy >;

// ============================================================================
// Receiver
//
// nano-signal-slot uses inheritance from Nano::Observer<Policy> for automatic
// disconnect on destruction.  ~Observer() unconditionally calls
// disconnect_all(), which walks the object's own connection list and removes
// the matching entry from each connected Signal's list too (Signal itself
// derives from Observer), so destroying a Receiver fully severs its
// connections with no manual disconnect needed.
// ============================================================================

struct Receiver : public Nano::Observer< Policy >
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
	sig.connect< &Receiver::onFired >( r );

	for ( auto _ : state )
	{
		sig.fire( EventArg{ PAYLOAD } );
	}

	sig.disconnect< &Receiver::onFired >( r );
}
BENCHMARK( BM_Emit_1Connection );

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
		sig.connect< &Receiver::onFired >( r );
	}

	for ( auto _ : state )
	{
		sig.fire( EventArg{ PAYLOAD } );
	}

	for ( auto& r : receivers )
	{
		sig.disconnect< &Receiver::onFired >( r );
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

		for ( auto& r : receivers )
		{
			sig.connect< &Receiver::onFired >( r );
		}

		sig.fire( EventArg{ PAYLOAD } );

		for ( auto& r : receivers )
		{
			sig.disconnect< &Receiver::onFired >( r );
		}
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// ~Observer() calls disconnect_all() unconditionally, so nothing here needs
// an explicit disconnect before the Receiver goes out of scope; destruction
// alone severs the connection, matching the other libraries' scenario 4.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Signal sig;

	for ( auto _ : state )
	{
		{
			Receiver r;
			sig.connect< &Receiver::onFired >( r );
			sig.fire( EventArg{ PAYLOAD } );
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

BENCHMARK_MAIN();
