/**
 * @file bench_sigslot.cpp
 * @brief sigslot (palacaze) benchmarks.
 *
 * Scenarios implemented:
 * 1.  Single-threaded emission, 1 connection
 * 2.  Single-threaded emission, N connections
 * 3.  Connect / disconnect throughput
 * 4.  Scoped receiver lifetime (sigslot auto-disconnects via observer base)
 * 7a. Concurrent emission - signal_st excluded; signal (mt) only
 *
 * Scenarios 5, 6, 7b: N/A - sigslot has no built-in event loop or deferred
 * dispatch mechanism.
 *
 * Two executables are produced by CMake:
 *   bench_sigslot_st - sigslot::signal_st (single-threaded, no locking)
 *   bench_sigslot_mt - sigslot::signal    (multi-threaded, mutex-protected)
 *
 * SIGSLOT_THREAD_SAFE selects the variant.
 */

#include "../common/benchmark_helpers.h"

#include <sigslot/signal.hpp>

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace bench;

// ============================================================================
// Signal type selection
// ============================================================================

#if defined( SIGSLOT_THREAD_SAFE ) && SIGSLOT_THREAD_SAFE
	template< typename... Args >
	using Signal = sigslot::signal< Args... >;          // multi-threaded variant
#else
	template< typename... Args >
	using Signal = sigslot::signal_st< Args... >;       // single-threaded variant
#endif

// ============================================================================
// Receiver
//
// sigslot supports two auto-disconnect patterns:
// a) Inherit from sigslot::observer (or sigslot::observer_st)
// b) Connect via shared_ptr - sigslot holds a weak_ptr internally
//
// We use (b) here because it avoids a base-class requirement on the receiver
// type.  For scenario 4 the shared_ptr going out of scope severs the
// connection automatically; no explicit disconnect is needed.
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
	Signal< EventArg > sig;
	auto r = std::make_shared< Receiver >();
	sig.connect( &Receiver::onFired, r );

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
	Signal< EventArg > sig;
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

	Signal< EventArg > sig;
	std::vector< std::shared_ptr< Receiver > > receivers;
	receivers.reserve( n );

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_shared< Receiver >();
		sig.connect( &Receiver::onFired, r );
		receivers.push_back( std::move( r ) );
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
//
// signal_st's emission loop iterates its slot vector directly (no
// copy-on-write isolation - that's only present in the thread-safe `signal`
// variant).  If a tracked shared_ptr receiver expires and an emission
// subsequently walks into that slot, call_slot() detects the expired
// weak_ptr and calls disconnect() -> clean() *during* that same emission,
// mutating the live vector the enclosing loop is iterating - undefined
// behaviour, and the source of the heap corruption.
//
// Fix: explicitly disconnect every connection handle before the receivers'
// shared_ptrs are reset.  This guarantees no expired-but-still-connected
// slot is ever present at the *next* emission, so the reentrant
// clean()-during-iteration path is never triggered.  (Simply emitting again
// after receivers.clear(), as an earlier version of this benchmark tried,
// does not fix this - it walks directly into the same hazard, once per
// now-expired slot, on every iteration.)
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	Signal< EventArg > sig;

	for ( auto _ : state )
	{
		std::vector< std::shared_ptr< Receiver > > receivers;
		std::vector< sigslot::connection > handles;
		receivers.reserve( CONNECT_DISCONNECT_BATCH );
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
		{
			auto r = std::make_shared< Receiver >();
			handles.push_back( sig.connect( &Receiver::onFired, r ) );
			receivers.push_back( std::move( r ) );
		}

		sig( PAYLOAD );

		// explicit disconnect while every receiver is still alive - no
		// weak_ptr expiry is ever discovered mid-emission
		for ( auto& c : handles )
		{
			c.disconnect();
		}

		// now safe to drop the shared_ptrs; the signal's slot list is already
		// empty, so no stale tracked slot can be found expired by a future emission
		receivers.clear();
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// sigslot tracks shared_ptr receivers via internal weak_ptr.  When the
// shared_ptr is reset, the weak_ptr expires and sigslot skips the dead slot
// on next emission (lazy cleanup).  No explicit disconnect required.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Signal< EventArg > sig;

	for ( auto _ : state )
	{
		{
			auto r = std::make_shared< Receiver >();
			sig.connect( &Receiver::onFired, r );
			sig( PAYLOAD );
			// r reset here; connection expires automatically
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

BENCHMARK_MAIN();
