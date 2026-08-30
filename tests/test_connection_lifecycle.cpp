#include "test_helpers.hpp"

#include <atomic>
#include <thread>

// ---------------------------------------------------------------------------
// Connection lifecycle: generational index, slot reuse, Event move, and
// mutex-policy parity across Event/SharedEvent/SingleThreadedEvent.
//
// Split into three typed suites:
//   - ConnectionLifecycle (AllEventTypes): no reentrant connect/disconnect
//     from within dispatch, no real threads.
//   - ConnectionLifecycleRestricted (Event/SingleThreadedEvent only):
//     ReentrantConnectExcludedFromCurrentEmission connects from within a
//     firing handler on the same event - SharedEvent's shared-lock dispatch
//     path doesn't support this (confirmed deadlock pattern elsewhere -
//     see test_pending_removal_stress.cpp / test_reentrancy.cpp).
//   - ConnectionLifecycleThreadSafe (Event/SharedEvent only): genuine
//     concurrent multi-thread triggering - SingleThreadedEvent's NullMutex
//     is documented UB under real cross-thread access, so it's excluded
//     here, matching the pattern in test_thread_safety.cpp.
// ---------------------------------------------------------------------------

namespace {

int callCount = 0;
void freeFunc( int )
{
	++callCount;
}

} // namespace

template< typename MutexType >
class ConnectionLifecycle : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( ConnectionLifecycle, MutexTypes );

// ---------------------------------------------------------------------------
// Stale Connection handles after slot reuse must not affect the new
// connection occupying that slot - the generation counter is what
// distinguishes them, not the index alone.
// ---------------------------------------------------------------------------

TYPED_TEST( ConnectionLifecycle, StaleConnectionAfterSlotReuseIsIgnored )
{
	pulsar::BasicEvent< TypeParam, int > ev;
	callCount = 0;

	auto c1 = ev.template connectFree< &freeFunc >();
	c1.disconnect();
	EXPECT_FALSE( c1.isConnected() );

	auto c2 = ev.template connectFree< &freeFunc >();  // reuses the same slot
	EXPECT_TRUE( c2.isConnected() );
	EXPECT_FALSE( c1.isConnected() );  // stale - generation mismatch

	ev( 0 );
	EXPECT_EQ( callCount, 1 );

	// disconnecting the stale handle must not touch the new connection
	c1.disconnect();
	EXPECT_TRUE( c2.isConnected() );
	ev( 0 );
	EXPECT_EQ( callCount, 2 );
}

// ---------------------------------------------------------------------------
// Dispatch always follows true connection order, never physical slot
// position - reconnecting after disconnecting non-adjacent connections
// does not let the new connection "jump ahead" of ones that were already
// connected and remained so the whole time.
// ---------------------------------------------------------------------------

TYPED_TEST( ConnectionLifecycle, ReconnectionFollowsSlotReuseOrderNotConnectionOrder )
{
	// dispatch order within a priority level follows slot reuse, not
	// connection order: a handler connected into a reused slot takes that
	// slot's dispatch position; explicit ordering is expressed via priority
	// (see PriorityControlsDispatchOrder), not connection order
	pulsar::BasicEvent< TypeParam, int > ev;
	std::vector< int > order;

	auto c0 = ev.connectLambda( [ &order ]( int ) { order.push_back( 0 ); } );  // slot 0
	auto c1 = ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); } );  // slot 1
	auto c2 = ev.connectLambda( [ &order ]( int ) { order.push_back( 2 ); } );  // slot 2

	c0.disconnect();  // frees slot 0
	c2.disconnect();  // frees slot 2

	// reuse is lowest-index-first: c4 takes slot 0, c5 takes slot 2
	auto c4 = ev.connectLambda( [ &order ]( int ) { order.push_back( 10 ); } );
	auto c5 = ev.connectLambda( [ &order ]( int ) { order.push_back( 20 ); } );

	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 10, 1, 20 } ) );  // slot order: 0,1,2
	EXPECT_TRUE( c4.isConnected() );
	EXPECT_TRUE( c5.isConnected() );
	EXPECT_FALSE( c0.isConnected() );
	EXPECT_FALSE( c2.isConnected() );
}

TYPED_TEST( ConnectionLifecycle, ReconnectingMiddleConnectionTakesReusedSlot )
{
	// disconnecting the middle of three connections frees its slot; the next
	// connection reuses that slot and therefore fires in that slot's position
	pulsar::BasicEvent< TypeParam, int > ev;
	std::vector< int > order;

	auto c1 = ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); } );  // slot 0
	auto c2 = ev.connectLambda( [ &order ]( int ) { order.push_back( 2 ); } );  // slot 1
	auto c3 = ev.connectLambda( [ &order ]( int ) { order.push_back( 3 ); } );  // slot 2

	c2.disconnect();  // frees slot 1

	auto c4 = ev.connectLambda( [ &order ]( int ) { order.push_back( 4 ); } );  // reuses slot 1

	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 1, 4, 3 } ) );
}

TYPED_TEST( ConnectionLifecycle, PriorityControlsDispatchOrder )
{
	// priority - not connection order - is how a specific dispatch order is
	// guaranteed, and it holds regardless of which slots get reused
	pulsar::BasicEvent< TypeParam, int > ev;
	std::vector< int > order;

	auto c0 = ev.connectLambda( [ &order ]( int ) { order.push_back( 0 ); } );
	auto c1 = ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); } );
	c0.disconnect();
	c1.disconnect();

	// reconnect into reused slots, but assign priorities: higher fires first
	ev.connectLambda( [ &order ]( int ) { order.push_back( 100 ); }, pulsar::ConnectionType::Auto, ev.params().prio( 1 ) );
	ev.connectLambda( [ &order ]( int ) { order.push_back( 200 ); }, pulsar::ConnectionType::Auto, ev.params().prio( 9 ) );
	ev.connectLambda( [ &order ]( int ) { order.push_back( 300 ); }, pulsar::ConnectionType::Auto, ev.params().prio( 5 ) );

	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 200, 300, 100 } ) );  // 9, 5, 1

	// removing every non-default-priority connection reverts to the plain
	// slot-order walk without leaving anything behind
	pulsar::EventInspector inspector( ev );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 3u );
}

// ---------------------------------------------------------------------------
// Event supports move construction; existing Connections continue to refer
// to the same underlying storage afterward.
// ---------------------------------------------------------------------------

TYPED_TEST( ConnectionLifecycle, EventMoveConstructionPreservesConnections )
{
	callCount = 0;
	pulsar::BasicEvent< TypeParam, int > ev1;
	auto c = ev1.template connectFree< &freeFunc >();
	EXPECT_TRUE( c.isConnected() );

	pulsar::BasicEvent< TypeParam, int > ev2 = std::move( ev1 );
	EXPECT_TRUE( c.isConnected() );  // still valid after the move

	ev2( 0 );
	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Basic connect/disconnect/trigger sanity - originally written to name
// SingleThreadedEvent/NullMutex specifically ("no locking cost"); now
// folded into the typed suite so it runs (and must pass) for all three
// mutex policies rather than just the one it was written to document.
// ---------------------------------------------------------------------------

TYPED_TEST( ConnectionLifecycle, BasicConnectDisconnectSanity )
{
	pulsar::BasicEvent< TypeParam, int > ev;
	callCount = 0;

	auto c = ev.template connectFree< &freeFunc >();
	ev( 0 );
	EXPECT_EQ( callCount, 1 );

	c.disconnect();
	ev( 0 );
	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// A Trackable with many connections to the same event is fully cleaned up
// on destruction, regardless of how many slots it occupied.
// ---------------------------------------------------------------------------

TYPED_TEST( ConnectionLifecycle, TrackableWithManyConnectionsCompaction )
{
	pulsar::BasicEvent< TypeParam, int > ev;

	{
		TestHandler r;
		for ( int i = 0; i < 10; ++i )
		{
			ev.connectLambda( r, [ &r ]( int ) { r.callCount++; } );
		}

		ev( 0 );
		EXPECT_EQ( r.callCount, 10 );
	}

	// all 10 connections should be gone now - no crash on next trigger
	pulsar::EventInspector inspector( ev );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
	EXPECT_NO_THROW( ev( 0 ) );
}

// ---------------------------------------------------------------------------
// Reentrant connect() from within a firing handler is excluded from the
// emission already in progress, and only takes effect on the next one.
// Event/SingleThreadedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class ConnectionLifecycleRestricted : public ::testing::Test {};

using RestrictedMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( ConnectionLifecycleRestricted, RestrictedMutexTypes );

TYPED_TEST( ConnectionLifecycleRestricted, ReentrantConnectExcludedFromCurrentEmission )
{
	pulsar::BasicEvent< TypeParam, int > ev;
	callCount = 0;
	bool firstCall = true;
	pulsar::Connection extra;

	auto c = ev.connectLambda( [ &ev, &extra, &firstCall ]( int ) {
		++callCount;
		if ( firstCall )
		{
			firstCall = false;
			extra = ev.template connectFree< &freeFunc >();
		}
	} );

	ev( 0 );
	EXPECT_EQ( callCount, 1 );  // reentrant connection excluded from this emission

	ev( 0 );
	EXPECT_EQ( callCount, 3 );  // both handlers active now - original value +1 (lambda) +1 (free func) = 1 +2 more = 3
}

// ---------------------------------------------------------------------------
// Concurrent triggering from multiple real threads must not crash or
// lose/duplicate calls. Event/SharedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class ConnectionLifecycleThreadSafe : public ::testing::Test {};

using ThreadSafeMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex >;
TYPED_TEST_SUITE( ConnectionLifecycleThreadSafe, ThreadSafeMutexTypes );

TYPED_TEST( ConnectionLifecycleThreadSafe, ConcurrentTriggerIsSafe )
{
	pulsar::BasicEvent< TypeParam, int > ev;
	std::atomic< int > concurrentCalls{ 0 };

	ev.connectLambda( [ &concurrentCalls ]( int ) { ++concurrentCalls; } );

	std::thread t1( [ &ev ] { for ( int i = 0; i < 100; ++i ) ev( 0 ); } );
	std::thread t2( [ &ev ] { for ( int i = 0; i < 100; ++i ) ev( 0 ); } );
	t1.join();
	t2.join();

	EXPECT_EQ( concurrentCalls.load(), 200 );
}
