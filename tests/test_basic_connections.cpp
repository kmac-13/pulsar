#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Basic Connections
//
// No test here does reentrant connect/disconnect from within its own
// dispatch, and no test spawns real threads, so all three MutexType
// variants (Event, SharedEvent, SingleThreadedEvent) are safe.
// ---------------------------------------------------------------------------

template< typename MutexType >
class BasicConnections : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( BasicConnections, MutexTypes );

TYPED_TEST( BasicConnections, ConnectionWithoutHandle )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 10, 20 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 10 );
	EXPECT_EQ( handler->lastY, 20 );

	button->click( 30, 40 );
	EXPECT_EQ( handler->callCount, 2 );
	EXPECT_EQ( handler->lastX, 30 );
	EXPECT_EQ( handler->lastY, 40 );

	button->clicked.disconnect( *handler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );

	handler->reset();
	button->click( 50, 60 );
	EXPECT_EQ( handler->callCount, 0 );
	EXPECT_EQ( handler->lastX, 0 );
	EXPECT_EQ( handler->lastY, 0 );
}

TYPED_TEST( BasicConnections, ConnectionWithHandleAndDisconnect )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );
	EXPECT_TRUE( conn.isConnected() );

	button->click( 5, 15 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	handler->reset();
	button->click( 25, 35 );
	EXPECT_EQ( handler->callCount, 0 );
}

TYPED_TEST( BasicConnections, MultipleConnections )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler1 = std::make_unique< TestHandler >();
	auto handler2 = std::make_unique< TestHandler >();
	auto handler3 = std::make_unique< TestHandler >();

	button->clicked.connect( *handler1, &TestHandler::onClicked );
	button->clicked.connect( *handler2, &TestHandler::onClicked );
	button->clicked.connect( *handler3, &TestHandler::onClicked );

	button->click( 1, 2 );

	EXPECT_EQ( handler1->callCount, 1 );
	EXPECT_EQ( handler2->callCount, 1 );
	EXPECT_EQ( handler3->callCount, 1 );
}

TYPED_TEST( BasicConnections, LambdaConnectionWithoutHandle )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	int callCount = 0;
	int lastX = 0;
	int lastY = 0;

	button->clicked.connectLambda( *receiver, [ &callCount, &lastX, &lastY ]( int x, int y ) {
		callCount++;
		lastX = x;
		lastY = y;
	} );

	button->click( 100, 200 );
	EXPECT_EQ( callCount, 1 );
	EXPECT_EQ( lastX, 100 );
	EXPECT_EQ( lastY, 200 );
}

TYPED_TEST( BasicConnections, ManualDisconnection )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TYPED_TEST( BasicConnections, AutoDisconnectOnReceiverDestruction )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();

	{
		auto handler = std::make_unique< TestHandler >();
		button->clicked.connect( *handler, &TestHandler::onClicked );

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );

		// handler destroyed here
	}

	// must not crash
	EXPECT_NO_THROW( button->click( 2, 2 ) );
}

TYPED_TEST( BasicConnections, DisconnectSpecificReceiver )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler1 = std::make_unique< TestHandler >();
	auto handler2 = std::make_unique< TestHandler >();

	button->clicked.connect( *handler1, &TestHandler::onClicked );
	button->clicked.connect( *handler2, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler1->callCount, 1 );
	EXPECT_EQ( handler2->callCount, 1 );

	button->clicked.disconnect( *handler1 );
	handler1->reset();
	handler2->reset();

	button->click( 2, 2 );
	EXPECT_EQ( handler1->callCount, 0 );
	EXPECT_EQ( handler2->callCount, 1 );
}

TYPED_TEST( BasicConnections, NTTPConnectAndInvoke )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.template connect< &TestHandler::onClicked >( *handler );

	button->click( 10, 20 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 10 );
	EXPECT_EQ( handler->lastY, 20 );

	button->click( 30, 40 );
	EXPECT_EQ( handler->callCount, 2 );
	EXPECT_EQ( handler->lastX, 30 );
	EXPECT_EQ( handler->lastY, 40 );
}

TYPED_TEST( BasicConnections, NTTPDisconnect )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.template connect< &TestHandler::onClicked >( *handler );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->clicked.template disconnect< &TestHandler::onClicked >( *handler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 0 );
}

TYPED_TEST( BasicConnections, NTTPDisconnectLeavesOtherReceiverConnected )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler1 = std::make_unique< TestHandler >();
	auto handler2 = std::make_unique< TestHandler >();

	// mix: handler1 via NTTP connect, handler2 via runtime connect - the
	// NTTP disconnect below must only remove the handler1 connection
	button->clicked.template connect< &TestHandler::onClicked >( *handler1 );
	button->clicked.connect( *handler2, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler1->callCount, 1 );
	EXPECT_EQ( handler2->callCount, 1 );

	button->clicked.template disconnect< &TestHandler::onClicked >( *handler1 );
	handler1->reset();
	handler2->reset();

	button->click( 2, 2 );
	EXPECT_EQ( handler1->callCount, 0 );
	EXPECT_EQ( handler2->callCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( BasicConnections, LambdaAsCallback )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();
	int result = 0;

	button->clicked.connectLambda( *receiver, [ &result ]( int x, int y ) {
		result = x + y;
	} );

	button->click( 10, 20 );
	EXPECT_EQ( result, 30 );

	button->click( 5, 7 );
	EXPECT_EQ( result, 12 );
}
