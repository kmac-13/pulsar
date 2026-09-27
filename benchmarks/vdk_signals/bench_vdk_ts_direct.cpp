/**
 * @file bench_vdk_ts_direct.cpp
 * @brief One of vdk-signals' five genuinely concurrent benchmark families.
 *
 * Each family is kept in its own dedicated executable - always thread-safe,
 * no ST variant.  Unlike nano/sigslot/rocket's single combined "_ts_2" file,
 * vdk's concurrent families each need their OWN separate process: running
 * multiple different thread-creating benchmarks sequentially in one process
 * was found to hang reliably on Windows/MinGW, root-caused to a thread_local
 * destructor issue in vdk's own channel handling, confirmed against multiple
 * independent MinGW-w64/GCC bug reports (see KNOWN_ISSUES.md for the full
 * diagnosis).  This is a genuine, confirmed root cause - unlike some of the
 * other libraries' splits, which are pragmatic mitigations for a hang whose
 * exact mechanism wasn't fully confirmed.
 *
 * This file: BM_ConcurrentEmission_Direct_Parallel, BM_ConcurrentEmission_Direct_Serialized.
 *
 * See bench_vdk.cpp for the four benchmarks that don't create any threads
 * (built both ST and TS from that one shared source file).
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include <signals.h>

#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

using namespace bench;
namespace vdk_ns = vdk;

/// Receiver with its OWN per-instance atomic state, used only by
/// BM_ConcurrentEmission_Direct_Parallel/_Serialized below - isolates real
/// concurrent dispatch cost from the cache-line contention that a single
/// shared global_sink written by every receiver on every thread would
/// otherwise introduce.  Every other benchmark in this file continues to use
/// the ordinary Receiver (shared global_sink) for continuity with prior numbers.
struct LocalSinkReceiver : public vdk_ns::context
{
	std::atomic< bench::EventArg > localSink{ 0 };
	void onFired( bench::EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 7a (execute-now, parallel) - concurrent Direct dispatch, no
// external serialisation
//
// Synchronous dispatch with no external serialisation at all - handler
// invocations from different emitter threads may genuinely run
// concurrently.  SpinBarrier (common/spin_barrier.h) synchronises the
// pre-created emitter threads with the measured per-iteration loop,
// avoiding std::thread create/join inside the timed region.
// ============================================================================

static void BM_ConcurrentEmission_Direct_Parallel( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	vdk_ns::signal< void( bench::EventArg ) > sig;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		// force synchronous dispatch to avoid queueing to the main thread
		// due to all Receivers having main thread context
		sig.connect( &r, &LocalSinkReceiver::onFired, vdk_ns::exec::sync );
	}

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( bench::CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( bench::CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( bench::CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < bench::CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sig, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < bench::CROSS_THREAD_EMISSIONS; ++i )
				{
					sig( bench::PAYLOAD );
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		// sig() is a synchronous, directly-dispatching call (no queue/drain),
		// so all emissions for this iteration are complete by the time every
		// emitter thread has reached endBarrier
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
		* bench::CROSS_THREAD_EMISSIONS
		* bench::CONCURRENT_THREAD_COUNT
	);
}
BENCHMARK( BM_ConcurrentEmission_Direct_Parallel )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 7a (execute-now, serialised) - concurrent Direct dispatch,
// externally forced to one handler at a time
//
// Same synchronous dispatch as BM_ConcurrentEmission_Direct_Parallel above,
// but every emitter thread takes serializeMutex before calling sig() -
// vdk's own exec::sync has no internal serialisation, so this external
// mutex is what makes handler invocations mutually exclusive across
// threads, the fair comparison point against libraries whose thread-safety
// policy serialises internally.
// ============================================================================

static void BM_ConcurrentEmission_Direct_Serialized( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	vdk_ns::signal< void( bench::EventArg ) > sig;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		sig.connect( &r, &LocalSinkReceiver::onFired, vdk_ns::exec::sync );
	}

	std::mutex serializeMutex;
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( bench::CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( bench::CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( bench::CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < bench::CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sig, &serializeMutex, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < bench::CROSS_THREAD_EMISSIONS; ++i )
				{
					std::lock_guard< std::mutex > lock( serializeMutex );
					sig( bench::PAYLOAD );
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
		* bench::CROSS_THREAD_EMISSIONS
		* bench::CONCURRENT_THREAD_COUNT
	);
}
BENCHMARK( BM_ConcurrentEmission_Direct_Serialized )
	->Arg( 1 )
	->Arg( 5 );

BENCHMARK_MAIN();
