/**
 * @file bench_nod_2.cpp
 * @brief Part 2 of the nod benchmark suite.
 *
 * Split from bench_nod_1.cpp to isolate a hang observed on Windows/MinGW
 * when running the full bench_nod suite unfiltered in one process -
 * mirroring the fix already proven necessary for vdk's thread-creating
 * benchmarks (see KNOWN_ISSUES.md).  Unlike vdk's issue, a specific root
 * cause for THIS hang hasn't been confirmed; splitting is a pragmatic
 * mitigation, not a diagnosis.
 *
 * This file: the thread-creating benchmarks (ConcurrentEmission_PerReceiver,
 * ContentionScaling_Direct, ConcurrentEmission_PerReceiver_Serialized).  See
 * bench_nod_1.cpp for the benchmarks that don't create any threads.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include <nod/nod.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <functional>
#include <vector>

using namespace bench;

using Signal = nod::signal< void( EventArg ) >;

/// Receiver with its OWN per-instance atomic state - isolates real concurrent
/// dispatch cost from the cache-line contention that a single shared
/// global_sink updated by every receiver on every thread would otherwise
/// introduce.
struct LocalSinkReceiver
{
	std::atomic< EventArg > localSink{ 0 };
	void onFired( EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 7a - concurrent emission, nod's default behaviour
//
// nod::multithread_policy only protects copy_slots() (the connection-list
// snapshot); it does not serialise handler execution across threads.  This
// benchmark measures nod's real, unmodified default: N receivers, all
// CONCURRENT_THREAD_COUNT emitter threads firing on the SAME shared signal
// concurrently, with handler bodies genuinely allowed to run in parallel.
//
// Not comparable to nano TS / sigslot MT / vdk-signals' forced-sync
// PerReceiver numbers - those fully serialise handler execution internally;
// this does not.  See the file-level note above.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	Signal sig;
	std::vector< LocalSinkReceiver > receivers( static_cast< size_t >( state.range( 0 ) ) );
	std::vector< nod::connection > handles;
	handles.reserve( receivers.size() );

	for ( auto& r : receivers )
	{
		handles.push_back( sig.connect( std::bind( &LocalSinkReceiver::onFired, &r, std::placeholders::_1 ) ) );
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

	for ( auto& c : handles )
	{
		c.disconnect();
	}
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 9 - contention scaling
//
// BM_ContentionScaling_Direct: CONTENTION_RECEIVER_COUNT receivers are fixed;
// the only thing that varies is the number of threads simultaneously calling
// sig(...) on the SAME shared signal.
//
// nod's multithread_policy (its default, mutex_type = std::mutex) only
// protects the connection-list snapshot during dispatch, held only for
// copy_slots() - not for the duration of handler invocation, not handler
// execution itself, the same narrower guarantee as rocket's
// thread_safe_policy.  Included anyway since nod's default policy is
// genuinely named "multithread"; the guarantee-level difference is what
// this comparison exists to make visible, not a reason to exclude it.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< LocalSinkReceiver > receivers( CONTENTION_RECEIVER_COUNT );
	std::vector< nod::connection > handles;
	handles.reserve( receivers.size() );

	for ( auto& r : receivers )
	{
		handles.push_back( sig.connect( std::bind( &LocalSinkReceiver::onFired, &r, std::placeholders::_1 ) ) );
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

	for ( auto& c : handles )
	{
		c.disconnect();
	}
}
BENCHMARK( BM_ContentionScaling_Direct )
	->Arg( 1 )
	->Arg( 2 )
	->Arg( 4 )
	->Arg( 8 )
	->Arg( 16 )
	->Arg( 32 );

// ============================================================================
// Scenario 7a - concurrent emission, externally serialised
//
// Identical workload to BM_ConcurrentEmission_PerReceiver above, but each
// emitter thread holds a shared std::mutex for the duration of its sig(...)
// call, forcing one-handler-at-a-time execution - the genuinely equivalent
// comparison point against nano TS / sigslot MT / vdk-signals' forced-sync
// numbers, at the cost of a lock nod itself doesn't provide.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver_Serialized( benchmark::State& state )
{
	Signal sig;
	std::vector< LocalSinkReceiver > receivers( static_cast< size_t >( state.range( 0 ) ) );
	std::vector< nod::connection > handles;
	handles.reserve( receivers.size() );

	for ( auto& r : receivers )
	{
		handles.push_back( sig.connect( std::bind( &LocalSinkReceiver::onFired, &r, std::placeholders::_1 ) ) );
	}

	std::mutex serializeMutex;
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sig, &serializeMutex, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					std::lock_guard< std::mutex > lock( serializeMutex );
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

	for ( auto& c : handles )
	{
		c.disconnect();
	}
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver_Serialized )
	->Arg( 1 )
	->Arg( 5 );


BENCHMARK_MAIN();
