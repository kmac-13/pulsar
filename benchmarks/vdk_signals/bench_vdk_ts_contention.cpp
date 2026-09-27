/**
 * @file bench_vdk_ts_contention.cpp
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
 * This file: BM_ContentionScaling_Direct.
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
/// BM_ContentionScaling_Direct below - isolates real concurrent dispatch
/// cost from the cache-line contention that a single shared g_sink written
/// by every receiver on every thread would otherwise introduce. Every other
/// benchmark in this file continues to use the ordinary Receiver (shared
/// g_sink) for continuity with prior numbers.
struct LocalSinkReceiver : public vdk_ns::context
{
	std::atomic< bench::EventArg > localSink{ 0 };
	void onFired( bench::EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 9 - contention scaling
//
// CONTENTION_RECEIVER_COUNT receivers are fixed throughout; the number of
// threads simultaneously calling sig() on the SAME shared signal is the
// only thing that varies, from 1 (no contention at all) up to 32.
// SpinBarrier (common/spin_barrier.h) synchronises pre-created emitter
// threads with the measured per-iteration loop, avoiding std::thread
// create/join inside the timed region.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	vdk_ns::signal< void( bench::EventArg ) > sig;
	std::vector< LocalSinkReceiver > receivers( bench::CONTENTION_RECEIVER_COUNT );

	for ( auto& r : receivers )
	{
		// force synchronous dispatch - see BM_ConcurrentEmission_Direct_Parallel's
		// comment above for why this is required, not just an optimization
		sig.connect( &r, &LocalSinkReceiver::onFired, vdk_ns::exec::sync );
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
