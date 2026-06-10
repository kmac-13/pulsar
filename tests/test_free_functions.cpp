#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Free Functions and Lambdas
// ---------------------------------------------------------------------------

TEST( FreeFunctions, FreeFunctionsAndLambdas )
{
	auto button = std::make_shared< TestButton >();

	int lambdaCalls = 0;
	auto conn1 = button->clicked.connectFree( [ &lambdaCalls ]( int, int ) { lambdaCalls++; } );

	EXPECT_TRUE( conn1.isConnected() );

	button->click( 1, 1 );
	EXPECT_EQ( lambdaCalls, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( lambdaCalls, 2 );

	int lastX = 0, lastY = 0;
	button->clicked.connectFree( [ &lastX, &lastY ]( int x, int y ) {
		lastX = x;
		lastY = y;
	} );

	button->click( 10, 20 );
	EXPECT_EQ( lastX, 10 );
	EXPECT_EQ( lastY, 20 );
	EXPECT_EQ( lambdaCalls, 3 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().directConnectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().deferredConnectionCount, 0u );

	conn1.disconnect();
	EXPECT_FALSE( conn1.isConnected() );

	button->click( 30, 40 );
	EXPECT_EQ( lambdaCalls, 3 );
	EXPECT_EQ( lastX, 30 );
	EXPECT_EQ( lastY, 40 );

	button->clicked.disconnectAll();
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );

	lastX = lastY = 0;
	button->click( 50, 60 );
	EXPECT_EQ( lastX, 0 );
	EXPECT_EQ( lastY, 0 );

	int onceCalls = 0;
	button->clicked.connectOnceFree( [ &onceCalls ]( int, int ) { onceCalls++; } );

	button->click( 1, 1 );
	EXPECT_EQ( onceCalls, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( onceCalls, 1 );

	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TEST( FreeFunctions, StaticFunctionConnection )
{
	auto button = std::make_shared< TestButton >();
	_staticCallCount = 0;

	auto conn = button->clicked.connectFree( staticClickHandler );
	EXPECT_TRUE( conn.isConnected() );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 2 );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );

	button->click( 3, 3 );
	EXPECT_EQ( _staticCallCount, 2 );
}

TEST( FreeFunctions, MixedConnectionTypes )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	int  lambdaCalls = 0;

	button->clicked.connect( handler, &TestHandler::onClicked );
	button->clicked.connectFree( [ &lambdaCalls ]( int, int ) { lambdaCalls++; } );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( lambdaCalls, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );

	handler.reset();

	button->click( 2, 2 );
	EXPECT_EQ( lambdaCalls, 2 );

	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TEST( FreeFunctions, DisconnectFreeFunctionByPointer )
{
	auto button = std::make_shared< TestButton >();
	_staticCallCount = 0;

	button->clicked.connectFree( staticClickHandler );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	button->clicked.disconnectFree( staticClickHandler );

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TEST( FreeFunctions, DisconnectNonCapturingLambda )
{
	auto button = std::make_shared< TestButton >();

	auto lambda = []( int, int ) {};
	button->clicked.connectFree( lambda );
	button->click( 1, 1 );

	button->clicked.disconnectFree( static_cast< void ( * )( int, int ) >( lambda ) );
	button->click( 2, 2 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TEST( FreeFunctions, DisconnectSpecificFreeFunction )
{
	auto button = std::make_shared< TestButton >();

	button->clicked.connectFree( staticClickHandler );
	button->clicked.connectFree( anotherStaticHandler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 2u );

	button->clicked.disconnectFree( staticClickHandler );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 1u );

	button->clicked.disconnectFree( anotherStaticHandler );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TEST( FreeFunctions, CapturingLambdaRequiresHandle )
{
	auto button = std::make_shared< TestButton >();
	int  callCount = 0;

	auto conn = button->clicked.connectFree( [ &callCount ]( int, int ) {
		callCount++;
	} );

	button->click( 1, 1 );
	EXPECT_EQ( callCount, 1 );

	conn.disconnect();

	button->click( 2, 2 );
	EXPECT_EQ( callCount, 1 );
}

TEST( FreeFunctions, MixedFreeFunctionsAndLambdas )
{
	auto button = std::make_shared< TestButton >();
	int  lambdaCalls = 0;
	_staticCallCount = 0;

	button->clicked.connectFree( staticClickHandler );
	auto conn = button->clicked.connectFree( [ &lambdaCalls ]( int, int ) {
		lambdaCalls++;
	} );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );
	EXPECT_EQ( lambdaCalls, 1 );

	button->clicked.disconnectFree( staticClickHandler );

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );
	EXPECT_EQ( lambdaCalls, 2 );

	conn.disconnect();

	button->click( 3, 3 );
	EXPECT_EQ( _staticCallCount, 1 );
	EXPECT_EQ( lambdaCalls, 2 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}
