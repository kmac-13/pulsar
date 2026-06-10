#include "test_helpers.hpp"
#include <sstream>

// ---------------------------------------------------------------------------
// PrivateEvent
// ---------------------------------------------------------------------------

class SecureButton : public pulsar::Object
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
	button->clicked.connect( handler, &TestHandler::onClicked );

	button->click( 5, 10 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 5 );
	EXPECT_EQ( handler->lastY, 10 );
}

TEST( PrivateEvent, ExternalCodeCanDisconnect )
{
	auto button = std::make_shared< SecureButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );
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
		button->clicked.connect( handler, &TestHandler::onClicked );
		button->click( 1, 1 );
	}

	EXPECT_NO_THROW( button->click( 2, 2 ) );
}

TEST( PrivateEvent, InspectorAccess )
{
	auto button = std::make_shared< SecureButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( handler, &TestHandler::onClicked );

	// EventInspector can inspect a PrivateEvent
	pulsar::EventInspector< int, int > inspector( button->clicked );
	auto info = inspector.getEventInfo();
	EXPECT_EQ( info.connectionCount, 1u );
	EXPECT_EQ( info.activeConnectionCount, 1u );
}

// ---------------------------------------------------------------------------
// EventInspector - deeper coverage
// ---------------------------------------------------------------------------

TEST( EventInspector, DumpConnectionsToString )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( handler, &TestHandler::onClicked );

	pulsar::EventInspector< int, int > inspector( button->clicked );
	auto str = inspector.dumpConnectionsToString();

	EXPECT_NE( str.find( "TestHandler" ), std::string::npos );
	EXPECT_GT( str.size(), 0u );
}

TEST( EventInspector, DumpConnectionGraph )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( handler, &TestHandler::onClicked );

	pulsar::EventInspector< int, int > inspector( button->clicked );
	std::ostringstream oss;
	inspector.dumpConnectionGraph( oss );

	EXPECT_NE( oss.str().find( "TestButton" ), std::string::npos );
	EXPECT_NE( oss.str().find( "TestHandler" ), std::string::npos );
}

TEST( EventInspector, GetSummary )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( handler, &TestHandler::onClicked );

	pulsar::EventInspector< int, int > inspector( button->clicked );
	auto summary = inspector.getSummary();

	EXPECT_NE( summary.find( "Total" ),  std::string::npos );
	EXPECT_NE( summary.find( "Active" ), std::string::npos );
}

TEST( EventInspector, ToDotString )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	button->clicked.connect( handler, &TestHandler::onClicked );

	auto conn2 = button->clicked.connect( handler, &TestHandler::onClicked, pulsar::ConnectionType::Deferred );
	conn2.block();

	button->clicked.connectFree( []( int, int ) {} );

	pulsar::EventInspector< int, int > inspector( button->clicked );
	auto dot = inspector.toDotString();

	EXPECT_NE( dot.find( "digraph PulsarConnections" ), std::string::npos );
	EXPECT_NE( dot.find( "rankdir=LR" ), std::string::npos );
	EXPECT_NE( dot.find( "TestButton" ), std::string::npos );
	EXPECT_NE( dot.find( "TestHandler" ), std::string::npos );
	EXPECT_NE( dot.find( "free function" ), std::string::npos );
	EXPECT_NE( dot.find( "BLOCKED" ), std::string::npos );
}

TEST( EventInspector, ConnectionInfoDetails )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connectWithPriority( handler, &TestHandler::onClicked, 42 );
	conn.block();

	pulsar::EventInspector< int, int > inspector( button->clicked );
	auto infos = inspector.getConnectionInfo();

	ASSERT_EQ( infos.size(), 1u );
	EXPECT_EQ( infos[ 0 ].priority, 42 );
	EXPECT_TRUE( infos[ 0 ].isBlocked );
	EXPECT_TRUE( infos[ 0 ].isConnected );
	EXPECT_EQ( infos[ 0 ].receiverTypeName, "TestHandler" );
}

// ---------------------------------------------------------------------------
// ConnectionGuard
// ---------------------------------------------------------------------------

TEST( ConnectionGuard, BasicUsage )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGuard guard( button->clicked.connect( handler, &TestHandler::onClicked ) );

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

	pulsar::ConnectionGuard guard( button->clicked.connect( handler, &TestHandler::onClicked ) );

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
		pulsar::ConnectionGuard guard( button->clicked.connect( handler, &TestHandler::onClicked ) );

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );

		guard.disconnect();
		// guard goes out of scope here - should not crash
	}

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// ConnectionGroup - gaps in existing coverage
// ---------------------------------------------------------------------------

TEST( ConnectionGroup, CleanupRemovesDeadConnections )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

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

	group += button->clicked.connect( handler, &TestHandler::onClicked );
	EXPECT_TRUE( group.hasActiveConnections() );

	group.disconnectAll();
	EXPECT_FALSE( group.hasActiveConnections() );
}

TEST( ConnectionGroup, Release )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

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
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

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
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

	EXPECT_TRUE( group[ 0 ].isConnected() );
	EXPECT_TRUE( group[ 1 ].isConnected() );

	group[ 0 ].disconnect();
	EXPECT_FALSE( group[ 0 ].isConnected() );
	EXPECT_TRUE(  group[ 1 ].isConnected() );
}

// ---------------------------------------------------------------------------
// ScopedConnection::release()
// ---------------------------------------------------------------------------

TEST( ScopedConnection, Release )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::Connection released;

	{
		auto scoped = button->clicked.connect( handler, &TestHandler::onClicked ).scoped();
		EXPECT_TRUE( scoped.isConnected() );

		released = scoped.release();  // transfer ownership out of scoped
		// scoped is now empty - going out of scope should NOT disconnect
	}

	// connection should still be alive after scoped was destroyed
	EXPECT_TRUE( released.isConnected() );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	released.disconnect();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// disconnectAndSetEventLoop()
// ---------------------------------------------------------------------------

TEST( Object, DisconnectAndSetEventLoop )
{
	auto loop1 = pulsar::EventLoop::makeManualProcessed();
	auto loop2 = pulsar::EventLoop::makeManualProcessed();
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &loop1 );
	receiver->setEventLoop( &loop1 );
	sender->dataReady.connect( receiver, &DataReceiver::processData );

	sender->sendData( 1 );
	loop1.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );

	// disconnectAndSetEventLoop: disconnects all connections, then migrates
	receiver->disconnectAndSetEventLoop( &loop2 );

	// no connections remain after fresh start
	sender->sendData( 2 );
	loop1.processEvents();
	loop2.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );  // no new calls
}

// ---------------------------------------------------------------------------
// Move-only captures on Deferred connections (fix 1.2)
// ---------------------------------------------------------------------------

TEST( MoveOnlyCapture, WorksOnDeferredConnection )
{
	auto loop = pulsar::EventLoop::makeManualProcessed();
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	sender->setEventLoop( &loop );
	receiver->setEventLoop( &loop );

	pulsar::Event< int > event{ sender.get() };

	auto ptr = std::make_unique< int >( 42 );
	std::atomic< bool > handlerCalled{ false };

	// move-only capture into a Deferred connection
	event.connect( receiver,
		[ p = std::move( ptr ), &handlerCalled ]( int ) mutable {
			handlerCalled = ( *p == 42 );
		},
		pulsar::ConnectionType::Deferred );

	event( 1 );
	loop.processEvents();  // drain sender deferral
	loop.processEvents();  // drain deferred invocation

	EXPECT_TRUE( handlerCalled );
}

// ---------------------------------------------------------------------------
// connectIf with member function condition
// ---------------------------------------------------------------------------

TEST( AdvancedConnections, ConnectIfMemberFunctionCondition )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	auto validator = std::make_shared< TestValidator >();

	// condition is a member function of a separate object
	button->clicked.connectIf( handler, &TestHandler::onClicked,
		[ validator ]( int x, int ) { return validator->checkValue( x ); } );

	button->click( 5, 5 );   // x > 0 -> fires
	EXPECT_EQ( handler->callCount, 1 );

	button->click( -1, 5 );  // x <= 0 -> filtered
	EXPECT_EQ( handler->callCount, 1 );
}
