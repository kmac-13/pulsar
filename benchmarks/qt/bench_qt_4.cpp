/**
 * @file bench_qt_4.cpp
 * @brief Qt Signals/Slots benchmarks - part 4 of 4 (direct/contention-
 * scaling cluster).
 *
 * See bench_qt_1.cpp for the full explanation of this 4-way split.
 *
 * Scenarios in this file:
 * 7a  (execute-now).  Concurrent Qt::DirectConnection, two variants.
 * 9.  Contention scaling (BM_ContentionScaling_Direct).
 *
 * Uses LocalSinkReceiver (its own per-instance atomic state) rather than
 * the shared-bench::global_sink Receiver used in bench_qt_1/2/3.cpp.
 *
 * Requires:
 *   find_package( Qt6 COMPONENTS Core REQUIRED )
 *   Target must have AUTOMOC ON or qt_add_executable used.
 *
 * Important: Qt benchmarks require a QCoreApplication instance.  Google
 * Benchmark's BENCHMARK_MAIN() does not create one, so we provide a custom
 * main() that initialises QCoreApplication first (see the end of this file).
 *
 * Sender, LocalSinkReceiver, and SpinBarrier are shared amoungst several
 * bench_qt_*.cpp via common and qt_bench includes - see those headers for
 * their definitions.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include "qt_bench_common.h"

#include <benchmark/benchmark.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include <memory>
#include <thread>
#include <vector>

using namespace bench;

// ============================================================================
// Scenario 7a (execute-now) - concurrent Qt::DirectConnection, two variants
//
// The BM_ConcurrentEmission_PerReceiver / BM_ReceiverDeferral_Serialised
// benchmarks above measure Qt::QueuedConnection - a genuinely different,
// heavier operation than "execute now" synchronous dispatch.
//
// Qt::DirectConnection dispatches synchronously, inline, on whichever thread
// calls emit - but unlike every other library's thread-safe policy, Qt
// provides NO internal locking at all for this path (Qt's own docs are
// explicit that concurrent direct-connected invocation from multiple
// threads is the caller's responsibility).  With multiple emitter threads
// and multiple receivers, that means genuinely parallel handler execution
// with zero synchronisation from Qt itself:
//
// - BM_ConcurrentEmission_Direct_Parallel: Qt::DirectConnection exactly as
//   Qt provides it, no external lock.  Comparable to nod's / rocket's real
//   (unmodified) default and Stellyra's SharedEvent-based
//   BM_ConcurrentEmission_Direct_Parallel.
//
// - BM_ConcurrentEmission_Direct_Serialized: identical workload, with an
//   external std::mutex held around each emit call, forcing
//   one-handler-at-a-time execution - the genuinely equivalent
//   comparison point against nano TS / sigslot MT / vdk-signals
//   forced-sync / nod Serialized / rocket Serialized / Stellyra's
//   Event-based BM_ConcurrentEmission_Direct_Serialized, at the cost of a
//   lock Qt itself doesn't provide.
//
// No QThread or event loop is involved in either variant - DirectConnection
// never queues, so receivers are plain stack-local QObjects, not moved to
// worker threads, matching nano/sigslot/rocket/nod's simpler (non-drain-
// thread) shape rather than other concurrent Qt benchmarks.
// ============================================================================

static void BM_ConcurrentEmission_Direct_Parallel( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::vector< std::unique_ptr< LocalSinkReceiver > > receivers;
	receivers.reserve( n );

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< LocalSinkReceiver >();
		QObject::connect( &sender, &Sender::fired, r.get(), &LocalSinkReceiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sender, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					emit sender.fired( PAYLOAD );
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
		* CONCURRENT_THREAD_COUNT );
}
BENCHMARK( BM_ConcurrentEmission_Direct_Parallel )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 9 - Contention scaling
//
// Isolates how Qt's DirectConnection dispatch (no locking at all, by
// design) scales as genuine concurrent contention increases, independent
// of receiver count.  CONTENTION_RECEIVER_COUNT receivers are fixed
// throughout; the number of threads simultaneously calling fired() on the
// SAME shared signal is the only thing that varies, from 1 (no contention
// at all) up to 32.  Compare directly against Stellyra's and vdk's
// BM_ContentionScaling_Direct at the same thread counts.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::vector< std::unique_ptr< LocalSinkReceiver > > receivers;
	receivers.reserve( CONTENTION_RECEIVER_COUNT );

	for ( int i = 0; i < CONTENTION_RECEIVER_COUNT; ++i )
	{
		auto r = std::make_unique< LocalSinkReceiver >();
		QObject::connect( &sender, &Sender::fired, r.get(), &LocalSinkReceiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( threadCount + 1 );
	SpinBarrier endBarrier( threadCount + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( threadCount );

	for ( int t = 0; t < threadCount; ++t )
	{
		emitters.emplace_back( [ &sender, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					emit sender.fired( PAYLOAD );
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
		* threadCount );
}
BENCHMARK( BM_ContentionScaling_Direct )
	->Arg( 1 )
	->Arg( 2 )
	->Arg( 4 )
	->Arg( 8 )
	->Arg( 16 )
	->Arg( 32 );

// ============================================================================
// Scenario 7a (execute-now, serialised) - concurrent Qt::DirectConnection,
// externally forced to one handler at a time
//
// Same synchronous Qt::DirectConnection dispatch as
// BM_ConcurrentEmission_Direct_Parallel above, but every emitter thread
// takes an external mutex before calling emit - Qt's DirectConnection has
// no internal serialisation of its own, so this external mutex is what
// makes handler invocations mutually exclusive across threads, the fair
// comparison point against libraries whose thread-safety policy
// serialises internally.
// ============================================================================

static void BM_ConcurrentEmission_Direct_Serialized( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::vector< std::unique_ptr< LocalSinkReceiver > > receivers;
	receivers.reserve( n );

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< LocalSinkReceiver >();
		QObject::connect( &sender, &Sender::fired, r.get(), &LocalSinkReceiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	std::mutex serializeMutex;
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sender, &serializeMutex, &startBarrier, &endBarrier, &running ]() {
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
					emit sender.fired( PAYLOAD );
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
		* CONCURRENT_THREAD_COUNT );
}
BENCHMARK( BM_ConcurrentEmission_Direct_Serialized )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Custom main - QCoreApplication required before any QObject is used
// ============================================================================

int main( int argc, char** argv )
{
	// QCoreApplication must be created before any QObject, QThread, or
	// signal/slot machinery is used; Google Benchmark's BENCHMARK_MAIN()
	// macro does not create one, so we provide our own main()
	QCoreApplication app( argc, argv );

	::benchmark::Initialize( &argc, argv );
	if ( ::benchmark::ReportUnrecognizedArguments( argc, argv ) )
	{
		return 1;
	}
	::benchmark::RunSpecifiedBenchmarks();
	::benchmark::Shutdown();
	return 0;
}
