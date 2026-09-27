#ifndef QT_BENCH_COMMON_H
#define QT_BENCH_COMMON_H

/**
 * @file qt_bench_common.h
 * @brief QObject types and thread-coordination helpers shared by all four
 * bench_qt_*.cpp translation units.
 *
 * To avoid duplication across the various Qt benchmarks, the common types
 * and function are implemented here.
 */

#include "../common/benchmark_helpers.h"

#include <QObject>

#include <atomic>
#include <chrono>
#include <thread>

// ============================================================================
// QObject subclasses
//
// Qt requires MOC processing (AUTOMOC ON in CMake).
// ============================================================================

class Sender : public QObject
{
	Q_OBJECT

public:
	explicit Sender( QObject* parent = nullptr ) : QObject( parent ) {}

signals:
	void fired( int value );

	/// Forwarding signal used in scenarios 6 and 7b.
	/// Connected to fired2 via AutoConnection on the same object so Qt
	/// queues the invocation to this object's thread before dispatching
	/// to receivers of fired2.
	void firedRaw( int value );
	void fired2( int value );

public:
	/// Wire firedRaw -> fired2 via AutoConnection so that calling firedRaw()
	/// from any thread queues a fired2() emission on this object's thread.
	void setupForwarding()
	{
		connect( this, &Sender::firedRaw, this, &Sender::fired2, Qt::AutoConnection );
	}
};

class Receiver : public QObject
{
	Q_OBJECT

public:
	explicit Receiver( QObject* parent = nullptr ) : QObject( parent ) {}

// NOTE: starting with Qt5, the `slots` keyword is no longer needed
// public slots:
	void onFired( int v ) { bench::global_sink.store( v, std::memory_order_relaxed ); }
};

/// Receiver with its OWN per-instance atomic state, used only by the
/// contention-scaling / Direct-dispatch benchmarks in bench_qt_4.cpp -
/// isolates real concurrent dispatch cost from the cache-line contention
/// that a single shared bench::global_sink written by every receiver on every
/// thread would otherwise introduce.  Every other benchmark continues to use
/// the ordinary Receiver (shared bench::global_sink) for continuity with
/// prior numbers.
class LocalSinkReceiver : public QObject
{
	Q_OBJECT
public:
	explicit LocalSinkReceiver( QObject* parent = nullptr ) : QObject( parent ) {}

	std::atomic< int > localSink{ 0 };

// NOTE: starting with Qt5, the `slots` keyword is no longer needed
// public slots:
	void onFired( int v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// qtSpinWait
//
// Short pure-spin phase for the common case, then falls back to sleep_for()
// rather than spinning on yield() indefinitely - once thread count meets or
// exceeds the CPU's core count, std::this_thread::yield() can spin without
// making progress: per Microsoft's own SwitchToThread documentation, it can
// only ever hand off to another thread on the SAME core, never a different
// one, even an idle one.  sleep_for() is used here rather than a
// condition_variable since this function takes a plain std::atomic<int>&
// with no persistent object to own a mutex/cv - sleep_for() needs no such
// state and uses a different OS mechanism (a real timer wait, not
// SwitchToThread()) not subject to the same same-core limitation.
// ============================================================================

inline void qtSpinWait( const std::atomic< int >& completions, int target )
{
	constexpr int SPIN_ATTEMPTS_BEFORE_SLEEP = 1000;
	for ( int spin = 0; spin < SPIN_ATTEMPTS_BEFORE_SLEEP; ++spin )
	{
		if ( completions.load( std::memory_order_relaxed ) >= target )
		{
			return;
		}
		std::this_thread::yield();
	}

	while ( completions.load( std::memory_order_relaxed ) < target )
	{
		std::this_thread::sleep_for( std::chrono::microseconds( 50 ) );
	}
}

#endif // QT_BENCH_COMMON_H
