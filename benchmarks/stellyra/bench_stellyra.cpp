/**
 * @file bench_stellyra.cpp
 * @brief Stellyra benchmarks covering the various scenarios.
 *
 * Scenarios 5, 6, 7a, and 7b are only registered in thread-safe builds
 * (STELLYRA_THREAD_SAFE != 0) because they require AutoDrainThread.
 * Scenarios 2a/2b/2c use EventLoop without a drain thread and run in both.
 *
 * Stellyra's internal mutex-backend comparison (Mutex vs. RecursiveMutex vs.
 * SharedMutex vs. NullMutex) lives in bench_stellyra_mutex_variants.cpp, not
 * here - it's a Stellyra-vs-itself comparison not intended to be compared
 * against other libraries and mixing them into this file's output might
 * give the false impression that they are cross-library number when skimming
 * benchmark names.
 */

// TODO: consider collapsing the Event vs ConcurrentEvent duplicate benchmarks
// into single benchmarks templated on event type, e.g.:
// template< template< typename > class EventType >
// static void BM_Emit_1Connection_FreeNTTP( benchmark::State& state )
// {
// 	EventType< EventArg > fired;
// 	fired.template connectFree< &onFiredFree >( stellyra::ConnectionType::Direct );

// 	for ( auto _ : state )
// 	{
// 		fired( PAYLOAD );
// 	}
// }
// BENCHMARK_TEMPLATE( BM_Emit_1Connection_FreeNTTP, stellyra::Event );
// BENCHMARK_TEMPLATE( BM_Emit_1Connection_FreeNTTP, stellyra::ConcurrentEvent );


#include "../common/benchmark_helpers.h"

#include <kmac/stellyra/event.h>
#include <kmac/stellyra/concurrent_event.h>

#include <benchmark/benchmark.h>

#include <list>
#include <vector>

namespace stellyra = kmac::stellyra;
using namespace bench;

// ============================================================================
// Fixtures
// ============================================================================

/// Minimal sender: owns one Event<EventArg> (RecursiveMutex-backed - fully
/// serialised dispatch, connect()/disconnect() safe from any thread).
struct Sender : stellyra::Trackable
{
	stellyra::Event< EventArg > fired { this };
};

/// Same shape as Sender, but backed by ConcurrentEvent<EventArg> (regular
/// Mutex guarding connect()/disconnect() only - dispatch itself is
/// lock-free/epoch-based and does not serialise concurrent triggers the way
/// Event's RecursiveMutex does).  Added so every Stellyra-vs-Qt-vs-vdk
/// concurrent scenario can show both of Stellyra's thread-safe event types
/// side by side rather than only Event - see RUNNING.md / BENCHMARK_RESULTS.md
/// for why both are worth comparing: Event is Stellyra's primary/default event
/// type (see event.h) and carries the unsuffixed benchmark name throughout
/// this file; ConcurrentEvent lives in stellyra_extras and is the
/// "_ConcurrentEvent"-suffixed counterpart wherever both are measured.
struct ConcurrentSender : stellyra::Trackable
{
	stellyra::ConcurrentEvent< EventArg > fired { this };
};

/// Minimal receiver: writes received value to global_sink.
struct Receiver : stellyra::Trackable
{
	void onFired( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }
};

/// Free-function handler for free-function connection benchmarks.
static void onFiredFree( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); }

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection );

// ConcurrentEvent counterpart to BM_Emit_1Connection above - identical
// workload, only the event type differs. ConcurrentEvent inherits the full
// connect API from EventStorage (the same base BasicEvent uses), so
// Trackable-derived receivers get the same auto-disconnect-on-destruction
// behaviour and the same connect<&Method>(receiver, ...) call form as
// Event - nothing here needs a capturing lambda.
static void BM_Emit_1Connection_ConcurrentEvent( benchmark::State& state )
{
	ConcurrentSender sender;
	Receiver receiver;
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_ConcurrentEvent );

// ============================================================================
// Scenario 1 (NTTP free) / (runtime free) - free-function connection
//
// Event genuinely distinguishes the two forms: connectFree<&fn>() is a
// compile-time NTTP connection, connectFree(fn) takes fn as a runtime
// function pointer (see event_storage.h).  Kept as two named benchmarks that
// measure two real, distinct call sites.
//
// ConcurrentEvent inherits the same connectFree<&Func>()/connectFree(func)
// overload pair from EventStorage, so the two "_ConcurrentEvent" siblings
// below genuinely distinguish the NTTP and runtime forms too, same as the
// Event pair above.
// ============================================================================

static void BM_Emit_1Connection_FreeNTTP( benchmark::State& state )
{
	stellyra::Event< EventArg > fired;
	fired.connectFree< &onFiredFree >( stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_FreeNTTP );

static void BM_Emit_1Connection_FreeNTTP_ConcurrentEvent( benchmark::State& state )
{
	stellyra::ConcurrentEvent< EventArg > fired;
	fired.connectFree< &onFiredFree >( stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_FreeNTTP_ConcurrentEvent );

static void BM_Emit_1Connection_Free( benchmark::State& state )
{
	stellyra::Event< EventArg > fired;
	fired.connectFree( onFiredFree, stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Free );

static void BM_Emit_1Connection_Free_ConcurrentEvent( benchmark::State& state )
{
	stellyra::ConcurrentEvent< EventArg > fired;
	fired.connectFree( onFiredFree, stellyra::ConnectionType::Direct );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Free_ConcurrentEvent );

// ============================================================================
// Scenario 1 (lambda) - 1 directly-connected lambda, no tracker
// ============================================================================

static void BM_Emit_1Connection_Lambda( benchmark::State& state )
{
	stellyra::Event< EventArg > fired;
	fired.connectLambda( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Lambda );

// ConcurrentEvent counterpart to BM_Emit_1Connection_Lambda above - identical
// workload, only the event type differs.
static void BM_Emit_1Connection_Lambda_ConcurrentEvent( benchmark::State& state )
{
	stellyra::ConcurrentEvent< EventArg > fired;
	fired.connectLambda( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } );

	for ( auto _ : state )
	{
		fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Lambda_ConcurrentEvent );

// ============================================================================
// Scenario 2 (NTTP) - N NTTP method connections
//
// No dedicated benchmark: BM_Emit_NConnections above already connects via
// connect<&Receiver::onFired>(...), the NTTP form.
// ============================================================================

// ============================================================================
// Scenario 2 (runtime method) - N connections, runtime member-function
//
// Counterpart to BM_Emit_NConnections above: same setup, but the member
// pointer is passed as a plain runtime argument (connect(receiver,
// &T::method, ...)) rather than baked in as a template parameter
// (connect<&T::method>(...)).  Mirrors the Free/FreeNTTP pairing above, which
// does this comparison for free functions instead of member functions.
// ============================================================================

static void BM_Emit_NConnections_RuntimeMethod( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		sender.fired.connect( receivers.back(), &Receiver::onFired, stellyra::ConnectionType::Direct );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections_RuntimeMethod )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ConcurrentEvent counterpart to BM_Emit_NConnections_RuntimeMethod above -
// identical workload, only the event type differs.  ConcurrentEvent inherits
// the same runtime connect(receiver, &T::method, ...) overload from
// EventStorage that Event uses, so this genuinely isolates the same
// NTTP-vs-runtime-pointer distinction, against BM_Emit_NConnections_
// ConcurrentEvent above.
static void BM_Emit_NConnections_RuntimeMethod_ConcurrentEvent( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	ConcurrentSender sender;
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		sender.fired.connect( receivers.back(), &Receiver::onFired, stellyra::ConnectionType::Direct );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections_RuntimeMethod_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 2 - single-threaded emission, N connections
// ============================================================================

static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	// std::list keeps references stable as more receivers are added
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		sender.fired.connect< &Receiver::onFired >( receivers.back(), 0, stellyra::ConnectionType::Direct );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ConcurrentEvent counterpart to BM_Emit_NConnections above - identical
// workload, only the event type differs.  ConcurrentEvent inherits the same
// connect<&Method>(receiver, ...) form from EventStorage that Event uses -
// see BM_Emit_1Connection_ConcurrentEvent's comment above.
static void BM_Emit_NConnections_ConcurrentEvent( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	ConcurrentSender sender;
	// std::list keeps references stable as more receivers are added
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		sender.fired.connect< &Receiver::onFired >( receivers.back(), 0, stellyra::ConnectionType::Direct );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 2a - single-threaded emission, 1 Auto connection, same loop
//
// Both sender and receiver have no EventLoop assigned, so Auto resolves to
// Direct (nullptr == nullptr).  Compared with BM_Emit_1Connection (explicit
// Direct) to isolate the cost of the Auto resolution path.
// ============================================================================

static void BM_Emit_1Connection_Auto( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Auto );

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection_Auto );

// ============================================================================
// Scenario 2b - single-threaded emission, N Auto connections, same loop
// ============================================================================

static void BM_Emit_NConnections_Auto( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		sender.fired.connect< &Receiver::onFired >( receivers.back(), 0, stellyra::ConnectionType::Auto );
	}

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections_Auto )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 2c - 1 Auto connection, cross-loop (Deferred path)
//
// Receiver has a manually-drained EventLoop different from the sender's
// (none).  Auto resolves to Deferred.  Each iteration drains the loop so
// the queued handler actually runs - measures full emit-plus-drain cost.
// ============================================================================

static void BM_Emit_1Connection_Auto_CrossLoop( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	stellyra::EventLoop receiverLoop;

	receiver.setEventLoop( &receiverLoop );
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Auto );

	for ( auto _ : state )
	{
		sender.fired( PAYLOAD );
		receiverLoop.drain();
	}
}
BENCHMARK( BM_Emit_1Connection_Auto_CrossLoop );

// ============================================================================
// Scenario 3 - connect / disconnect throughput
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	stellyra::Event< EventArg > fired;

	for ( auto _ : state )
	{
		std::vector< stellyra::Connection > handles;
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
		{
			handles.push_back( fired.connectLambda( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } ) );
		}

		fired( PAYLOAD );

		for ( auto& c : handles )
		{
			c.disconnect();
		}
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ConcurrentEvent counterpart to BM_ConnectDisconnect above - identical
// workload, only the event type differs.  Event is Stellyra's primary/default
// event type (see event.h), so BM_ConnectDisconnect above measures it
// directly; ConcurrentEvent (Stellyra Extras) is the copy-on-write,
// lock-free-dispatch alternative and gets this suffixed sibling instead.
// Under a Stellyra ST build (STELLYRA_THREAD_SAFE=0) both this and
// BM_ConnectDisconnect have their locking compiled out to a no-op, so any
// remaining gap between the two here reflects Event's in-place,
// mutex-protected connection list versus ConcurrentEvent's copy-on-write
// snapshot rebuild on every connect()/disconnect() - not lock contention.
static void BM_ConnectDisconnect_ConcurrentEvent( benchmark::State& state )
{
	stellyra::ConcurrentEvent< EventArg > fired;

	for ( auto _ : state )
	{
		std::vector< stellyra::Connection > handles;
		handles.reserve( CONNECT_DISCONNECT_BATCH );

		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
		{
			handles.push_back( fired.connectLambda( []( EventArg v ) { global_sink.store( v, std::memory_order_relaxed ); } ) );
		}

		fired( PAYLOAD );

		for ( auto& c : handles )
		{
			c.disconnect();
		}
	}
}
BENCHMARK( BM_ConnectDisconnect_ConcurrentEvent );

// Same scenario, but using Trackable-derived receiver objects connected via
// connect<&Method>() rather than connectFree().  This is the fair
// apples-to-apples comparison point against every other library in this
// benchmark suite: Qt, vdk, rocket, nano, nod, and libsigc++ all require a
// receiver object per connection and bundle its construction/destruction
// cost into their own ConnectDisconnect numbers.  BM_ConnectDisconnect above
// (connectFree, no receiver object) remains a separate, legitimate data
// point - it reflects a lower-overhead connection pattern that's uniquely
// available in Stellyra, not a substitute for this one.
static void BM_ConnectDisconnect_TrackedReceiver( benchmark::State& state )
{
	Sender sender;

	for ( auto _ : state )
	{
		std::vector< Receiver > receivers( CONNECT_DISCONNECT_BATCH );

		for ( auto& r : receivers )
		{
			sender.fired.connect< &Receiver::onFired >( r, 0, stellyra::ConnectionType::Direct );
		}

		sender.fired( PAYLOAD );

		// receivers destroyed here; each Trackable dtor disconnects its
		// connection automatically - no explicit disconnect() needed.
	}
}
BENCHMARK( BM_ConnectDisconnect_TrackedReceiver );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// Receiver is stack-allocated inside each iteration.  Trackable auto-
// disconnects when it goes out of scope; no manual disconnect needed.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	Sender sender;

	for ( auto _ : state )
	{
		{
			Receiver receiver;
			sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Direct );
			sender.fired( PAYLOAD );
			// receiver destroyed here; Trackable dtor disconnects automatically
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

// ============================================================================
// NOTE: Scenarios 5, 6, and 7 are TS only (below, guarded behind STELLYRA_THREAD_SAFE).
// ============================================================================

// ============================================================================
// Scenario 8a - disconnect-by-target linear scan
//
// BM_DisconnectByTarget_LinearScan: connects N receivers to the same event
// with the same method, then disconnects one specific receiver (the last
// one connected) by (receiver, method) identity.  disconnect(receiver,
// method) breaks after its first match (single-match semantics, per
// event_storage.h), so this measures the linear-scan cost of *finding* the
// match as N grows - it does NOT exercise GenData's batch width, since the
// resulting toDisconnect vector never holds more than one entry regardless
// of N.  Kept as a separate, legitimate benchmark of scan cost, not as a
// GenData validation.
// ============================================================================

static void BM_DisconnectByTarget_LinearScan( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	Sender sender;

	for ( auto _ : state )
	{
		state.PauseTiming();
		std::vector< Receiver > receivers( n );
		for ( auto& r : receivers )
		{
			sender.fired.connect< &Receiver::onFired >( r );
		}
		state.ResumeTiming();

		// worst case: the target is the last one connected, so the scan
		// must walk past all n-1 others first
		sender.fired.disconnect( receivers.back(), &Receiver::onFired );
	}
}
BENCHMARK( BM_DisconnectByTarget_LinearScan )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 )->MinTime( 0.05 )->UseRealTime();

// ============================================================================
// Scenario 8b - bulk disconnect via Trackable::extractConnectionsTo()
//
// BM_DisconnectTracker_Batch: one receiver connects to the same single
// event N times (N distinct connectLambda closures), then
// event.disconnect(receiver) bulk-disconnects all N at once via
// Trackable::extractConnectionsTo() + disconnectHandlers() - the one real
// path that builds an N-entry std::vector<GenData>.  This is the scenario
// GenData's index-width change can actually show an effect on.
// ============================================================================

static void BM_DisconnectTracker_Batch( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );
	Sender sender;

	for ( auto _ : state )
	{
		state.PauseTiming();
		Receiver receiver;
		for ( int i = 0; i < n; ++i )
		{
			sender.fired.connectLambda( receiver, [ i ]( EventArg v ) { global_sink.store( v + i, std::memory_order_relaxed ); } );
		}
		state.ResumeTiming();

		sender.fired.disconnect( receiver );
	}
}
BENCHMARK( BM_DisconnectTracker_Batch )
	->Arg( 1 )->Arg( 10 )->Arg( 100 )->Arg( 1000 )->MinTime( 0.05 )->UseRealTime();

// ============================================================================
// Thread-safety-only scenarios - require AutoDrainThread
// ============================================================================

#if !defined( STELLYRA_THREAD_SAFE ) || STELLYRA_THREAD_SAFE != 0

#include <kmac/stellyra/auto_drain_thread.h>

// CROSS_THREAD_EMISSIONS and CONCURRENT_THREAD_COUNT come from
// benchmark_helpers.h, so both libraries measure identical workloads.

// Helper: wait until completions reaches target.  Calls yield() on each
// iteration to avoid burning CPU cycles that the drain thread needs,
// particularly on machines where emitter and drain threads share cores.
static void spinWait( const std::atomic< int >& completions, int target )
{
	// short pure-spin phase for the common case - see Barrier::arrive()
	// above for the full reasoning (same fallback, same justification,
	// just applied to a plain wait-for-a-counter rather than a reusable
	// barrier object) - once thread count meets or exceeds the CPU's core
	// count, std::this_thread::yield() can spin indefinitely without
	// making progress: per Microsoft's own SwitchToThread documentation,
	// it can only ever hand off to another thread on the SAME core;
	// sleep_for() is used here rather than a condition_variable, since
	// this function takes a plain std::atomic<int>& with no persistent
	// object to own a mutex/cv - sleep_for() needs no such state and uses
	// a different OS mechanism (a real timer wait, not SwitchToThread())
	// that isn't subject to the same same-core limitation
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

// Barrier: lets N pre-created threads synchronise the start and end of each
// benchmark iteration without creating/joining threads inside the hot loop.
struct Barrier
{
	const int _n;
	std::atomic< int > _count;
	std::atomic< int > _generation;
	std::mutex _mutex;
	std::condition_variable _cv;

	// number of pure-spin attempts (no yield) before falling back to a real
	// blocking wait - see the note on arrive() below for why the fallback exists
	static constexpr int SPIN_ATTEMPTS_BEFORE_BLOCKING = 1000;

	explicit Barrier( int n )
		: _n( n ), _count( 0 ), _generation( 0 )
	{
	}

	void arrive()
	{
		const int gen = _generation.load( std::memory_order_relaxed );
		if ( _count.fetch_add( 1, std::memory_order_acq_rel ) == _n - 1 )
		{
			_count.store( 0, std::memory_order_relaxed );
			_generation.fetch_add( 1, std::memory_order_release );

			// safe to notify without holding _mutex: _cv.wait(lock,
			// predicate) below re-checks the predicate (backed by the
			// atomic _generation) under the lock before actually
			// blocking, so a notify that "arrives early" relative to a
			// waiter isn't lost - the waiter's own recheck will see it
			_cv.notify_all();
			return;
		}

		// short pure-spin phase for the common case: at low thread
		// counts (at or below the CPU's core count), the barrier
		// resolves within microseconds and spinning avoids the cost of
		// a real OS wait/wake round trip
		for ( int spin = 0; spin < SPIN_ATTEMPTS_BEFORE_BLOCKING; ++spin )
		{
			if ( _generation.load( std::memory_order_acquire ) != gen )
			{
				return;
			}
			std::this_thread::yield();
		}

		// fall back to a real blocking wait once thread count exceeds core
		// count: yield() (SwitchToThread() on Windows) only hands off to a
		// thread on the SAME core, so a waiter whose partner is scheduled
		// elsewhere can spin forever;  a condition_variable wait has no
		// such limit (verified: an oversubscribed stress test hangs under
		// pure spinning and passes cleanly - incl. under ThreadSanitizer -
		// with this fallback)
		//
		// loops on wait_for(), not a single wait(): a reproduced hang once
		// stuck here despite the generation having already changed - a
		// lost-wakeup pattern seen before in MinGW's stdlib (see vdk's
		// thread_local dtor issue);  not a confirmed root cause, but the
		// periodic recheck makes this fallback self-healing regardless of
		// cause
		std::unique_lock< std::mutex > lock( _mutex );
		while ( _generation.load( std::memory_order_acquire ) == gen )
		{
			_cv.wait_for( lock, std::chrono::milliseconds( 5 ) );
		}
	}
};

// ============================================================================
// Scenario 5 - cross-thread deferred emission
//
// Sender emits on the main thread; receiver runs on a dedicated drain thread.
// Measures full round-trip latency: emit (post to receiver loop) + drain
// (handler execution on receiver thread).
//
// Completion tracking rides as a second Deferred connection on the same
// sender.fired signal.
// ============================================================================

static void BM_CrossThreadDeferred( benchmark::State& state )
{
	Sender sender;
	Receiver receiver;
	stellyra::EventLoop receiverLoop;
	stellyra::AutoDrainThread drainer( receiverLoop );

	receiver.setEventLoop( &receiverLoop );
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Deferred );

	std::atomic< int > completions { 0 };
	sender.fired.connectLambda( receiver,
		[ &completions ]( EventArg ) {
			completions.fetch_add( 1, std::memory_order_relaxed );
		},
		stellyra::ConnectionType::Deferred, 0 );

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );

		for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
		{
			sender.fired( PAYLOAD );
		}

		spinWait( completions, CROSS_THREAD_EMISSIONS );
	}

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * CROSS_THREAD_EMISSIONS );
}
BENCHMARK( BM_CrossThreadDeferred );

// ConcurrentEvent counterpart to BM_CrossThreadDeferred above - identical
// cross-thread queue+drain workload, only the sender's event type differs
// (ConcurrentSender/ConcurrentEvent instead of Sender/Event).
static void BM_CrossThreadDeferred_ConcurrentEvent( benchmark::State& state )
{
	ConcurrentSender sender;
	Receiver receiver;
	stellyra::EventLoop receiverLoop;
	stellyra::AutoDrainThread drainer( receiverLoop );

	receiver.setEventLoop( &receiverLoop );
	sender.fired.connect< &Receiver::onFired >( receiver, 0, stellyra::ConnectionType::Deferred );

	std::atomic< int > completions { 0 };
	sender.fired.connectLambda( receiver,
		[ &completions ]( EventArg ) {
			completions.fetch_add( 1, std::memory_order_relaxed );
		},
		stellyra::ConnectionType::Deferred, 0 );

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );

		for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
		{
			sender.fired( PAYLOAD );
		}

		spinWait( completions, CROSS_THREAD_EMISSIONS );
	}

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * CROSS_THREAD_EMISSIONS );
}
BENCHMARK( BM_CrossThreadDeferred_ConcurrentEvent );

// ============================================================================
// Scenario 6 (Sender Deferral) - automatic sender-thread-affinity forwarding
//
// Exercises BasicEvent::operator()'s built-in sender-loop check (event.h),
// NOT ConnectionType::Deferred and NOT ConnectionType::Auto - this path is
// unconditional and runs regardless of how any individual handler is
// connected.  Sender is given an EventLoop of its own, drained by a
// dedicated AutoDrainThread; when trigger() is called from any thread other
// than that loop's registered drain thread, the WHOLE trigger call
// (including Direct connections) is transparently posted to the sender's
// own loop and re-run there.  Calling from the registered drain thread
// itself dispatches synchronously with no queue round-trip at all - the
// same check-then-maybe-direct shape as Qt::AutoConnection, just keyed on
// the sender's home thread rather than the receiver's.
//
// This is Stellyra's built-in behavior, implemented in Qt by invokeMethod /
// signal-forwarding pattern (bench_qt.cpp's BM_ThreadAffinityForwarding_
// SignalForwarding / _InvokeMethod at n=1, and BM_ConcurrentEmission_
// Serialised_SignalForwarding / _InvokeMethod at general n): forcing
// execution back onto the thread that owns the object being called into,
// regardless of which thread made the call.
//
// Parameterized by n (number of receivers) to match Qt's pairing - there is
// only one variant here, not a PerReceiver/Serialised split like the
// receiver-side scenarios have, because sender-side deferral always funnels
// through the sender's single home loop by construction; there is no "each
// receiver gets its own independent loop" equivalent possible on this side.
//
// Do not confuse this with BM_ConcurrentEmission_PerReceiver or
// BM_ReceiverDeferral_Serialised below - those measure the receiver-side
// mechanism instead; see their own comments for the distinction.
// ============================================================================

static void BM_SenderDeferral_ThreadAffinity( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	stellyra::EventLoop senderLoop;
	stellyra::AutoDrainThread senderDrainer( senderLoop );  // registers itself as senderLoop's drain thread
	sender.setEventLoop( &senderLoop );

	// std::list keeps pointers stable, same reasoning as
	// BM_ConcurrentEmission_PerReceiver and BM_ReceiverDeferral_Serialised below
	std::list< Receiver > receivers;
	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		// explicit Direct connections
		sender.fired.connect< &Receiver::onFired >( receivers.back(), 0, stellyra::ConnectionType::Direct );
	}

	std::atomic< int > completions { 0 };
	const int totalEmissions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT;

	// completion is counted once per emission, not once per receiver - rides
	// as one more Direct connection on the same sender.fired signal,
	// anchored on the last receiver purely for Trackable bookkeeping
	sender.fired.connectLambda( receivers.back(),
		[ &completions ]( EventArg ) {
			completions.fetch_add( 1, std::memory_order_relaxed );
		},
		stellyra::ConnectionType::Direct, 0 );

	// pre-create threads outside the measured loop; synchronise per
	// iteration with a barrier, same as BM_ConcurrentEmission_PerReceiver below
	std::atomic< bool > running { true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );  // N emitters + main
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &sender, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();  // wait for main to start the iteration
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					// None of these emitter threads is senderLoop's
					// registered drain thread, so every call here takes
					// operator()'s deferred branch: the whole trigger
					// is posted to senderLoop and runs on
					// senderDrainer's thread instead of this one.
					sender.fired( PAYLOAD );
				}

				endBarrier.arrive();  // signal main that emissions are done
			}
		} );
	}

	for ( auto _ : state )
	{
		completions.store( 0, std::memory_order_relaxed );
		startBarrier.arrive();  // release emitters

		endBarrier.arrive();    // wait for all emitters to finish posting

		spinWait( completions, totalEmissions );
	}

	// stop all the threads
	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * totalEmissions );
}
BENCHMARK( BM_SenderDeferral_ThreadAffinity )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 7a - concurrent emission, per-receiver queuing
//
// Each of N receivers has its own EventLoop and AutoDrainThread.  Multiple
// threads emit simultaneously; each emission produces N queue entries, one
// per receiver loop.  Measures emission throughput under concurrent push
// contention spread across N independent queues.
//
// At n=1 this is Stellyra's fair comparison point against Qt's
// BM_ThreadAffinityForwarding_SignalForwarding / _InvokeMethod's receiver-
// side counterpart in bench_qt.cpp.
//
// NOTE(2026-07-13): The n=1 case of this scenario (at the time, still its
// own separate benchmark) hung once on Windows/MinGW, immediately after a
// clean build, and had to be killed manually; every run before and after
// that one - including five consecutive re-runs and a second attempt at the
// identical clean-build-then-run sequence - completed normally, so it has
// not reproduced on demand.  The one-time-only, first-run-after-build
// failure pattern is likely the result of a transient OS/AV scheduling
// contention on a freshly-built binary than a logic bug, but that is inferred
// from the pattern, not confirmed.  If this hangs again, try to attach a
// debugger to it and capture "thread apply all bt" for every thread - this
// works on a Release binary, but a plain -O2 Release build with no debug info
// gives thread names/addresses without source lines, and a stripped one loses
// user-code function names entirely (the actual blocked-on-what - mutex /
// condition-variable wait - still resolves by name either way, since that
// frame is inside the C++ runtime, not user code).  For a fully readable
// stack, capture it from a RelWithDebInfo build (same optimization level as
// Release, debug info kept) instead if one is set up.
// ============================================================================

// ============================================================================
// KNOWN DISCREPANCY (observed, not yet investigated - deliberately
// deprioritized for now, noted here so it isn't lost or mistaken for
// ordinary run-to-run variance if it resurfaces):
//
// This benchmark (and BM_ReceiverDeferral_Serialised/1 further below) reports
// meaningfully higher numbers when run as part of the FULL benchmark binary
// than when run in isolation via --benchmark_filter, on the same machine,
// back to back.  Isolated: ~1.4M ns; as part of the full ~30-benchmark run:
// ~2.0-2.2M ns - a 40-50% difference, reproduced across two separate full-suite
// runs.  This does NOT seem to be general run-to-run noise that every benchmark
// in this file shows to some degree - it is specific to these two benchmarks:
// an adjacent, similarly-shaped benchmark (BM_SenderDeferral_ThreadAffinity,
// also multi-threaded, also AutoDrainThread-based) does NOT show the same
// full-suite-vs-isolated gap.
//
// Whatever's different about running earlier in a long benchmark process
// specifically affects the two AutoDrainThread + per-receiver-loop scenarios
// and not the sender-context one sitting right next to them - worth a look if
// this ever becomes relevant, but not currently being chased.
// ============================================================================

static void BM_ConcurrentEmission_PerReceiver( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;
	std::atomic< int > completions { 0 };
	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	// declaration order deliberately kept as loops, drainers, receivers - receivers
	// destructs FIRST, BEFORE drainers' documented final-drain destructor runs
	std::list< stellyra::EventLoop > loops;
	std::list< stellyra::AutoDrainThread > drainers;
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		loops.emplace_back();
		drainers.emplace_back( loops.back() );

		receivers.emplace_back();
		receivers.back().setEventLoop( &loops.back() );

		sender.fired.connect< &Receiver::onFired >( receivers.back(), stellyra::ConnectionType::Deferred );

		// completion counter: second Deferred connection on the same receiver
		// loop matches the two-connection-per-receiver pattern in the original
		sender.fired.connectLambda( receivers.back(),
			[ &completions ]( EventArg ) {
				completions.fetch_add( 1, std::memory_order_relaxed );
			},
			stellyra::ConnectionType::Deferred );
	}

	std::atomic< bool > running { true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

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
					sender.fired( PAYLOAD );
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
		spinWait( completions, totalCompletions );
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
BENCHMARK( BM_ConcurrentEmission_PerReceiver )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );

// ConcurrentEvent counterpart to BM_ConcurrentEmission_PerReceiver above -
// identical per-receiver-EventLoop, multi-emitter-thread workload; only the
// sender's event type differs (ConcurrentSender/ConcurrentEvent instead of
// Sender/Event). Rounds out the Stellyra-vs-Qt-vs-vdk receiver-deferred table
// with both of Stellyra's thread-safe event types rather than only Event.
static void BM_ConcurrentEmission_PerReceiver_ConcurrentEvent( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	ConcurrentSender sender;
	std::atomic< int > completions { 0 };
	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	// same declaration-order reasoning as BM_ConcurrentEmission_PerReceiver
	// above (receivers must destruct before drainers' final-drain dtor runs)
	std::list< stellyra::EventLoop > loops;
	std::list< stellyra::AutoDrainThread > drainers;
	std::list< Receiver > receivers;

	for ( int i = 0; i < n; ++i )
	{
		loops.emplace_back();
		drainers.emplace_back( loops.back() );

		receivers.emplace_back();
		receivers.back().setEventLoop( &loops.back() );

		sender.fired.connect< &Receiver::onFired >( receivers.back(), stellyra::ConnectionType::Deferred );

		sender.fired.connectLambda( receivers.back(),
			[ &completions ]( EventArg ) {
				completions.fetch_add( 1, std::memory_order_relaxed );
			},
			stellyra::ConnectionType::Deferred );
	}

	std::atomic< bool > running { true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

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
					sender.fired( PAYLOAD );
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
		spinWait( completions, totalCompletions );
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
BENCHMARK( BM_ConcurrentEmission_PerReceiver_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );

// ============================================================================
// Scenario 7b (Receiver Deferral) - concurrent emission, serialised dispatch
//
// Structurally identical to BM_ConcurrentEmission_PerReceiver above, with
// one difference: all n receivers here share a SINGLE EventLoop and
// AutoDrainThread, instead of each getting its own.  Same n receivers, same
// 2 Deferred connections per receiver, same emitter thread pattern, same
// total post count - the only thing that changes is whether those posts
// land in n independent queues drained in parallel (PerReceiver) or one
// shared queue drained serially by a single thread (this benchmark).  That
// isolates the cost of sharing a loop across receivers from every other
// variable, and is the reason this exists as its own scenario rather than
// just being PerReceiver with n=1: at n=1 the two are identical by
// construction (one receiver either way) - the comparison only means
// something once n>1, run both at the same n and compare directly.
//
// RECEIVER-side ConnectionType::Deferred against receiver.setEventLoop(),
// same as BM_ConcurrentEmission_PerReceiver above - not sender's
// operator() loop check.  Its fair Qt comparison is scenario 7a's pattern
// (per-receiver Qt::QueuedConnection) at the same n, not
// BM_ConcurrentEmission_Serialised_SignalForwarding / _InvokeMethod in
// bench_qt.cpp, which force dispatch onto the sender's own thread the same
// way BM_SenderDeferral_ThreadAffinity does above - that scenario is the
// sender-deferral counterpart to this one, now parameterized by n the same
// way this scenario is.
// ============================================================================

static void BM_ReceiverDeferral_Serialised( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	Sender sender;

	// declaration order deliberately kept as sharedLoop, drainer,
	// receivers - receivers destructs FIRST, BEFORE drainer's
	// documented final-drain destructor runs
	stellyra::EventLoop sharedLoop;
	stellyra::AutoDrainThread drainer( sharedLoop );  // the ONE drain thread every receiver below funnels through

	std::list< Receiver > receivers;

	std::atomic< int > completions { 0 };
	const int totalCompletions = CROSS_THREAD_EMISSIONS * CONCURRENT_THREAD_COUNT * n;

	for ( int i = 0; i < n; ++i )
	{
		receivers.emplace_back();
		receivers.back().setEventLoop( &sharedLoop );  // same loop for every receiver - the whole point of this scenario

		sender.fired.connect< &Receiver::onFired >( receivers.back(), 0, stellyra::ConnectionType::Deferred );

		// completion counter: second Deferred connection per receiver, matching
		// BM_ConcurrentEmission_PerReceiver's two-connection-per-receiver pattern above
		sender.fired.connectLambda( receivers.back(),
			[ &completions ]( EventArg ) {
				completions.fetch_add( 1, std::memory_order_relaxed );
			},
			stellyra::ConnectionType::Deferred, 0 );
	}

	std::atomic< bool > running { true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

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
					sender.fired( PAYLOAD );
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
		spinWait( completions, totalCompletions );
	}

	running.store( false, std::memory_order_relaxed );
	startBarrier.arrive();
	for ( auto& t : emitters )
	{
		t.join();
	}

	state.SetItemsProcessed( static_cast< int64_t >( state.iterations() ) * totalCompletions );
}
BENCHMARK( BM_ReceiverDeferral_Serialised )
	->Arg( 1 )
	->Arg( 5 )
	->MinTime( 1.5 );

// ============================================================================
// Scenario 7a (execute-now) - concurrent Direct dispatch
//
// Two Stellyra event types, each with a Direct-dispatch benchmark: Event
// (RecursiveMutex, fully serialised - the genuine entry in the "execute-now,
// serialised" table) and ConcurrentEvent (lock-free, epoch-protected snapshot
// read, inherently parallel-capable - the genuine entry in the "execute-now,
// parallel" table).  Each also appears as the reference point in the other's
// table, since neither is a competing implementation of the other's guarantee.
// The ConcurrentEvent benchmarks reuse the identical thread/barrier/workload
// shape as their Event counterparts, swapping only the event type.
// ============================================================================

/// Receiver with its OWN per-instance atomic state, used only by
/// BM_ConcurrentEmission_Direct_Serialized/_Parallel (and their
/// _ConcurrentEvent siblings) and BM_ContentionScaling_* below - isolates
/// real concurrent dispatch cost from the cache-line contention that a
/// single shared global_sink written by every receiver on every thread
/// would otherwise introduce. Every other benchmark in this file continues
/// to use the ordinary Receiver (shared global_sink) for continuity with
/// prior numbers.
struct LocalSinkReceiver : stellyra::Trackable
{
	std::atomic< EventArg > localSink{ 0 };
	void onFired( EventArg v ) { localSink.store( v, std::memory_order_relaxed ); }
};

// Event (RecursiveMutex) is Stellyra's primary/default event type (see
// event.h).  Its operator() holds the RecursiveMutex for the whole
// handler-invocation loop, so this is genuinely serialised the way the
// table it feeds into describes - the fair comparison point against
// nano/sigslot's internally-serialised policies and rocket/nod/Qt/vdk's
// externally-mutexed variants.
static void BM_ConcurrentEmission_Direct_Serialized( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	stellyra::Event< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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

// ConcurrentEvent counterpart to BM_ConcurrentEmission_Direct_Serialized
// above.  Its dispatch() is a lock-free, epoch-protected read of an
// immutable snapshot, so it is inherently parallel-capable and not
// genuinely "serialised" the way the Event-backed benchmark above is -
// included as the copy-on-write reference point (Stellyra Extras).  Identical
// thread/barrier/workload shape to BM_ConcurrentEmission_Direct_Serialized,
// only the event type differs.
static void BM_ConcurrentEmission_Direct_Serialized_ConcurrentEvent( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	stellyra::ConcurrentEvent< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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
BENCHMARK( BM_ConcurrentEmission_Direct_Serialized_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 5 );

// Event is Stellyra's primary/default event type (see event.h), but it is
// included here as a reference point rather than a genuine "parallel"
// data point: Event always serialises its dispatch (its RecursiveMutex is
// held for the whole handler loop, same as in the _Serialized benchmark
// above), so under this same multi-emitter workload it cannot actually run
// handlers concurrently the way ConcurrentEvent does.  Its number here
// should read alongside BM_ConcurrentEmission_Direct_Parallel_ConcurrentEvent
// below as "what forced serialisation costs relative to genuine lock-free
// parallel dispatch under identical thread pressure", not as a competing
// parallel implementation.
static void BM_ConcurrentEmission_Direct_Parallel( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	stellyra::Event< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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

// ConcurrentEvent counterpart to BM_ConcurrentEmission_Direct_Parallel
// above.  Its dispatch() is a lock-free, epoch-protected read of an
// immutable snapshot, so handlers genuinely run concurrently here across
// the emitter threads - the real "parallel" data point that the Event
// benchmark above is a serialised reference point against.  Identical
// thread/barrier/workload shape to BM_ConcurrentEmission_Direct_Parallel,
// only the event type differs.
static void BM_ConcurrentEmission_Direct_Parallel_ConcurrentEvent( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	stellyra::ConcurrentEvent< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( n );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( CONCURRENT_THREAD_COUNT + 1 );
	Barrier endBarrier( CONCURRENT_THREAD_COUNT + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( CONCURRENT_THREAD_COUNT );

	for ( int t = 0; t < CONCURRENT_THREAD_COUNT; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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
BENCHMARK( BM_ConcurrentEmission_Direct_Parallel_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 5 );

// ============================================================================
// Scenario 9 - Contention scaling
//
// CONTENTION_RECEIVER_COUNT receivers are fixed throughout; the number of
// threads simultaneously calling fired() on the SAME shared event is the
// only thing that varies, from 1 (no contention at all) up to 32.
// BM_ContentionScaling_Direct below uses Event (RecursiveMutex), Stellyra's
// primary/default event type (see event.h): every thread here genuinely
// contends on the same RecursiveMutex, so it's the real mutex-under-
// contention data point, directly comparable against Qt's and vdk's
// BM_ContentionScaling_Direct at the same thread counts.
//
// BM_ContentionScaling_Direct_ConcurrentEvent below is the flagship
// comparison this file's copy-on-write ConcurrentEvent conversion exists
// for (ATOMIC_DISPATCH_DESIGN.md measured vdk 4.8-5.4x faster than
// Stellyra's best mutex variant of any type tested, specifically at this
// scenario's thread counts).  dispatch() under ConcurrentEvent is a
// per-slot atomic read protected by one epoch-domain enter() per call, not
// a lock of any kind - there is no mutex-backed *ConcurrentEvent* sender
// type left to isolate here.
// ============================================================================

static void BM_ContentionScaling_Direct( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	stellyra::Event< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( CONTENTION_RECEIVER_COUNT );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( threadCount + 1 );
	Barrier endBarrier( threadCount + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( threadCount );

	for ( int t = 0; t < threadCount; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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

// ConcurrentEvent counterpart to BM_ContentionScaling_Direct above -
// identical fixed-receiver-count, scaling-thread-count workload; only the
// event type differs.  dispatch() here is lock-free (see the scenario
// description above), so this is the genuinely contention-scaling-free
// data point the Event-backed benchmark above is measured against.
static void BM_ContentionScaling_Direct_ConcurrentEvent( benchmark::State& state )
{
	const int threadCount = static_cast< int >( state.range( 0 ) );

	stellyra::ConcurrentEvent< EventArg > fired;
	std::vector< LocalSinkReceiver > receivers( CONTENTION_RECEIVER_COUNT );

	for ( auto& r : receivers )
	{
		fired.connectLambda( [ & ]( EventArg v ) { r.onFired( v ); } );
	}

	std::atomic< bool > running{ true };
	Barrier startBarrier( threadCount + 1 );
	Barrier endBarrier( threadCount + 1 );

	std::vector< std::thread > emitters;
	emitters.reserve( threadCount );

	for ( int t = 0; t < threadCount; ++t )
	{
		emitters.emplace_back( [ &fired, &startBarrier, &endBarrier, &running ]() {
			for ( ;; )
			{
				startBarrier.arrive();
				if ( ! running.load( std::memory_order_relaxed ) )
				{
					break;
				}

				for ( int i = 0; i < CROSS_THREAD_EMISSIONS; ++i )
				{
					fired( PAYLOAD );
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
BENCHMARK( BM_ContentionScaling_Direct_ConcurrentEvent )
	->Arg( 1 )
	->Arg( 2 )
	->Arg( 4 )
	->Arg( 8 )
	->Arg( 16 )
	->Arg( 32 );

#endif // STELLYRA_THREAD_SAFE != 0

BENCHMARK_MAIN();
