#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>

#include <atomic>
#include <sstream>
#include <thread>

// ---------------------------------------------------------------------------
// PrivateEvent
// ---------------------------------------------------------------------------

class SecureButton : public pulsar::Trackable
{
public:
	pulsar::PEvent< SecureButton, int, int > clicked{ this };

	void click( int x, int y )
	{
		clicked( x, y );  // only SecureButton can trigger
	}
};

TEST( PrivateEvent, ExternalCodeCanConnect )
{
	auto button = std::make_shared< SecureButton >();
	auto handler = std::make_shared< TestHandler >();

	// external code can connect
	button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 5, 10 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 5 );
	EXPECT_EQ( handler->lastY, 10 );
}

TEST( PrivateEvent, ExternalCodeCanDisconnect )
{
	auto button = std::make_shared< SecureButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( PrivateEvent, AutoDisconnectOnReceiverDestruction )
{
	auto button = std::make_shared< SecureButton >();

	{
		auto handler = std::make_shared< TestHandler >();
		button->clicked.connect( *handler, &TestHandler::onClicked );
		button->click( 1, 1 );
	}

	EXPECT_NO_THROW( button->click( 2, 2 ) );
}

TEST( PrivateEvent, InspectorAccess )
{
	auto button = std::make_shared< SecureButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );

	// EventInspector can inspect a PrivateEvent
	auto inspector = pulsar::EventInspector( button->clicked );
	auto info = inspector.getEventInfo();
	EXPECT_EQ( info.connectionCount, 1u );
	EXPECT_EQ( info.activeConnectionCount, 1u );
}

// ---------------------------------------------------------------------------
// EventInspector - deeper coverage
// ---------------------------------------------------------------------------

TEST( EventInspector, DumpConnectionsToString )
{
	// the receiver's concrete type/address isn't tracked at all (only
	// whether the handler is bound to an owner, via HandlerEntry's flags),
	// so the dump only ever shows the generic "method"/"function/lambda"
	// classification, never the real class name - unlike the sender,
	// which is demangled from the actual Trackable* it was constructed with
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( *handler, &TestHandler::onClicked );

	auto inspector = pulsar::EventInspector( button->clicked );
	auto str = inspector.dumpConnectionsToString();

	EXPECT_NE( str.find( "method" ), std::string::npos );
	EXPECT_GT( str.size(), 0u );
}

TEST( EventInspector, DumpConnectionGraph )
{
	// sender is demangled from the real Trackable*; receiver is only ever
	// labeled [method]/[function/lambda] - see DumpConnectionsToString above
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( *handler, &TestHandler::onClicked );

	auto inspector = pulsar::EventInspector( button->clicked );
	std::ostringstream oss;
	inspector.dumpConnectionGraph( oss );

	EXPECT_NE( oss.str().find( "TestButton" ), std::string::npos );
	EXPECT_NE( oss.str().find( "[method]" ), std::string::npos );
}

TEST( EventInspector, GetSummary )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( *handler, &TestHandler::onClicked );

	auto inspector = pulsar::EventInspector( button->clicked );
	auto summary = inspector.getSummary();

	EXPECT_NE( summary.find( "Total" ),  std::string::npos );
	EXPECT_NE( summary.find( "Active" ), std::string::npos );
}

TEST( EventInspector, ToDotString )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( *handler, &TestHandler::onClicked );
	auto blockedConn = button->clicked.connect( *handler, &TestHandler::onClicked, pulsar::ConnectionType::Deferred );

	button->clicked.connectFree( []( int, int ) {} );

	blockedConn.block();

	auto inspector = pulsar::EventInspector( button->clicked );
	auto dot = inspector.toDotString();

	EXPECT_NE( dot.find( "digraph PulsarConnections" ), std::string::npos );
	EXPECT_NE( dot.find( "rankdir=LR" ), std::string::npos );
	EXPECT_NE( dot.find( "TestButton" ), std::string::npos );
	EXPECT_NE( dot.find( "method" ), std::string::npos );  // method node label, not the real receiver type
	EXPECT_NE( dot.find( "function/lambda" ), std::string::npos );
	EXPECT_NE( dot.find( "BLOCKED" ), std::string::npos );

	// the free-function node gets a distinct diamond shape, method nodes
	// don't - verifies the two are genuinely distinguished, not just that
	// both labels happen to appear somewhere in the output
	EXPECT_NE( dot.find( "function/lambda\\nslot 2\", shape=diamond" ), std::string::npos );
}

TEST( EventInspector, ConnectionInfoDetails )
{
	// receiverTypeName is only ever the generic "<method>"/"<function/lambda>"
	// placeholder - see DumpConnectionsToString above
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked, 42 );

	auto inspector = pulsar::EventInspector( button->clicked );
	auto infos = inspector.getConnectionInfo();

	ASSERT_EQ( infos.size(), 1u );
	EXPECT_EQ( infos[ 0 ].priority, 42 );
	EXPECT_TRUE( infos[ 0 ].isConnected );
	EXPECT_FALSE( infos[ 0 ].isBlocked );
	EXPECT_EQ( infos[ 0 ].receiverTypeName, "<method>" );

	conn.block();
	infos = inspector.getConnectionInfo();
	ASSERT_EQ( infos.size(), 1u );
	EXPECT_TRUE( infos[ 0 ].isBlocked );
}

// ---------------------------------------------------------------------------
// ConnectionGuard
// ---------------------------------------------------------------------------

TEST( ConnectionGuard, BasicUsage )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( *handler, &TestHandler::onClicked ) );

	EXPECT_TRUE( guard.isConnected() );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	guard.disconnect();
	EXPECT_FALSE( guard.isConnected() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( ConnectionGuard, BlockAndUnblock )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( *handler, &TestHandler::onClicked ) );

	guard.block();
	EXPECT_TRUE( guard.isBlocked() );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 0 );

	guard.unblock();
	EXPECT_FALSE( guard.isBlocked() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( ConnectionGuard, DisconnectThenDestroy )
{
	// verify the guard disconnects the underlying connection when asked,
	// and does not double-disconnect when it goes out of scope
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	{
		pulsar::ConnectionGuard guard( button->clicked.connect( *handler, &TestHandler::onClicked ) );

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );

		guard.disconnect();
		// guard goes out of scope here - should not crash
	}

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// ConnectionGuard - move construction and move assignment
// ---------------------------------------------------------------------------

TEST( ConnectionGuard, MoveConstructionTransfersConnection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard1( button->clicked.connect( *handler, &TestHandler::onClicked ) );
	EXPECT_TRUE( guard1.isConnected() );

	pulsar::ConnectionGuard guard2( std::move( guard1 ) );

	// the connection is now owned by guard2
	EXPECT_TRUE( guard2.isConnected() );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	// guard1 is left wrapping a moved-from (empty) Connection - safe to call
	// every method on it, all reporting "not connected" rather than crashing
	EXPECT_FALSE( guard1.isConnected() );
	EXPECT_FALSE( guard1.isBlocked() );
	EXPECT_NO_THROW( guard1.block() );
	EXPECT_NO_THROW( guard1.unblock() );
	EXPECT_NO_THROW( guard1.disconnect() );

	// none of those moved-from-guard calls touched guard2's connection
	EXPECT_TRUE( guard2.isConnected() );
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 2 );
}

TEST( ConnectionGuard, MoveAssignmentReplacesWrappedConnectionWithoutDisconnectingOld )
{
	// unlike ConnectionGroup::operator= (which explicitly disconnects its
	// current contents first - see ConnectionGroup::MoveAssignment... below),
	// ConnectionGuard::operator= simply overwrites _conn with the moved-from
	// other._conn; the previously-wrapped connection is never disconnected by
	// this call - it just stops being reachable through this guard and keeps
	// running as an ordinary live connection on its event.
	auto button = std::make_shared< TestButton >();
	auto oldHandler = std::make_shared< TestHandler >();
	auto newHandler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( *oldHandler, &TestHandler::onClicked ) );
	pulsar::ConnectionGuard incoming( button->clicked.connect( *newHandler, &TestHandler::onClicked ) );

	button->click( 1, 1 );
	EXPECT_EQ( oldHandler->callCount, 1 );
	EXPECT_EQ( newHandler->callCount, 1 );

	guard = std::move( incoming );

	EXPECT_TRUE( guard.isConnected() );
	EXPECT_FALSE( incoming.isConnected() );  // moved-from

	button->click( 2, 2 );
	// oldHandler's connection is still live (never explicitly disconnected) -
	// only guard's own reference switched to point at newHandler's connection
	EXPECT_EQ( oldHandler->callCount, 2 );
	EXPECT_EQ( newHandler->callCount, 2 );

	// guard now controls newHandler's connection
	guard.disconnect();
	button->click( 3, 3 );
	EXPECT_EQ( oldHandler->callCount, 3 );  // unaffected - separate connection
	EXPECT_EQ( newHandler->callCount, 2 );  // disconnected via guard
}

TEST( ConnectionGuard, SelfMoveAssignmentIsNoOp )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( *handler, &TestHandler::onClicked ) );

	// obscure the self-assignment behind a pointer so the compiler can't
	// diagnose (or elide) it as a no-op at compile time - this exercises the
	// `if ( this != &other )` guard in operator= at runtime
	pulsar::ConnectionGuard* self = &guard;
	EXPECT_NO_THROW( guard = std::move( *self ) );

	// must still be usable and still own its connection afterward
	EXPECT_TRUE( guard.isConnected() );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// ConnectionGuard - the actual point of the class: concurrent access and
// concurrent move must be serialised correctly by the internal mutex, with
// no crash, no lost update, and (per the lock-lower-address-first ordering
// in operator=) no deadlock even when two guards are cross-assigned from
// each other on two different threads at once.
// ---------------------------------------------------------------------------

TEST( ConnectionGuardConcurrency, ConcurrentAccessDuringMoveIsSafe )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( *handler, &TestHandler::onClicked ) );

	std::atomic< bool > running{ true };
	std::atomic< int > moveCount{ 0 };

	// thread 1: repeatedly move fresh connections into guard
	std::thread mover( [ & ]() {
		while ( running )
		{
			pulsar::ConnectionGuard fresh( button->clicked.connect( *handler, &TestHandler::onClicked ) );
			guard = std::move( fresh );
			moveCount.fetch_add( 1, std::memory_order_relaxed );
		}
	} );

	// thread 2: repeatedly read/mutate guard's state concurrently
	std::thread accessor( [ & ]() {
		while ( running )
		{
			guard.isConnected();
			guard.isBlocked();
			guard.block();
			guard.unblock();
		}
	} );

	msleep( 100 );
	running = false;
	mover.join();
	accessor.join();

	// no crash, no torn state - guard ends up wrapping some valid connection
	EXPECT_GT( moveCount.load(), 0 );
	EXPECT_NO_THROW( guard.disconnect() );
}

TEST( ConnectionGuardConcurrency, CrossAssignmentDoesNotDeadlock )
{
	// two guards move-assigned FROM each other concurrently on two threads -
	// this is exactly the scenario operator='s lock-lower-address-first
	// ordering exists to make safe; a naive "always lock this then other"
	// implementation would deadlock here under contention
	auto button = std::make_shared< TestButton >();
	auto h1 = std::make_shared< TestHandler >();
	auto h2 = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard g1( button->clicked.connect( *h1, &TestHandler::onClicked ) );
	pulsar::ConnectionGuard g2( button->clicked.connect( *h2, &TestHandler::onClicked ) );

	std::atomic< bool > running{ true };
	std::atomic< int > iters1{ 0 };
	std::atomic< int > iters2{ 0 };

	std::thread t1( [ & ]() {
		while ( running )
		{
			g1 = std::move( g2 );
			iters1.fetch_add( 1, std::memory_order_relaxed );
		}
	} );
	std::thread t2( [ & ]() {
		while ( running )
		{
			g2 = std::move( g1 );
			iters2.fetch_add( 1, std::memory_order_relaxed );
		}
	} );

	msleep( 100 );
	running = false;
	t1.join();
	t2.join();

	// reaching here at all (rather than hanging) is the real assertion;
	// these confirm both threads actually made forward progress throughout
	EXPECT_GT( iters1.load(), 0 );
	EXPECT_GT( iters2.load(), 0 );
}

// ---------------------------------------------------------------------------
// ConnectionGroup - gaps in existing coverage
// ---------------------------------------------------------------------------

TEST( ConnectionGroup, CleanupRemovesDeadConnections )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );

	EXPECT_EQ( group.size(), 3u );

	// manually disconnect one through the connection handle
	group[ 1 ].disconnect();
	EXPECT_EQ( group.activeCount(), 2u );
	EXPECT_EQ( group.size(), 3u );  // still 3 entries, one is dead

	group.cleanup();
	EXPECT_EQ( group.size(), 2u );  // dead entry removed
	EXPECT_EQ( group.activeCount(), 2u );
}

TEST( ConnectionGroup, HasActiveConnections )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	EXPECT_FALSE( group.hasActiveConnections() );

	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	EXPECT_TRUE( group.hasActiveConnections() );

	group.disconnectAll();
	EXPECT_FALSE( group.hasActiveConnections() );
}

TEST( ConnectionGroup, Release )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );

	EXPECT_EQ( group.size(), 2u );

	// release clears the group without disconnecting
	group.release();
	EXPECT_EQ( group.size(), 0u );

	// connections are still alive
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );
}

TEST( ConnectionGroup, RangeFor )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );

	int count = 0;
	for ( auto& conn : group )
	{
		EXPECT_TRUE( conn.isConnected() );
		count++;
	}
	EXPECT_EQ( count, 3 );
}

TEST( ConnectionGroup, IndexedAccess )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );

	EXPECT_TRUE( group[ 0 ].isConnected() );
	EXPECT_TRUE( group[ 1 ].isConnected() );

	group[ 0 ].disconnect();
	EXPECT_FALSE( group[ 0 ].isConnected() );
	EXPECT_TRUE(  group[ 1 ].isConnected() );
}

// ---------------------------------------------------------------------------
// ConnectionGroup - move construction and move assignment
// ---------------------------------------------------------------------------

TEST( ConnectionGroup, MoveConstructionTransfersWithoutDisconnecting )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group1;
	group1 += button->clicked.connect( *handler, &TestHandler::onClicked );
	group1 += button->clicked.connect( *handler, &TestHandler::onClicked );
	group1 += button->clicked.connect( *handler, &TestHandler::onClicked );

	pulsar::ConnectionGroup group2( std::move( group1 ) );

	// all 3 connections transferred, still active, still fire
	EXPECT_EQ( group2.size(), 3u );
	EXPECT_EQ( group2.activeCount(), 3u );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 3 );

	// group1 is left empty - destroying it must not touch group2's connections
	EXPECT_EQ( group1.size(), 0u );
}

TEST( ConnectionGroup, MoveAssignmentDisconnectsCurrentThenTakesOwnership )
{
	// unlike ConnectionGuard::operator= (which just overwrites its single
	// wrapped Connection - see ConnectionGuard::MoveAssignment... above),
	// ConnectionGroup::operator= is documented to disconnect everything it
	// currently holds before taking ownership of the source's connections
	auto button = std::make_shared< TestButton >();
	auto oldHandler = std::make_shared< TestHandler >();
	auto newHandler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *oldHandler, &TestHandler::onClicked );
	group += button->clicked.connect( *oldHandler, &TestHandler::onClicked );

	pulsar::ConnectionGroup incoming;
	incoming += button->clicked.connect( *newHandler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( oldHandler->callCount, 2 );
	EXPECT_EQ( newHandler->callCount, 1 );

	group = std::move( incoming );

	// group's old connections (to oldHandler) were disconnected by the assignment
	EXPECT_EQ( group.size(), 1u );
	button->click( 2, 2 );
	EXPECT_EQ( oldHandler->callCount, 2 );  // unchanged - disconnected
	EXPECT_EQ( newHandler->callCount, 2 );  // group now owns this connection

	// incoming is left empty
	EXPECT_EQ( incoming.size(), 0u );
}

TEST( ConnectionGroup, SelfMoveAssignmentIsNoOp )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( *handler, &TestHandler::onClicked );
	group += button->clicked.connect( *handler, &TestHandler::onClicked );

	// obscure the self-assignment behind a pointer so the compiler can't
	// diagnose (or elide) it as a no-op at compile time - this exercises the
	// `if ( this != &other )` guard in operator= at runtime.  Without that
	// guard, self-move-assignment here would disconnectAll() the group's own
	// connections and then try to move-assign from the now-empty vector it
	// just destroyed - group would very plausibly still end up empty rather
	// than retaining its two connections.
	pulsar::ConnectionGroup* self = &group;
	EXPECT_NO_THROW( group = std::move( *self ) );

	// must still hold both connections, unaffected
	EXPECT_EQ( group.size(), 2u );
	EXPECT_EQ( group.activeCount(), 2u );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );
}

TEST( ConnectionGroup, MovedFromGroupIsSafeToReuse )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group1;
	group1 += button->clicked.connect( *handler, &TestHandler::onClicked );

	pulsar::ConnectionGroup group2( std::move( group1 ) );
	(void)group2;

	// group1 (moved-from) must still be a fully functional empty group -
	// safe to add to, iterate, and destroy, not just sit inert
	EXPECT_TRUE( group1.empty() );
	EXPECT_NO_THROW( group1.disconnectAll() );  // no-op on empty group

	group1 += button->clicked.connect( *handler, &TestHandler::onClicked );
	EXPECT_EQ( group1.size(), 1u );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );  // one call via group1, one via group2
}

// ---------------------------------------------------------------------------
// ScopedConnection::release()
// ---------------------------------------------------------------------------

TEST( ScopedConnection, Release )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	// release() returns void (it just stops the wrapper from disconnecting
	// on destruction) - keep our own copy of the Connection to use afterward
	pulsar::Connection conn = button->clicked.connect( *handler, &TestHandler::onClicked );

	{
		pulsar::ScopedConnection scoped( conn );
		EXPECT_TRUE( scoped.isConnected() );

		scoped.release();  // prevent auto-disconnect on scope exit
		// scoped is now detached - going out of scope should NOT disconnect
	}

	// connection should still be alive after scoped was destroyed
	EXPECT_TRUE( conn.isConnected() );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// disconnectAll() + setEventLoop()
//
// The old disconnectAndSetEventLoop() convenience method (disconnect
// everything, then migrate) is just these two Trackable calls in sequence -
// Trackable::disconnectAll() already iterates every tracked connection
// where this object is the receiver and disconnects each one.
// ---------------------------------------------------------------------------

TEST( Trackable, DisconnectAllThenSetEventLoop )
{
	pulsar::EventLoop loop1;
	pulsar::EventLoop loop2;
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &loop1 );
	receiver->setEventLoop( &loop1 );
	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	sender->sendData( 1 );
	loop1.drain();
	EXPECT_EQ( receiver->callCount, 1 );

	// disconnect everything, then migrate
	receiver->disconnectAll();
	receiver->setEventLoop( &loop2 );

	// no connections remain after fresh start
	sender->sendData( 2 );
	loop1.drain();
	loop2.drain();
	EXPECT_EQ( receiver->callCount, 1 );  // no new calls
}

// ---------------------------------------------------------------------------
// Move-only captures on Deferred connections
// ---------------------------------------------------------------------------

TEST( MoveOnlyCapture, WorksOnDeferredConnection )
{
	pulsar::EventLoop loop;
	pulsar::Trackable sender;
	pulsar::Trackable receiver;

	sender.setEventLoop( &loop );
	receiver.setEventLoop( &loop );

	pulsar::Event< int > event{ &sender };

	auto ptr = std::make_unique< int >( 42 );
	std::atomic< bool > handlerCalled{ false };

	// move-only capture into a Deferred connection
	event.connectLambda( receiver,
		[ p = std::move( ptr ), &handlerCalled ]( int ) mutable {
			handlerCalled = ( *p == 42 );
		},
		pulsar::ConnectionType::Deferred );

	event( 1 );
	loop.drain();  // drain sender deferral
	loop.drain();  // drain deferred invocation

	EXPECT_TRUE( handlerCalled );
}

// ---------------------------------------------------------------------------
// connectIfLambda with a member function condition
// ---------------------------------------------------------------------------

TEST( ConnectIfLambda, MemberFunctionCondition )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	auto validator = std::make_shared< TestValidator >();

	// condition is a member function of a separate object, passed as a
	// predicate lambda; connectIfLambda's runtime argument order is
	// (tracker, handler, predicate)
	button->clicked.connectLambda( *handler,
		[ handler ]( int x, int y ) { handler->onClicked( x, y ); },
		{ [ validator ]( int x, int ) { return validator->checkValue( x ); } } );

	button->click( 5, 5 );   // x > 0 -> fires
	EXPECT_EQ( handler->callCount, 1 );

	button->click( -1, 5 );  // x <= 0 -> filtered
	EXPECT_EQ( handler->callCount, 1 );
}
