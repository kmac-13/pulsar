/**
 * @file bench_vdk_ts_threadaffinity.cpp
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
 * This file: BM_ThreadAffinityForwarding.
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

struct Receiver : public vdk_ns::context
{
	void onFired( bench::EventArg v ) { bench::global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 6 - thread-affinity forwarding
//
// Forcing dispatch back onto the receiver's own owning thread via vdk's
// native per-thread channel model (the default queued dispatch used here,
// drained explicitly with signals_execute()) rather than any external
// forwarding trick.  SpinBarrier (common/spin_barrier.h) synchronises the
// pre-created emitter threads with the measured per-iteration loop,
// avoiding std::thread create/join inside the timed region.
// ============================================================================

static void BM_ThreadAffinityForwarding( benchmark::State& state )
{
	vdk_ns::signal< void( bench::EventArg ) > sig;
	Receiver r;  // created on this thread, so this is its owning thread

	sig.connect( &r, &Receiver::onFired );  // default (queued) dispatch

	const int totalEmissions = bench::CROSS_THREAD_EMISSIONS * bench::CONCURRENT_THREAD_COUNT;

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( bench::CONCURRENT_THREAD_COUNT + 1 );  // N emitters + main
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
		startBarrier.arrive();   // release all emitters for this iteration
		endBarrier.arrive();     // wait for all emitters to finish posting
		vdk::signals_execute( static_cast< unsigned >( totalEmissions ) );  // drain everything posted this iteration
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * totalEmissions );
}
BENCHMARK( BM_ThreadAffinityForwarding );

BENCHMARK_MAIN();
