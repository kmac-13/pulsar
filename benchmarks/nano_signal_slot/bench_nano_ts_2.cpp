/**
 * @file bench_nano_ts_2.cpp
 * @brief The genuinely concurrent nano benchmarks.
 *
 * Always thread-safe, no ST variant, since neither makes sense without real
 * concurrency.  Kept in a separate file from bench_nano.cpp (which produces
 * both bench_nano_st and bench_nano_ts_1 from the same source, compiled
 * twice via NANO_THREAD_SAFE) for two reasons:
 * 1. These scenarios have no ST equivalent at all, so they don't belong
 *    in a file whose whole point is code shared between an ST and TS build.
 * 2. Isolating them into their own process/executable is also a pragmatic
 *    mitigation for a hang observed on Windows/MinGW when running the full
 *    nano_ts suite unfiltered in one process - mirroring the fix already
 *    proven necessary for vdk's thread-creating benchmarks (see
 *    KNOWN_ISSUES.md).  A specific root cause for THIS hang hasn't been
 *    confirmed; splitting is a pragmatic mitigation, not a diagnosis.
 *
 * This file: BM_ConcurrentEmission_PerReceiver, BM_ContentionScaling_Direct.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include <nano_signal_slot.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

using namespace bench;

// ============================================================================
// Policy selection - always TS_Policy for this target; requires
// -DNANO_THREAD_SAFE=1.
// ============================================================================

#if defined( NANO_THREAD_SAFE ) && NANO_THREAD_SAFE
	using Policy = Nano::TS_Policy<>;
#else
	using Policy = Nano::ST_Policy;
#endif

using Signal = Nano::Signal< void( EventArg ), Policy >;

// ============================================================================
// Receiver - see bench_nano_ts_1.cpp for the full explanation of the
// manual-disconnect pattern used throughout this file.
// ============================================================================

struct Receiver : public Nano::Observer< Policy >
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 7a - concurrent emission (TS_Policy only)
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	Signal sig;
	std::vector< Receiver > receivers( static_cast< size_t >( state.range( 0 ) ) );

	for ( auto& r : receivers )
	{
		sig.connect< &Receiver::onFired >( r );
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
					sig.fire( EventArg{ PAYLOAD } );
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

	for ( auto& r : receivers )
	{
		sig.disconnect< &Receiver::onFired >( r );
	}
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 9 - contention scaling
//
// BM_ContentionScaling_Direct: CONTENTION_RECEIVER_COUNT receivers are
// fixed; the only thing that varies is the number of threads simultaneously
// calling fire() on the SAME shared signal.  nano's TS_Policy serialises the
//  entire handler-invocation loop under its Spin_Mutex (a hand-rolled atomic
// spin lock, not a real OS mutex) - this is the genuine article for the
// "spin lock vs real mutex under contention" question the Pulsar-side
// SpinLock benchmark was built to investigate.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< Receiver > receivers( CONTENTION_RECEIVER_COUNT );

	for ( auto& r : receivers )
	{
		sig.connect< &Receiver::onFired >( r );
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
					sig.fire( EventArg{ PAYLOAD } );
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

	for ( auto& r : receivers )
	{
		sig.disconnect< &Receiver::onFired >( r );
	}
}
BENCHMARK( BM_ContentionScaling_Direct )
	->Arg( 1 )
	->Arg( 2 )
	->Arg( 4 )
	->Arg( 8 )
	->Arg( 16 )
	->Arg( 32 );

BENCHMARK_MAIN();
