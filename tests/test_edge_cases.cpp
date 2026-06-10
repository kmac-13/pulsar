#include "test_helpers.hpp"
#include <atomic>

// ---------------------------------------------------------------------------
// Edge Cases
// ---------------------------------------------------------------------------

TEST( EdgeCases, SignalForwarding )
{
	auto source = std::make_shared< DataSender >();
	auto target = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	auto conn = source->dataReady.forwardTo( target->dataReady );
	target->dataReady.connect( receiver, &DataReceiver::processData );

	source->sendData( 42 );
	EXPECT_EQ( receiver->lastValue, 42 );
	EXPECT_EQ( receiver->callCount, 1 );

	conn.disconnect();

	source->sendData( 99 );
	EXPECT_EQ( receiver->callCount, 1 );
}

TEST( EdgeCases, ChainedSignalForwarding )
{
	auto source = std::make_shared< DataSender >();
	auto relay1 = std::make_shared< RelayNode >();
	auto relay2 = std::make_shared< RelayNode >();
	auto receiver = std::make_shared< DataReceiver >();

	relay1->setupRelay();
	relay2->setupRelay();

	source->dataReady.forwardTo( relay1->dataIn );
	relay1->dataOut.forwardTo( relay2->dataIn );
	relay2->dataOut.connect( receiver, &DataReceiver::processData );

	source->sendData( 123 );
	EXPECT_EQ( receiver->lastValue, 123 );
	EXPECT_EQ( receiver->callCount, 1 );
}

TEST( EdgeCases, EmptyEventEmission )
{
	auto button = std::make_shared< TestButton >();

	// shouldn't throw when there are no connections
	EXPECT_NO_THROW( button->click( 1, 1 ) );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TEST( EdgeCases, SelfDisconnectionDuringEmission )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::Connection conn;
	conn = button->clicked.connect( handler, [ &handler, &conn ]( int, int ) {
		handler->callCount++;
		conn.disconnect();
	} );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( EdgeCases, RecursiveEmission )
{
	auto button1 = std::make_shared< TestButton >();
	auto button2 = std::make_shared< TestButton >();
	auto receiver = std::make_shared< TestHandler >();

	std::atomic< int > depth{ 0 };
	std::atomic< int > maxDepth{ 0 };

	button1->clicked.connect( receiver, [ &button2, &depth, &maxDepth ]( int x, int ) {
		depth++;
		int d = depth.load();
		int m = maxDepth.load();
		while ( d > m && !maxDepth.compare_exchange_weak( m, d ) );
		if ( depth < 5 )
		{
			button2->click( x + 1, 0 );
		}
		depth--;
	} );

	button2->clicked.connect( receiver, [ &button1, &depth, &maxDepth ]( int x, int ) {
		depth++;
		int d = depth.load();
		int m = maxDepth.load();
		while ( d > m && !maxDepth.compare_exchange_weak( m, d ) );
		if ( depth < 5 )
		{
			button1->click( x + 1, 0 );
		}
		depth--;
	} );

	button1->click( 0, 0 );

	EXPECT_GE( maxDepth.load(), 5 );
	EXPECT_EQ( depth.load(), 0 );
}

TEST( EdgeCases, DoubleDisconnect )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );

	conn.disconnect();
	EXPECT_NO_THROW( conn.disconnect() );  // safe to call twice

	EXPECT_FALSE( conn.isConnected() );
}

TEST( EdgeCases, UseAfterDisconnect )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();

	EXPECT_FALSE( conn.isConnected() );
	EXPECT_FALSE( conn.isBlocked() );
	EXPECT_NO_THROW( conn.block() );    // no-op
	EXPECT_NO_THROW( conn.unblock() );  // no-op

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( EdgeCases, HandlerDestructionDuringEmission )
{
	auto button = std::make_shared< TestButton >();
	auto handler1 = std::make_shared< TestHandler >();
	auto handler2 = std::make_shared< TestHandler >();

	button->clicked.connect( handler1, &TestHandler::onClicked );
	button->clicked.connect( handler2, [ &handler1, &handler2 ]( int, int ) {
		handler2->callCount++;
		handler1.reset();  // destroy handler1 during emission
	} );

	// must not crash
	EXPECT_NO_THROW( button->click( 1, 1 ) );

	EXPECT_EQ( handler2->callCount, 1 );
	EXPECT_EQ( handler1, nullptr );

	button->click( 2, 2 );
	EXPECT_EQ( handler2->callCount, 2 );
}

TEST( EdgeCases, NullSenderEvent )
{
	// an Event constructed with nullptr sender has no associated Object,
	// but it should still connect and emit without crashing, behaving as
	// a plain Direct-connection event with no sender lifetime tracking
	auto handler = std::make_shared< pulsar::Object >();
	int callCount = 0;

	pulsar::Event< std::string > event{ nullptr };

	event.connect( handler, [ &callCount ]( const std::string& ) { callCount++; } );

	EXPECT_NO_THROW( event.emit( "test message" ) );
	EXPECT_EQ( callCount, 1 );

	// auto-disconnect still works when receiver is destroyed
	handler.reset();
	EXPECT_NO_THROW( event.emit( "after reset" ) );
	EXPECT_EQ( callCount, 1 );
}
