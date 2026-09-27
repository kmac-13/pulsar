/**
 * @file bench_vdk_ts_crossthread.cpp
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
 * This file: BM_CrossThreadDeferred.
 *
 * See bench_vdk.cpp for the four benchmarks that don't create any threads
 * (built both ST and TS from that one shared source file).
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include <signals.h>

#include <benchmark/benchmark.h>

#include <atomic>

using namespace bench;
namespace vdk_ns = vdk;

struct Receiver : public vdk_ns::context
{
	void onFired( bench::EventArg v ) { bench::global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 5 - cross-thread deferred emission
//
// One thread emits; a second thread (the receiver's actual owning context)
// later drains and executes the handler - measures the full cross-thread
// round trip (queue, wake, dequeue, invoke), not just the emitting call.
// SpinBarrier (common/spin_barrier.h) synchronises the pre-created emitter
// thread with the measured per-iteration loop, avoiding std::thread
// create/join inside the timed region.
// ============================================================================

static void BM_CrossThreadDeferred( benchmark::State& state )
{
	vdk_ns::signal< void( bench::EventArg ) > sig;
	Receiver r;  // created on this thread, so this is its owning thread

	sig.connect( &r, &Receiver::onFired );  // default (queued) dispatch - see note above

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( 2 );
	SpinBarrier endBarrier( 2 );

	std::thread emitter( [ &sig, &startBarrier, &endBarrier, &running ]() {
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

	for ( auto _ : state )
	{
		startBarrier.arrive();
		endBarrier.arrive();  // emitter has posted CROSS_THREAD_EMISSIONS entries

		// drain exactly what was posted this iteration - signals_execute() with
		// NO argument only executes one slot call per the docs, which would
		// leave 99 of every 100 posted entries queued and never drain them;
		// this bug was in the first version of this benchmark and is why that
		// earlier reported number should be discarded, not trusted
		vdk::signals_execute( static_cast< unsigned >( bench::CROSS_THREAD_EMISSIONS ) );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	emitter.join();

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * bench::CROSS_THREAD_EMISSIONS );
}
BENCHMARK( BM_CrossThreadDeferred );

BENCHMARK_MAIN();
