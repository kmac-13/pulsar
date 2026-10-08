#include "test_helpers.h"

/**
 * @file test_concurrent_event_gtest.cpp
 *
 * @brief Validation of BasicConcurrentEvent surface to match API of BasicEvent.
 *
 * The implementation (basic_concurrent_event_impl.h) uses EpochDomain
 * (epoch-based reclamation covering a whole dispatch() call, not one
 * pointer at a time) with a growable, directly-indexed slot array, and
 * stores a real Callable by value in each Entry.  The test cases below
 * exercise the public Connection-based behavior.
 *
 * Coverage: basic behavior, reentrancy (same thread, several levels deep),
 * plus the two tests targeting the two genuinely tricky concurrent mechanisms
 * in the design - retire-heavy connect/disconnect racing against active
 * readers (EpochDomain's reclamation), and the slot array growing while
 * dispatch() is concurrently active (the _usedCount/_slotsPtr read-ordering
 * rule documented in basic_concurrent_event_impl.h).
 */

#include "kmac/stellyra/concurrent_event.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// ===========================================================================
// basic behaviour - through the real Connection, not an internal handle
// ===========================================================================

TEST( ConcurrentEvent, BasicConnectTriggerDisconnect )
{
	stellyra::ConcurrentEvent< int > event;

	int sum = 0;
	stellyra::Connection conn = event.connectLambda( [ & ]( int v ) { sum += v; } );

	event( 3 );
	event.trigger( 4 );
	EXPECT_EQ( sum, 7 );
	EXPECT_TRUE( conn.isConnected() );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	event( 100 );
	EXPECT_EQ( sum, 7 );  // disconnected handler must not fire
}

TEST( ConcurrentEvent, DirectConnectionTypeAccepted )
{
	stellyra::ConcurrentEvent< int > event;
	int calls = 0;
	stellyra::Connection conn = event.connectLambda(
		[ & ]( int ) { ++calls; }, stellyra::ConnectionType::Direct );

	event( 1 );
	EXPECT_EQ( calls, 1 );
	conn.disconnect();
}

TEST( ConcurrentEvent, StaleConnectionIgnored )
{
	stellyra::ConcurrentEvent< int > event;

	int calls = 0;
	stellyra::Connection first = event.connectLambda( [ & ]( int ) { ++calls; } );
	first.disconnect();

	// reconnect - may reuse the same slot index with a bumped generation
	stellyra::Connection second = event.connectLambda( [ & ]( int ) { ++calls; } );

	// disconnecting the stale handle must not touch the new connection,
	// even if it landed in the same slot index
	first.disconnect();
	EXPECT_TRUE( second.isConnected() );

	event( 1 );
	EXPECT_EQ( calls, 1 );
}

TEST( ConcurrentEvent, BlockUnblock )
{
	stellyra::ConcurrentEvent< int > event;

	int calls = 0;
	stellyra::Connection conn = event.connectLambda( [ & ]( int ) { ++calls; } );

	conn.block();
	EXPECT_TRUE( conn.isBlocked() );
	event( 1 );
	EXPECT_EQ( calls, 0 );

	conn.unblock();
	EXPECT_FALSE( conn.isBlocked() );
	event( 1 );
	EXPECT_EQ( calls, 1 );
}

TEST( ConcurrentEvent, SingleShotBatching )
{
	stellyra::ConcurrentEvent< int > event;

	int fastFires = 0;
	int onceFires = 0;

	event.connectLambda( [ & ]( int ) { ++fastFires; } );
	for ( int i = 0; i < 50; ++i )
	{
		event.connectLambda(
			[ & ]( int ) { ++onceFires; }, stellyra::ConnectionType::Auto, event.params().once() );
	}

	event( 0 );
	EXPECT_EQ( onceFires, 50 );
	EXPECT_EQ( fastFires, 1 );

	// all 50 single-shot handlers must be retired after one trigger, in the
	// one combined disconnectHandlers() call - not one mutation per handler
	event( 0 );
	EXPECT_EQ( onceFires, 50 );
	EXPECT_EQ( fastFires, 2 );
}

TEST( ConcurrentEvent, ScopedConnectionDisconnectsOnDestruction )
{
	stellyra::ConcurrentEvent< int > event;
	int calls = 0;
	{
		stellyra::ScopedConnection guard( event.connectLambda( [ & ]( int ) { ++calls; } ) );
		event( 1 );
		EXPECT_EQ( calls, 1 );
	}

	event( 1 );
	EXPECT_EQ( calls, 1 );  // guard's destructor disconnected it
}

/**
 * @brief Confirms handler independence directly: connecting many handlers,
 * then disconnecting a few, must leave every OTHER handler's Entry intact
 * and callable - i.e. disconnectHandlers() only touched the specific slots
 * named, nothing else in the array.
 */
TEST( ConcurrentEvent, UnrelatedHandlerSurvivesDisconnect )
{
	stellyra::ConcurrentEvent< int > event;
	std::vector< int > fireCounts( 20, 0 );
	std::vector< stellyra::Connection > conns;

	for ( int i = 0; i < 20; ++i )
	{
		conns.push_back( event.connectLambda( [ &fireCounts, i ]( int ) { ++fireCounts[ i ]; } ) );
	}

	event( 0 );
	for ( int i = 0; i < 20; ++i )
	{
		EXPECT_EQ( fireCounts[ i ], 1 ) << "handler " << i;
	}

	// disconnect a handful of unrelated slots - every remaining slot's
	// Entry must have survived untouched
	conns[ 3 ].disconnect();
	conns[ 11 ].disconnect();
	conns[ 17 ].disconnect();

	event( 0 );
	for ( int i = 0; i < 20; ++i )
	{
		if ( i == 3 || i == 11 || i == 17 )
		{
			EXPECT_EQ( fireCounts[ i ], 1 ) << "handler " << i << " should not have fired again";
		}
		else
		{
			EXPECT_EQ( fireCounts[ i ], 2 ) << "handler " << i << " should have fired again";
		}
	}
}

// ===========================================================================
// reentrancy - same thread, several levels deep, before anything else
// ===========================================================================

TEST( ConcurrentEventReentrancy, ConnectDuringTrigger )
{
	stellyra::ConcurrentEvent< int > event;

	int baseFires = 0;
	int addedFires = 0;

	event.connectLambda( [ & ]( int ) {
		++baseFires;
		event.connectLambda( [ & ]( int ) { ++addedFires; } );
	} );

	event( 1 );
	EXPECT_EQ( baseFires, 1 );
	EXPECT_EQ( addedFires, 0 );

	event( 1 );
	EXPECT_EQ( baseFires, 2 );
	EXPECT_EQ( addedFires, 1 );
}

TEST( ConcurrentEventReentrancy, SelfDisconnectDuringTrigger )
{
	stellyra::ConcurrentEvent< int > event;

	int fires = 0;
	stellyra::Connection self;
	self = event.connectLambda( [ & ]( int ) {
		++fires;
		self.disconnect();
	} );

	event( 1 );
	event( 1 );
	event( 1 );
	EXPECT_EQ( fires, 1 );
	EXPECT_FALSE( self.isConnected() );
}

TEST( ConcurrentEventReentrancy, NestedTriggerSeveralLevelsDeep )
{
	stellyra::ConcurrentEvent< int > event;

	constexpr int MAX_DEPTH = 5;
	int callsAtDepth[ MAX_DEPTH + 1 ] = { 0 };

	event.connectLambda( [ & ]( int depth ) {
		callsAtDepth[ depth ]++;
		if ( depth < MAX_DEPTH )
		{
			event( depth + 1 );  // reentrant trigger on the same event
		}
	} );

	event( 0 );

	for ( int d = 0; d <= MAX_DEPTH; ++d )
	{
		EXPECT_EQ( callsAtDepth[ d ], 1 ) << "depth " << d;
	}
}

// ===========================================================================
// concurrent stress - properly joined threads, multi-trial, high volume
//
// These are deliberately slow and deliberately not parameterized down to
// fewer trials: a single trial passing is weak evidence for the absence of
// a race, and every trial count here matches what previously caught real
// issues during this design's development. Run under ASan/UBSan and TSan
// as two separate binaries, not combined with this file's other tests, to
// keep sanitizer overhead from making the whole suite impractically slow.
// ===========================================================================

TEST( ConcurrentEventConcurrency, ConnectDisconnectDuringTrigger )
{
	constexpr int TRIALS = 200;
	constexpr int WRITER_THREADS = 4;
	constexpr int OPS_PER_WRITER = 2000;
	constexpr int TRIGGERS_PER_TRIAL = 500;

	for ( int trial = 0; trial < TRIALS; ++trial )
	{
		stellyra::ConcurrentEvent< int > event;
		std::atomic< uint64_t > dispatchCount { 0 };

		std::vector< stellyra::Connection > longLived;
		for ( int i = 0; i < 4; ++i )
		{
			longLived.push_back( event.connectLambda( [ & ]( int ) {
				dispatchCount.fetch_add( 1, std::memory_order_relaxed );
			} ) );
		}

		std::vector< std::thread > writers;
		for ( int w = 0; w < WRITER_THREADS; ++w )
		{
			writers.emplace_back( [ & ]() {
				for ( int op = 0; op < OPS_PER_WRITER; ++op )
				{
					stellyra::Connection c = event.connectLambda( [ & ]( int ) {
						dispatchCount.fetch_add( 1, std::memory_order_relaxed );
					} );
					c.disconnect();
				}
			} );
		}

		std::thread dispatcher( [ & ]() {
			for ( int t = 0; t < TRIGGERS_PER_TRIAL; ++t )
			{
				event( t );
			}
		} );

		for ( auto& w : writers )
		{
			w.join();
		}
		dispatcher.join();

		ASSERT_GE( dispatchCount.load(), 4ull * TRIGGERS_PER_TRIAL ) << "trial " << trial;

		for ( auto& conn : longLived )
		{
			ASSERT_TRUE( conn.isConnected() ) << "trial " << trial;
		}
	}
}

/**
 * @brief Targets EpochDomain's reclamation specifically: many reader
 * threads continuously dispatch() (enter the epoch, walk every slot,
 * release) while one writer thread continuously connects and disconnects,
 * forcing a retire() - and the writer-side scan/possible epoch advance -
 * on essentially every writer operation. If enter()'s claim protocol or
 * retire()'s active-reader scan were wrong, this is where a reader would
 * end up dereferencing a freed Entry - something ASan/TSan below would
 * catch directly, not just this test's own assertions.
 */
TEST( ConcurrentEventConcurrency, ReclamationRaceUnderRetireHeavyWriter )
{
	constexpr int TRIALS = 100;
	constexpr int READER_THREADS = 8;
	constexpr int DISPATCHES_PER_READER = 5000;
	constexpr int WRITER_OPS = 10000;

	for ( int trial = 0; trial < TRIALS; ++trial )
	{
		stellyra::ConcurrentEvent< int > event;
		std::atomic< uint64_t > fires { 0 };

		// one long-lived handler every dispatch should see
		stellyra::Connection longLived = event.connectLambda( [ & ]( int ) {
			fires.fetch_add( 1, std::memory_order_relaxed );
		} );

		std::vector< std::thread > readers;
		for ( int r = 0; r < READER_THREADS; ++r )
		{
			readers.emplace_back( [ & ]() {
				for ( int i = 0; i < DISPATCHES_PER_READER; ++i )
				{
					event( i );
				}
			} );
		}

		std::thread writer( [ & ]() {
			for ( int i = 0; i < WRITER_OPS; ++i )
			{
				stellyra::Connection c = event.connectLambda( [ & ]( int ) {
					fires.fetch_add( 1, std::memory_order_relaxed );
				} );
				c.disconnect();  // guarantees a retire() on almost every iteration
			}
		} );

		for ( auto& r : readers )
		{
			r.join();
		}
		writer.join();

		ASSERT_GE( fires.load(), static_cast< uint64_t >( READER_THREADS ) * DISPATCHES_PER_READER )
			<< "trial " << trial;
		ASSERT_TRUE( longLived.isConnected() ) << "trial " << trial;
	}
}

/**
 * @brief Specifically exercises the growable slot array: forces many
 * growSlots() calls (INITIAL_CAPACITY is small - 8) while dispatch() is
 * concurrently active on other threads, directly targeting the
 * _usedCount/_slotsPtr read-ordering rule documented in basic_concurrent_event_impl.h.
 * If that ordering were wrong, a dispatching thread could observe a fresh
 * _usedCount paired with a stale, too-small _slotsPtr and walk off the end
 * of the array - this is exactly the shape of stress that should surface
 * that under ASan/TSan.
 */
TEST( ConcurrentEventConcurrency, GrowthUnderConcurrentDispatch )
{
	constexpr int TRIALS = 100;
	constexpr int TOTAL_HANDLERS = 500;  // forces ~6 doublings past INITIAL_CAPACITY=8
	constexpr int DISPATCHER_THREADS = 4;
	constexpr int DISPATCHES_PER_THREAD = 2000;

	for ( int trial = 0; trial < TRIALS; ++trial )
	{
		stellyra::ConcurrentEvent< int > event;
		std::atomic< uint64_t > fires { 0 };

		std::thread connector( [ & ]() {
			std::vector< stellyra::Connection > conns;
			for ( int i = 0; i < TOTAL_HANDLERS; ++i )
			{
				conns.push_back( event.connectLambda( [ & ]( int ) {
					fires.fetch_add( 1, std::memory_order_relaxed );
				} ) );
			}
		} );

		std::vector< std::thread > dispatchers;
		for ( int d = 0; d < DISPATCHER_THREADS; ++d )
		{
			dispatchers.emplace_back( [ & ]() {
				for ( int i = 0; i < DISPATCHES_PER_THREAD; ++i )
				{
					event( i );
				}
			} );
		}

		connector.join();
		for ( auto& t : dispatchers )
		{
			t.join();
		}

		// final sanity dispatch: every one of the TOTAL_HANDLERS should
		// still fire exactly once here, proving the array holds all of
		// them correctly after however many growths happened
		uint64_t before = fires.load();
		event( 0 );
		ASSERT_EQ( fires.load() - before, static_cast< uint64_t >( TOTAL_HANDLERS ) )
			<< "trial " << trial;
	}
}

// ===========================================================================
// concurrent dispatch correctness at volume - this test exists to confirm
// that a high volume of concurrent dispatches across many threads and
// handlers accounts for every expected invocation, with nothing lost or
// double-counted.
// ===========================================================================

TEST( ConcurrentEventConcurrency, HighVolumeDispatchAccountsForEveryInvocation )
{
	constexpr int THREADS = 8;
	constexpr int TRIGGERS_PER_THREAD = 200000;

	stellyra::ConcurrentEvent< int > event;
	std::atomic< uint64_t > total { 0 };

	std::vector< stellyra::Connection > handlers;
	for ( int i = 0; i < 8; ++i )
	{
		handlers.push_back( event.connectLambda( [ & ]( int v ) {
			total.fetch_add( static_cast< uint64_t >( v ), std::memory_order_relaxed );
		} ) );
	}

	std::vector< std::thread > threads;
	for ( int t = 0; t < THREADS; ++t )
	{
		threads.emplace_back( [ & ]() {
			for ( int i = 0; i < TRIGGERS_PER_THREAD; ++i )
			{
				event( 1 );
			}
		} );
	}
	for ( auto& th : threads )
	{
		th.join();
	}

	EXPECT_EQ( total.load(), 8ull * THREADS * TRIGGERS_PER_THREAD );
}
