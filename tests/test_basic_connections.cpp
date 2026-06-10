#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Basic Connections
// ---------------------------------------------------------------------------

TEST( BasicConnections, ConnectionWithoutHandle )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( handler, &TestHandler::onClicked );

	button->click( 10, 20 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 10 );
	EXPECT_EQ( handler->lastY, 20 );

	button->click( 30, 40 );
	EXPECT_EQ( handler->callCount, 2 );
	EXPECT_EQ( handler->lastX, 30 );
	EXPECT_EQ( handler->lastY, 40 );

	button->clicked.disconnect( handler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );

	handler->reset();
	button->click( 50, 60 );
	EXPECT_EQ( handler->callCount, 0 );
	EXPECT_EQ( handler->lastX, 0 );
	EXPECT_EQ( handler->lastY, 0 );
}

TEST( BasicConnections, ConnectionWithHandleAndDisconnect )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );
	EXPECT_TRUE( conn.isConnected() );

	button->click( 5, 15 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	handler->reset();
	button->click( 25, 35 );
	EXPECT_EQ( handler->callCount, 0 );
}

TEST( BasicConnections, MultipleConnections )
{
	auto button = std::make_shared< TestButton >();
	auto handler1 = std::make_shared< TestHandler >();
	auto handler2 = std::make_shared< TestHandler >();
	auto handler3 = std::make_shared< TestHandler >();

	button->clicked.connect( handler1, &TestHandler::onClicked );
	button->clicked.connect( handler2, &TestHandler::onClicked );
	button->clicked.connect( handler3, &TestHandler::onClicked );

	button->click( 1, 2 );

	EXPECT_EQ( handler1->callCount, 1 );
	EXPECT_EQ( handler2->callCount, 1 );
	EXPECT_EQ( handler3->callCount, 1 );
}

TEST( BasicConnections, LambdaConnectionWithoutHandle )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();

	int callCount = 0;
	int lastX = 0;
	int lastY = 0;

	button->clicked.connect( receiver, [ &callCount, &lastX, &lastY ]( int x, int y ) {
		callCount++;
		lastX = x;
		lastY = y;
	} );

	button->click( 100, 200 );
	EXPECT_EQ( callCount, 1 );
	EXPECT_EQ( lastX, 100 );
	EXPECT_EQ( lastY, 200 );
}

TEST( BasicConnections, ManualDisconnection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( BasicConnections, AutoDisconnectOnReceiverDestruction )
{
	auto button = std::make_shared< TestButton >();

	{
		auto handler = std::make_shared< TestHandler >();
		button->clicked.connect( handler, &TestHandler::onClicked );

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );

		// handler destroyed here
	}

	// must not crash
	EXPECT_NO_THROW( button->click( 2, 2 ) );
}

TEST( BasicConnections, DisconnectSpecificReceiver )
{
	auto button = std::make_shared< TestButton >();
	auto handler1 = std::make_shared< TestHandler >();
	auto handler2 = std::make_shared< TestHandler >();

	button->clicked.connect( handler1, &TestHandler::onClicked );
	button->clicked.connect( handler2, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler1->callCount, 1 );
	EXPECT_EQ( handler2->callCount, 1 );

	button->clicked.disconnect( handler1 );
	handler1->reset();
	handler2->reset();

	button->click( 2, 2 );
	EXPECT_EQ( handler1->callCount, 0 );
	EXPECT_EQ( handler2->callCount, 1 );
}

TEST( BasicConnections, LambdaAsCallback )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();
	int  result = 0;

	button->clicked.connect( receiver, [ &result ]( int x, int y ) {
		result = x + y;
	} );

	button->click( 10, 20 );
	EXPECT_EQ( result, 30 );

	button->click( 5, 7 );
	EXPECT_EQ( result, 12 );
}
