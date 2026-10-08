/**
 * @file bench_rocket_ts_2.cpp
 * @brief The genuinely concurrent rocket benchmarks.
 *
 * Always thread-safe, no ST variant, since none of these make sense without
 * real concurrency.  Kept in a separate file from bench_rocket.cpp (which
 * produces both bench_rocket_st and bench_rocket_ts_1 from the same source,
 * compiled twice) for two reasons:
 * 1. These scenarios have no ST equivalent at all, so they don't belong in
 *    a file whose whole point is code shared between an ST and TS build.
 * 2. Isolating them into their own process/executable is also a pragmatic
 *    mitigation for a hang observed on Windows/MinGW when running the full
 *    rocket_ts suite unfiltered in one process - mirroring the fix already
 *    proven necessary for vdk's thread-creating benchmarks (see
 *    KNOWN_ISSUES.md).  A specific root cause for THIS hang hasn't been
 *    confirmed; splitting is a pragmatic mitigation, not a diagnosis.
 *
 * This file: BM_ConcurrentEmission_PerReceiver, BM_ContentionScaling_Direct,
 * BM_ConcurrentEmission_PerReceiver_Serialized.
 *
 * Note on thread safety: per rocket's source (signal::invoke()),
 * thread_safe_policy's lock_state.lock() covers only pinning the current
 * connection node via connection_scope; it is released (lock_state.unlock())
 * before the slot is actually invoked.  Safe to connect()/disconnect()
 * concurrently with an in-flight emission, but concurrent emit() calls from
 * different threads can have their handler invocations genuinely run in
 * parallel, with no mutual exclusion between them.
 *
 * Because of this, two PerReceiver variants are benchmarked rather than
 * one:
 * - BM_ConcurrentEmission_PerReceiver: rocket's default thread-safe
 *   behaviour, as a real user would get it - genuinely parallel handler
 *   execution across threads.  This is NOT the same operation as nano TS /
 *   sigslot MT / vdk-signals' forced-sync PerReceiver benchmarks (which
 *   fully serialise handler execution under their own internal lock), so
 *   it should not be compared to those numbers directly.
 * - BM_ConcurrentEmission_PerReceiver_Serialized: the same workload with
 *   an external std::mutex added around each sig(PAYLOAD) call, forcing
 *   one-handler-at-a-time execution.  This is the genuinely equivalent
 *   comparison point against the other libraries' internally-serialised
 *   numbers, at the cost of a lock rocket itself doesn't provide.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

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

/// Receiver with its OWN per-instance atomic state - isolates real
/// concurrent dispatch cost from the cache-line contention that a single
/// shared global_sink written by every receiver on every thread would
/// otherwise introduce.
struct LocalSinkReceiver : public rocket::trackable
{
	std::atomic< EventArg > localSink{ 0 };
	void onFired( EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 7a - concurrent emission (thread-safe variant only)
//
// BM_ConcurrentEmission_PerReceiver - rocket's real, unmodified thread-safe
// behaviour. See the file-level note: handler invocations from different
// threads can genuinely run in parallel. Not comparable to nano TS /
// sigslot MT / vdk-signals' forced-sync numbers.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		sig.connect( &r, &LocalSinkReceiver::onFired );
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
// fixed; the only thing that varies is the number of threads
// simultaneously calling sig(...) on the SAME shared signal.
//
// rocket's thread_safe_policy only protects the connection-list snapshot
// during dispatch: shared_lock wraps a plain std::mutex, held only long
// enough to copy the slot list, not for the duration of handler
// invocation - not handler execution itself, so this is a narrower
// "thread-safe" guarantee than Stellyra's/nano's/sigslot's full
// serialisation.  Included anyway since rocket's own policy is genuinely
// named and documented as thread-safe; the guarantee-level difference is
// what this comparison exists to make visible, not a reason to exclude it.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< LocalSinkReceiver > receivers( CONTENTION_RECEIVER_COUNT );

	for ( auto& r : receivers )
	{
		sig.connect( &r, &LocalSinkReceiver::onFired );
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

// ============================================================================
// Scenario 7a (execute-now, serialised)
//
// BM_ConcurrentEmission_PerReceiver_Serialized - identical workload, with
// each emitter thread holding a shared std::mutex for the duration of its
// sig(...) call, forcing one-handler-at-a-time execution.  This is the
// genuinely equivalent comparison point against the other libraries'
// internally-serialised numbers, at the cost of a lock rocket itself doesn't
// provide.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver_Serialized( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Signal sig;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		sig.connect( &r, &LocalSinkReceiver::onFired );
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
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver_Serialized )
	->Arg( 1 )
	->Arg( 5 );


BENCHMARK_MAIN();
