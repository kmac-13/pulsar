/**
 * @file bench_vdk_ts_perreceiver.cpp
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
 * This file: BM_ConcurrentEmission_PerReceiver.
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
// Scenario 7a (receiver-deferred) - concurrent emission, per-receiver
// queuing
//
// Genuine cross-thread queue + drain on each receiver's own context - one
// or more emitter threads post, and each of N receiver-owning drain
// threads later drains and executes only what accumulated on its own
// channel.  SpinBarrier (common/spin_barrier.h) synchronises the
// pre-created emitter and drain threads with the measured per-iteration
// loop, avoiding std::thread create/join inside the timed region.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	const int perReceiverPerRound = bench::CONCURRENT_THREAD_COUNT * bench::CROSS_THREAD_EMISSIONS;

	vdk_ns::signal< void( bench::EventArg ) > sig;

	const int participants = n + bench::CONCURRENT_THREAD_COUNT + 1;  // drains + emitters + main
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( participants );
	SpinBarrier postingDoneBarrier( participants );
	SpinBarrier endBarrier( participants );

	// each drain thread constructs and connects its OWN Receiver as its
	// first action, binding that receiver's channel to this specific
	// thread, then loops: wait for a round to start, wait for all emitters
	// to finish posting, drain exactly what accumulated on its own channel,
	// signal done
	std::vector< std::thread > drainThreads;
	drainThreads.reserve( n );
	for ( int i = 0; i < n; ++i )
	{
		drainThreads.emplace_back( [ &sig, &startBarrier, &postingDoneBarrier, &endBarrier, &running, perReceiverPerRound ]() {
			Receiver r;  // constructed here, so this thread is its owning channel
			sig.connect( &r, &Receiver::onFired );  // default (queued) dispatch

			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				postingDoneBarrier.arrive();  // wait for emitters to finish this round
				vdk::signals_execute( static_cast< unsigned >( perReceiverPerRound ) );
				endBarrier.arrive();
			}
		} );
	}

	std::vector< std::thread > emitters;
	emitters.reserve( bench::CONCURRENT_THREAD_COUNT );
	for ( int t = 0; t < bench::CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sig, &startBarrier, &postingDoneBarrier, &endBarrier, &running ]() {
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

				// all posting for this round is done
				postingDoneBarrier.arrive();

				// pass through; don't start next round's posting before this round's drain finishes
				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		startBarrier.arrive();
		postingDoneBarrier.arrive();  // pass-through: unblocks once emitters have posted
		endBarrier.arrive();          // wait for every drain thread to finish this round
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}
	for ( auto& t : drainThreads )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() )
		* bench::CROSS_THREAD_EMISSIONS
		* bench::CONCURRENT_THREAD_COUNT
		* n
	);
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );

BENCHMARK_MAIN();
