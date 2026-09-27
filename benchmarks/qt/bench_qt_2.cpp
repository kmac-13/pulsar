/**
 * @file bench_qt_2.cpp
 * @brief Qt Signals/Slots benchmarks - part 2 of 4 (sender-affinity
 * forwarding cluster).
 *
 * See bench_qt_1.cpp for the full explanation of this 4-way split.
 *
 * Scenarios in this file:
 * 5.  Cross-thread deferred emission (Qt::QueuedConnection)
 * 6.  Thread-affinity forwarding - both the signal-forwarding and
 *     invokeMethod variants
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
// Scenario 5 - cross-thread deferred emission
//
// One thread emits; a second thread (the receiver's actual owning context)
// later drains and executes the handler via Qt::QueuedConnection - measures
// the full cross-thread round trip (queue, wake, dequeue, invoke), not just
// the emitting call.
// ============================================================================

static void BM_CrossThreadDeferred( benchmark::State& state )
{
	// receiver thread with its own event loop
	QThread receiverThread;
	receiverThread.start();

	Sender sender;
	auto receiver = std::make_unique< Receiver >();
	receiver->moveToThread( &receiverThread );

	// QueuedConnection: handler always posted to receiver's thread
	std::atomic< int > completions{ 0 };
	QObject::connect( &sender, &Sender::fired, receiver.get(), &Receiver::onFired, Qt::QueuedConnection );

	// drain signal: a second QueuedConnection that increments completions,
	// letting us know each emission has been processed on the receiver thread
	QObject::connect(
		&sender, &Sender::fired,
		receiver.get(),
		[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
		Qt::QueuedConnection
	);

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );

		for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
		{
			emit sender.fired( PAYLOAD );
		}

		// spin until the receiver thread has processed all queued events -
		// short pure-spin phase, then fall back to sleep_for() rather
		// than spinning on yield() indefinitely (see qtSpinWait's own
		// comment further down in this file for the full reasoning: per
		// Microsoft's own SwitchToThread documentation, yield() can only
		// ever hand off to another thread on the SAME core, so it can
		// spin without making progress once thread count meets or
		// exceeds the CPU's core count)
		{
			constexpr int SPIN_ATTEMPTS_BEFORE_SLEEP = 1000;
			int spin = 0;
			for ( ; spin < SPIN_ATTEMPTS_BEFORE_SLEEP; ++spin )
			{
				if ( completions.load( std::memory_order_relaxed ) >= CROSS_THREAD_EMISSIONS )
				{
					break;
				}
				std::this_thread::yield();
			}
			while ( completions.load( std::memory_order_relaxed ) < CROSS_THREAD_EMISSIONS )
			{
				std::this_thread::sleep_for( std::chrono::microseconds( 50 ) );
			}
		}
	}

	state.SetItemsProcessed(
		static_cast< int64_t >( state.iterations() ) * CROSS_THREAD_EMISSIONS
	);

	receiverThread.quit();
	receiverThread.wait();
}
BENCHMARK( BM_CrossThreadDeferred );

// ============================================================================
// Scenario 6 / 7b setup helper
//
// The signal-forwarding pattern:
//   firedRaw (emitted from any thread)
//     -> fired2 (queued to Sender's thread via AutoConnection)
//       -> Receiver::onFired (DirectConnection, runs on Sender's thread)
//
// This serialises multi-thread emission to a single thread before dispatch,
// equivalent to Pulsar's implicit deferred-to-EventLoop behaviour.
//
// Sender must live on its own QThread so AutoConnection has a non-main thread
// to queue to; otherwise AutoConnection degrades to DirectConnection when the
// emitting thread matches the sender's thread.
// ============================================================================

// ============================================================================
// Scenario 6 - thread-affinity forwarding (single emitter -> receiver thread)
//
// The classic Qt approach: emitter threads call emit firedRaw() directly.
// AutoConnection detects the thread mismatch (emitter thread != senderThread)
// and posts fired2 to senderThread's event queue - one queue post per
// emission, no invokeMethod needed.  senderThread drains its queue, emits
// fired2, and receivers are called via DirectConnection on senderThread.
//
// Chain per emission:
//   emit firedRaw() [emitter thread]
//     -> AutoConnection detects cross-thread -> posts fired2 to senderThread
//       -> senderThread drains -> emit fired2 [senderThread]
//         -> onFired + completion lambda [DirectConnection, senderThread]
//
// This is likely what most Qt engineers use for thread-affinity forwarding.
// Compare with BM_ThreadAffinityForwarding_InvokeMethod for the faster
// (but less commonly known) invokeMethod approach, and with Pulsar's
// BM_ThreadAffinityForwarding for that library's built-in equivalent.
//
// Threads are pre-created and synchronised with a barrier so thread creation
// cost is excluded from the measurement.  spinWait uses yield() to avoid
// burning CPU cycles that senderThread's event loop needs.  SpinBarrier (in
// spin_barrier.h) and qtSpinWait (in qt_bench_common.h) are used.
// ============================================================================

static void BM_ThreadAffinityForwarding_SignalForwarding( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	QThread senderThread;
	senderThread.start();

	auto sender = std::make_unique< Sender >();
	sender->moveToThread( &senderThread );
	sender->setupForwarding();

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		QObject::connect( sender.get(), &Sender::fired2, r.get(), &Receiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	// completion is counted once per emission, not once per receiver - a
	// separate DirectConnection on the same fired2 signal, anchored on the
	// last receiver purely so it has a QObject context to be destroyed
	// with, same convention Pulsar's BM_SenderDeferral_ThreadAffinity uses
	std::atomic< int > completions{ 0 };
	QObject::connect(
		sender.get(), &Sender::fired2,
		receivers.back().get(),
		[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
		Qt::DirectConnection
	);

	const int totalEmissions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT;

	// pre-create threads outside the measured loop; synchronise per iteration
	// with a barrier to avoid thread creation overhead in the hot path
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
					// emit firedRaw directly from this (non-senderThread) thread -
					// AutoConnection sees the thread mismatch and posts fired2
					// to senderThread's queue (one queue post per emission,
					// no invokeMethod needed), which is the classic Qt pattern
					// most engineers use for thread-affinity forwarding
					emit sender->firedRaw( PAYLOAD );
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
		qtSpinWait( completions, totalEmissions );
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

	senderThread.quit();
	senderThread.wait();
}
BENCHMARK( BM_ThreadAffinityForwarding_SignalForwarding )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 6 (InvokeMethod) - thread-affinity forwarding
//
// The faster (but less commonly-known) Qt approach: emitter threads use
// QMetaObject::invokeMethod with Qt::QueuedConnection to post a lambda
// directly to senderThread's queue.  The lambda emits fired() on senderThread;
// receivers are called via DirectConnection.  No intermediate signal needed.
//
// Chain per emission:
//   invokeMethod(sender, lambda, QueuedConnection) [emitter thread]
//     -> posts lambda to senderThread's queue
//       -> senderThread drains -> lambda runs -> emit fired() [senderThread]
//         -> onFired + completion lambda [DirectConnection, senderThread]
//
// Avoids the firedRaw -> AutoConnection -> fired2 forwarding chain entirely -
// one queue post, one signal emission, vs SignalForwarding's one queue post
// plus two signal emissions.  Compare with BM_ThreadAffinityForwarding_SignalForwarding
// to see the cost of the forwarding chain, and with Pulsar's BM_ThreadAffinityForwarding
// for that library's built-in equivalent.
// ============================================================================

static void BM_ThreadAffinityForwarding_InvokeMethod( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	QThread senderThread;
	senderThread.start();

	auto sender = std::make_unique< Sender >();
	sender->moveToThread( &senderThread );

	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );
	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		QObject::connect( sender.get(), &Sender::fired, r.get(), &Receiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	// completion is counted once per emission, not once per receiver - see
	// the identical convention in BM_ThreadAffinityForwarding_SignalForwarding
	// above
	std::atomic< int > completions{ 0 };
	QObject::connect(
		sender.get(), &Sender::fired,
		receivers.back().get(),
		[ &completions ]( int ) { completions.fetch_add( 1, std::memory_order_relaxed ); },
		Qt::DirectConnection
	);

	const int totalEmissions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT;

	// pre-create threads outside the measured loop; synchronise per iteration
	// with a barrier to avoid thread creation overhead in the hot path
	std::atomic< bool > running{ true };
	SpinBarrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	SpinBarrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::atomic< int > invokeFailures{ 0 };

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
		startBarrier.arrive();   // release emitters
		endBarrier.arrive();     // wait for all emitters to finish posting
		qtSpinWait( completions, totalEmissions );
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
		static_cast< int64_t >( state.iterations() ) * totalEmissions
	);

	senderThread.quit();
	senderThread.wait();
}
BENCHMARK( BM_ThreadAffinityForwarding_InvokeMethod )
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
