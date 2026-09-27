/**
 * @file bench_qt_1.cpp
 * @brief Qt Signals/Slots benchmarks - part 1 of 4 (non-concurrent).
 *
 * Split from a single-file benchmark file into four cluster executables to
 * isolate a hang observed on Windows/MinGW when running the full suite
 * unfiltered in one process - mirroring the fix already proven necessary for
 * vdk's thread-creating benchmarks (see KNOWN_ISSUES.md).  Unlike vdk's
 * issue, a specific root cause for THIS hang hasn't been confirmed; splitting
 * is a pragmatic mitigation, not a diagnosis.  Qt's hang was observed
 * mid-family (inside BM_ThreadAffinityForwarding_SignalForwarding itself),
 * not just at a family boundary, and Qt has eleven distinct thread-creating
 * benchmark functions - far more than most other libraries in this suite
 * (Pulsar being the only library with more) - so this is a 4-way cluster
 * split, not a simple ST/TS-style 2-way split:
 *   bench_qt_1.cpp (this file) - no threads created at all
 *   bench_qt_2.cpp - sender-affinity forwarding cluster
 *   bench_qt_3.cpp - receiver-deferred / serialised dispatch cluster
 *   bench_qt_4.cpp - direct/contention-scaling cluster
 *
 * Scenarios in this file:
 * 1.   Single-threaded emission, 1 connection
 * 2.   Single-threaded emission, N connections
 * 2a.  Single-threaded emission, 1 Auto connection
 * 2b.  Single-threaded emission, N Auto connections
 * 2c.  Single-threaded emission, 1 connection, syntax comparison (lambda)
 * 3.   Connect / disconnect throughput
 * 4.   Scoped receiver lifetime (QObject destroyed; Qt auto-disconnects)
 * 8a.  Disconnect-by-target
 * 8b.  Bulk disconnect via QObject::disconnect(sender, signal, receiver, nullptr)
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

#include "qt_bench_common.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace bench;

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection (DirectConnection)
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	QObject::connect( &sender, &Sender::fired, &receiver, &Receiver::onFired, Qt::DirectConnection );

	for ( auto _ : state )
	{
		emit sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection );

// ============================================================================
// Scenario 2c - single-threaded emission, 1 connection, syntax comparison (lambda)
//
// Qt's 3-argument connect(sender, signal, functor) overload - no context
// object, so this connection lives until sender is destroyed (no separate
// tracked lifetime), matching the no-receiver-object shape used for the
// equivalent benchmark in the other libraries.
// ============================================================================

static void BM_Emit_1Connection_Lambda( benchmark::State& state )
{
	Sender sender;
	QObject::connect( &sender, &Sender::fired,
		[]( int v ) { global_sink.store( v, std::memory_order_relaxed ); } );

	for ( auto _ : state )
	{
		emit sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Lambda );

// ============================================================================
// Scenario 2 - single-threaded emission, N connections (DirectConnection)
// ============================================================================

static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::DirectConnection );
		receivers.push_back( std::move( r ) );
	}

	for ( auto _ : state )
	{
		emit sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 2 (Auto) - single-threaded emission, 1 connection (AutoConnection)
//
// Qt::AutoConnection checks QThread::currentThread() against the receiver's
// thread on EVERY emission.  Since sender and receiver are on the same thread
// here, it degrades to a direct call, but pays the thread-check cost each
// time.  Compare with BM_Emit_1Connection (DirectConnection) to isolate that
// overhead, and with Pulsar's BM_Emit_1Connection_Auto to show that Pulsar
// resolves the same check once at connect time rather than per-emission.
// ============================================================================

static void BM_Emit_1Connection_Auto( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	// Qt::AutoConnection: checks thread affinity on every emission:
	// same-thread -> direct call, but thread check is not free
	QObject::connect( &sender, &Sender::fired, &receiver, &Receiver::onFired );

	for ( auto _ : state )
	{
		emit sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Auto );

// ============================================================================
// Scenario 2 (Auto) - single-threaded emission, N connections (AutoConnection)
// ============================================================================

static void BM_Emit_NConnections_Auto( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::vector< std::unique_ptr< Receiver > > receivers;
	receivers.reserve( n );

	for ( int i = 0; i < n; ++i )
	{
		auto r = std::make_unique< Receiver >();
		// Qt::AutoConnection (default): per-emission thread check
		QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired );
		receivers.push_back( std::move( r ) );
	}

	for ( auto _ : state )
	{
		emit sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections_Auto )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 3 - connect / disconnect throughput
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	Sender sender;

	for ( auto _ : state )
	{
		std::vector< std::unique_ptr< Receiver > > receivers;
		std::vector< QMetaObject::Connection > handles;
		receivers.reserve( CONNECT_DISCONNECT_BATCH );
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
		{
			auto r = std::make_unique< Receiver >();
			handles.push_back(
				QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::DirectConnection )
			);
			receivers.push_back( std::move( r ) );
		}

		emit sender.fired( PAYLOAD );

		for ( auto& c : handles )
		{
			QObject::disconnect( c );
		}
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ============================================================================
// Scenario 3 (TrackedReceiver) - connect / disconnect throughput
//
// Counterpart to Pulsar's BM_ConnectDisconnect_TrackedReceiver.  The two
// mechanisms aren't equally weighted. Trackable is a narrow, optional role:
// it need not even be a base class of the receiver - connect() can take a
// separate Trackable (a member, or an unrelated object entirely) to own the
// connection's lifetime instead, so a type can get this guarantee without
// inheriting anything.  QObject is not optional in that sense - every
// receiver here derives from it because QObject is the load-bearing mechanism
// for Qt signals/slots in general (connecting, dispatch, moc integration, all
// of it), not a bolt-on chosen for auto-disconnect specifically; the
// automatic teardown is one piece of that larger, mandatory dependency, not a
// comparable opt-in.  The externally observable behaviour matches either way:
// destroy the receiver, its connections vanish, no explicit disconnect() call
// required on either side.  That's what this measures: N receiver
// constructions + N connects + one emit + N receiver destructions (each
// auto-disconnecting), with no explicit teardown loop - contrast with
// BM_ConnectDisconnect above, which disconnects explicitly via stored
// connection handles.
// ============================================================================

static void BM_ConnectDisconnect_TrackedReceiver( benchmark::State& state )
{
	Sender sender;

	for ( auto _ : state )
	{
		std::vector< std::unique_ptr< Receiver > > receivers;
		receivers.reserve( CONNECT_DISCONNECT_BATCH );

		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
		{
			auto r = std::make_unique< Receiver >();
			QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired,Qt::DirectConnection );
			receivers.push_back( std::move( r ) );
		}

		emit sender.fired( PAYLOAD );

		// receivers destroyed here (vector goes out of scope); ~QObject
		// automatically disconnects every connection involving each one -
		// no explicit QObject::disconnect() call needed
	}
}
BENCHMARK( BM_ConnectDisconnect_TrackedReceiver );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// Qt automatically disconnects all connections when a QObject is destroyed.
// Destroying the receiver (unique_ptr reset) is the Qt-idiomatic approach;
// no explicit QObject::disconnect() call is needed or measured.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Sender sender;

	for ( auto _ : state )
	{
		{
			auto receiver = std::make_unique< Receiver >();
			QObject::connect( &sender, &Sender::fired, receiver.get(), &Receiver::onFired, Qt::DirectConnection );
			emit sender.fired( PAYLOAD );

			// receiver destroyed here, Qt auto-disconnects
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

// ============================================================================
// Disconnect-only scenarios
// ============================================================================

// ============================================================================
// Scenario 8a - disconnect-by-target
//
// Counterpart to Pulsar's BM_DisconnectByTarget_LinearScan.  Deliberately
// not named with a complexity claim - Pulsar's name states its disconnect
// is a linear scan, per event_storage.h; Qt's internal
// QObjectPrivate::Connection list/removal implementation is not asserted
// to share that complexity here, so this measures the analogous operation
// (disconnect one specific (sender-signal, receiver-slot) connection out of
// n concurrent ones on the same signal) without claiming the underlying
// mechanism is the same.  Worst case: the target is the last one connected.
// ============================================================================

static void BM_DisconnectByTarget( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	Sender sender;

	for ( auto _ : state )
	{
		state.PauseTiming();
		std::vector< std::unique_ptr< Receiver > > receivers;
		receivers.reserve( n );
		for ( int i = 0; i < n; ++i )
		{
			auto r = std::make_unique< Receiver >();
			QObject::connect( &sender, &Sender::fired, r.get(), &Receiver::onFired, Qt::DirectConnection );
			receivers.push_back( std::move( r ) );
		}
		state.ResumeTiming();

		QObject::disconnect( &sender, &Sender::fired, receivers.back().get(), &Receiver::onFired );

		// matches Pulsar's BM_DisconnectByTarget_LinearScan's timing
		// boundary: PauseTiming() stops before setup, ResumeTiming()
		// starts before the one disconnect() call, and is NOT paused again
		// before this scope ends - so the remaining n-1 receivers' automatic
		// disconnection-on-destruction, and the vector's own teardown, are
		// both included in the timed region on both sides, not isolated out
	}
}
BENCHMARK( BM_DisconnectByTarget )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );

// ============================================================================
// Scenario 8b - bulk disconnect via QObject::disconnect(sender, signal, receiver, nullptr)
//
// Counterpart to Pulsar's BM_DisconnectTracker_Batch.  Named without
// "Tracker" deliberately - that word refers to Pulsar's Trackable base
// class, which has no Qt equivalent; Qt achieves the same externally
// observable operation (tear down every connection between one specific
// sender-signal and one specific receiver, in a single call, regardless of
// how many separate connect() calls created them) via
// QObject::disconnect(sender, signal, receiver, nullptr) - passing nullptr
// for the method disconnects every connection matching the
// (sender, signal, receiver) triple, per Qt's own documented behaviour,
// including ones created via distinct lambda connections the way this
// benchmark creates them.  One receiver, n distinct lambda connections to
// the same signal, one batch disconnect call.
// ============================================================================

static void BM_DisconnectAllForReceiver_Batch( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	Sender sender;

	for ( auto _ : state )
	{
		state.PauseTiming();
		Receiver receiver;
		for ( int i = 0; i < n; ++i )
		{
			QObject::connect( &sender, &Sender::fired, &receiver,
				[ i ]( int v ) { global_sink.store( v + i, std::memory_order_relaxed ); } );
		}
		state.ResumeTiming();

		QObject::disconnect( &sender, &Sender::fired, &receiver, nullptr );
	}
}
BENCHMARK( BM_DisconnectAllForReceiver_Batch )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 );

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
