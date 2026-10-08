/**
 * @file bench_qt_3.cpp
 * @brief Qt Signals/Slots benchmarks - part 3 of 4 (receiver-deferred /
 * serialised dispatch cluster).
 *
 * See bench_qt_1.cpp for the full explanation of this 4-way split.
 *
 * Scenarios in this file:
 * 7a. Concurrent emission, per-receiver queuing
 * 7b. Concurrent emission, serialised dispatch (sender-side and
 *     receiver-side variants)
 *
 * Requires:
 *   find_package( Qt6 COMPONENTS Core REQUIRED )
 *   Target must have AUTOMOC ON or qt_add_executable used.
 *
 * Important: Qt benchmarks require a QCoreApplication instance.  Google
 * Benchmark's BENCHMARK_MAIN() does not create one, so we provide a custom
 * main() that initialises QCoreApplication first (see the end of this file).
 *
 * Sender, Receiver, and qtSpinWait are shared amoungst all bench_qt_*.cpp
 * benchmarks via qt_bench_common.h - see that header for their definitions.
 */

#include "../common/benchmark_helpers.h"
#include "../common/spin_barrier.h"

#include "qt_bench_common.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include <benchmark/benchmark.h>

#include <memory>
#include <thread>
#include <vector>

using namespace bench;

// ============================================================================
// Scenario 7a - concurrent emission, per-receiver queuing
//
// Multiple threads emit directly via Qt::AutoConnection.  Each receiver on a
// different thread gets its own QMetaCallEvent posted to its thread's queue.
// Measures the O(threads * receivers) queue push cost.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;

	// each receiver lives on its own QThread
	std::vector< std::unique_ptr< QThread > > threads;
	std::vector< std::unique_ptr< Receiver > > receivers;
	threads.reserve( n );
	receivers.reserve( n );

	std::atomic< int > completions{ 0 };

	for ( int i = 0; i < n; ++i )
	{
		auto t = std::make_unique< QThread >();
		auto r = std::make_unique< Receiver >();
		r->moveToThread( t.get() );
		t->start();

		QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::QueuedConnection );
		QObject::connect(
			&sender, &Sender::fired,
			r.get(),
			[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
			Qt::QueuedConnection
		);

		threads.push_back( std::move( t ) );
		receivers.push_back( std::move( r ) );
	}

	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	// pre-create emitter threads outside the measured loop; synchronise per
	// iteration with a barrier to avoid thread creation/teardown overhead in
	// the hot path (same pattern as BM_ThreadAffinityForwarding_SignalForwarding
	// and both Stellyra counterparts of this scenario)
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );  // N emitters + main
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
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();   // release emitters
		endBarrier.arrive();     // wait for all emitters to finish posting

		qtSpinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT
	);

	for ( auto& t : threads )
	{
		t->quit();
		t->wait();
	}
}
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );  // allow time for N receiver threads to drain

// ============================================================================
// Scenario 7b - concurrent emission, serialised via signal forwarding +
// explicit queued receiver connections
//
// Multiple threads emit firedRaw; the AutoConnection queues fired2 to the
// sender's thread.  Once there, EVERY receiver is connected with an explicit
// Qt::QueuedConnection (not Direct) - a real, valid pattern: forward to a
// known thread first, then still queue each slot rather than assume Direct
// is safe once "home".  This is the genuinely equivalent test to Stellyra's
// Deferred-per-receiver mechanism, plus the extra cost of the forwarding hop
// itself.  N threads -> 1 forwarding queue entry per emission -> N further
// queued entries (one per receiver) on the SAME senderThread, all drained
// serially by that one thread's event loop.
// ============================================================================

static void BM_ConcurrentEmission_Serialised_SignalForwarding( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	QThread senderThread;
	senderThread.start();

	auto sender = std::make_unique< Sender >();
	sender->moveToThread( &senderThread );
	sender->setupForwarding();

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	std::atomic< int > completions{ 0 };

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		r->moveToThread( &senderThread );

		// explicit QueuedConnection, not Direct: matches Stellyra's Deferred
		// semantics - always queue, even though sender and receiver now
		// share the same thread post-forwarding
		QObject::connect( sender.get(), &Sender::fired2, r.get(), &Receiver::onFired, Qt::QueuedConnection );

		// a second QueuedConnection per receiver for completion counting,
		// matching Stellyra's "second Deferred connection per receiver"
		// convention, so totalCompletions scales with n the same way
		QObject::connect(
			sender.get(), &Sender::fired2,
			r.get(),
			[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
			Qt::QueuedConnection
		);

		receivers.push_back( std::move( r ) );
	}

	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	// pre-create emitter threads outside the measured loop; synchronise per
	// iteration with a barrier to avoid thread creation/teardown overhead in
	// the hot path (same fix applied to PerReceiver and ThreadAffinityForwarding)
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
					emit sender->firedRaw( PAYLOAD );
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();
		endBarrier.arrive();
		qtSpinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * totalCompletions
	);

	senderThread.quit();
	senderThread.wait();
}
BENCHMARK( BM_ConcurrentEmission_Serialised_SignalForwarding )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 7b (InvokeMethod) - concurrent emission, serialised dispatch,
// with explicit queued receiver connections
//
// Uses invokeMethod directly to post fired() to the sender's thread - one
// queue post per emission for the forwarding hop.  Once on senderThread,
// EVERY receiver is connected with an explicit Qt::QueuedConnection (not
// Direct) - see the note above BM_ConcurrentEmission_Serialised_SignalForwarding
// for why this, not a Direct fan-out, is the genuinely equivalent test to
// Stellyra's Deferred-per-receiver mechanism.
// ============================================================================

static void BM_ConcurrentEmission_Serialised_InvokeMethod( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	QThread senderThread;
	senderThread.start();

	auto sender = std::make_unique< Sender >();
	sender->moveToThread( &senderThread );

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	std::atomic< int > completions{ 0 };

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		r->moveToThread( &senderThread );

		// explicit QueuedConnection, not Direct: matches Stellyra's Deferred
		// semantics - always queue, even though sender and receiver now
		// share the same thread post-invokeMethod
		QObject::connect( sender.get(), &Sender::fired, r.get(), &Receiver::onFired, Qt::QueuedConnection );
		receivers.push_back( std::move( r ) );
	}

	// a second QueuedConnection per receiver for completion counting,
	// matching Stellyra's "second Deferred connection per receiver"
	// convention, so totalCompletions scales with n the same way
	for ( auto& r : receivers )
	{
		QObject::connect(
			sender.get(), &Sender::fired,
			r.get(),
			[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
			Qt::QueuedConnection
		);
	}

	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	std::atomic< bool > running{ true };
	std::atomic< int > invokeFailures{ 0 };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );
	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sender, &startBarrier, &endBarrier, &running, &invokeFailures ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					// one queue post: invokeMethod -> emit fired directly
					const bool ok = QMetaObject::invokeMethod(
						sender.get(),
						[ s = sender.get() ]() { emit s->fired( PAYLOAD ); },
						Qt::QueuedConnection
					);
					if ( !ok )
					{
						invokeFailures.fetch_add( 1, std::memory_order_relaxed );
					}
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();
		endBarrier.arrive();
		qtSpinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	if ( invokeFailures.load( std::memory_order_relaxed ) > 0 )
	{
		state.SkipWithError( "QMetaObject::invokeMethod returned false at least once" );
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * totalCompletions
	);

	senderThread.quit();
	senderThread.wait();
}
BENCHMARK( BM_ConcurrentEmission_Serialised_InvokeMethod )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 7b (QueuedConnection) - the genuine structural equivalent of
// Stellyra's BM_ReceiverDeferral_Serialised
//
// SignalForwarding/InvokeMethod above are sender-forwarding tricks (queue
// ONCE to reach a specific thread, then fan out to every receiver via
// DirectConnection, which never queues at all) - functionally the same
// mechanism as Scenario 6, not a receiver-deferred one.  Stellyra's
// BM_ReceiverDeferral_Serialised does something different: EVERY receiver
// gets its own independent Deferred connection to a SHARED EventLoop, so
// one emission produces N separate EventLoop::post() calls, all funnelled
// through the same drain thread.
//
// This benchmark is Qt's real counterpart to that: every receiver is
// moved to the SAME shared drainThread and connected with a genuine
// Qt::QueuedConnection each - Qt's own signal-slot machinery queues each
// receiver's invocation independently based on the RECEIVER's thread
// affinity (no forwarding trick needed at all, since the sender doesn't
// need to live anywhere in particular here).  One emission from any emitter
// thread produces N separate queued items on drainThread, mirroring Stellyra's
// N separate EventLoop::post() calls per emission.
// ============================================================================

static void BM_ConcurrentEmission_Serialised_QueuedConnection( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	QThread drainThread;
	drainThread.start();

	// sender lives on the constructing thread; never moved - its own
	// thread affinity is irrelevant here, only each receiver's is
	Sender sender;

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	std::atomic< int > completions{ 0 };

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		r->moveToThread( &drainThread );

		// QueuedConnection, not AutoConnection: matches Stellyra's Deferred
		// semantics - always queue, unconditionally, regardless of which
		// thread happens to be emitting
		QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::QueuedConnection );

		// a second QueuedConnection per receiver for completion counting,
		// matching Stellyra's "second Deferred connection per receiver"
		// convention, so totalCompletions scales with n the same way
		QObject::connect(
			&sender, &Sender::fired,
			r.get(),
			[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
			Qt::QueuedConnection
		);

		receivers.push_back( std::move( r ) );
	}

	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

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
					// emitted directly from this emitter thread, no
					// forwarding hop; Qt queues each QueuedConnection'd
					// receiver independently because drainThread differs
					// from the calling thread
					emit sender.fired( PAYLOAD );
				}

				endBarrier.arrive();
			}
		} );
	}

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();
		endBarrier.arrive();
		qtSpinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * totalCompletions
	);

	drainThread.quit();
	drainThread.wait();
}
BENCHMARK( BM_ConcurrentEmission_Serialised_QueuedConnection )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 7b (Receiver Deferral) - concurrent emission, serialised dispatch
//
// Structurally identical to BM_ConcurrentEmission_PerReceiver above, with
// exactly one difference: all n receivers here share a SINGLE QThread
// instead of each getting its own. Same n receivers, same 2
// QueuedConnections per receiver, same emitter thread pattern, same total
// post count - the only thing that changes is whether those posts land in
// n independent queues drained in parallel (PerReceiver) or one shared
// queue drained serially by a single thread (this benchmark). At n=1 the
// two are identical by construction; the comparison only means something
// once n>1, run both at the same n and compare directly.
//
// RECEIVER-side Qt::QueuedConnection - not AutoConnection, no thread-
// affinity check, no forwarding chain.  This is the Qt equivalent of
// Stellyra's BM_ReceiverDeferral_Serialised (same name, same mechanism, same
// n=1/n=5 parameterization) - NOT BM_ConcurrentEmission_Serialised_SignalForwarding
// or BM_ConcurrentEmission_Serialised_InvokeMethod above, which force
// dispatch onto the SENDER's own thread regardless of caller; that
// mechanism's Stellyra counterpart is BM_SenderDeferral_ThreadAffinity, not
// this scenario.
// ============================================================================

static void BM_ReceiverDeferral_Serialised( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;

	// all n receivers share this ONE QThread - the entire point of this
	// scenario, compared with BM_ConcurrentEmission_PerReceiver, where each
	// receiver gets its own QThread instead
	QThread receiverThread;
	receiverThread.start();

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	std::atomic< int > completions{ 0 };
	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		r->moveToThread( &receiverThread );

		QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::QueuedConnection );

		// completion counter: second QueuedConnection per receiver, matching
		// BM_ConcurrentEmission_PerReceiver's two-connection-per-receiver
		// pattern above and Stellyra's BM_ReceiverDeferral_Serialised exactly -
		// every receiver counts its own completions, so totalCompletions
		// scales with n, not just with emission count.
		QObject::connect(
			&sender, &Sender::fired,
			r.get(),
			[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
			Qt::QueuedConnection
		);

		receivers.push_back( std::move( r ) );
	}

	const int totalEmissions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT;

	// pre-create emitter threads outside the measured loop; synchronise per
	// iteration with a barrier, same pattern as every other cross-thread
	// scenario in this file
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
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();
		endBarrier.arrive();

		qtSpinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * totalEmissions
	);

	receiverThread.quit();
	receiverThread.wait();
}
BENCHMARK( BM_ReceiverDeferral_Serialised )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );  // allow time for the shared drain thread to catch up

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
