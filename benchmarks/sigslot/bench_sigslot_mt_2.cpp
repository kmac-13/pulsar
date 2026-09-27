/**
 * @file bench_sigslot_mt_2.cpp
 * @brief The genuinely concurrent sigslot benchmarks.
 *
 * Always thread-safe (MT), no ST variant, since neither makes sense without
 * real concurrency.  Kept in a separate file from bench_sigslot.cpp (which
 * produces both bench_sigslot_st and bench_sigslot_mt_1 from the same
 * source, compiled twice via SIGSLOT_THREAD_SAFE) for two reasons:
 * 1. These scenarios have no ST equivalent at all, so they don't belong in
 *    a file whose whole point is code shared between an ST and TS build.
 * 2. Isolating them into their own process/executable is also a pragmatic
 *    mitigation for a hang observed on Windows/MinGW when running the full
 *    sigslot_mt suite unfiltered in one process - mirroring the fix already
 *    proven necessary for vdk's thread-creating benchmarks (see
 *    KNOWN_ISSUES.md).  A specific root cause for THIS hang hasn't been
 *    confirmed; splitting is a pragmatic mitigation, not a diagnosis.
 *
 * This file: BM_ConcurrentEmission_PerReceiver, BM_ContentionScaling_Direct.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include <sigslot/signal.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <memory>
#include <vector>

using namespace bench;

// ============================================================================
// Signal type selection - always the MT variant for this target; requires
// -DSIGSLOT_THREAD_SAFE=1.
// ============================================================================

#if defined( SIGSLOT_THREAD_SAFE ) && SIGSLOT_THREAD_SAFE
	template< typename... Args >
	using Signal = sigslot::signal< Args... >;          // multi-threaded variant
#else
	template< typename... Args >
	using Signal = sigslot::signal_st< Args... >;       // single-threaded variant
#endif

// ============================================================================
// Receiver - see bench_sigslot.cpp (compiled with SIGSLOT_THREAD_SAFE, output as bench_sigslot_mt_1) for the full explanation of the
// shared_ptr ownership pattern used throughout this file.
// ============================================================================

struct Receiver
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 7a - concurrent emission (MT variant only)
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
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

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sig, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					sig( PAYLOAD );
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		startBarrier.arrive();
		endBarrier.arrive();
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() )
		* CROSS_THREAD_EMISSIONS
		* CONCURRENT_THREAD_COUNT
	);
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 9 - contention scaling
//
// BM_ContentionScaling_Direct: CONTENTION_RECEIVER_COUNT receivers are
// fixed; the only thing that varies is the number of threads simultaneously
// calling operator() on the SAME shared signal.  sigslot's MT signal
// (signal_base<std::mutex, ...>) serialises the entire handler-invocation
// loop under a real std::mutex.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	Signal< EventArg > sig;
	std::vector< std::shared_ptr< Receiver > > receivers;
	receivers.reserve( CONTENTION_RECEIVER_COUNT );

	for ( int i = 0; i < CONTENTION_RECEIVER_COUNT; ++i )
	{
		auto r = std::make_shared< Receiver >();
		sig.connect( &Receiver::onFired, r );
		receivers.push_back( std::move( r ) );
	}

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( threadCount + 1 );
	SpinBarrier endBarrier( threadCount + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( threadCount );

	for ( int t = 0; t < threadCount; ++t )
	{
		emitters.emplace_back( [ &sig, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					sig( PAYLOAD );
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		startBarrier.arrive();
		endBarrier.arrive();
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() )
		* CROSS_THREAD_EMISSIONS
		* threadCount
	);
}
BENCHMARK( BM_ContentionScaling_Direct )
	->Arg( 1 )
	->Arg( 2 )
	->Arg( 4 )
	->Arg( 8 )
	->Arg( 16 )
	->Arg( 32 );

BENCHMARK_MAIN();
