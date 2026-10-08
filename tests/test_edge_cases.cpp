#include "test_helpers.h"

#include <atomic>

// ---------------------------------------------------------------------------
// Edge Cases
//
// forwardTo() does not exist as a first-class method on Event - relaying one
// event into another is expressed directly with connectLambda(tracker,
// [target](Args... a) { target(a...); }), the same pattern RelayNodeT in
// test_helpers.hpp already uses for its own dataIn -> dataOut relay.
//
// Split into two typed suites:
//   - EdgeCases (AllEventTypes): no reentrant connect/disconnect on the same
//     event from within its own dispatch.
//   - EdgeCasesReentrant (Event/SingleThreadedEvent only): a handler
//     disconnects its own connection, or destroys a fellow receiver, from
//     within that same event's dispatch - SharedEvent's shared-lock dispatch
//     path doesn't support this (confirmed deadlock pattern, see
//     test_pending_removal_stress.cpp / test_reentrancy.cpp).
// ---------------------------------------------------------------------------

template< typename MutexType >
class EdgeCases : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( EdgeCases, MutexTypes );

TYPED_TEST( EdgeCases, EventForwarding )
{
	auto source = std::make_shared< DataSenderT< TypeParam > >();
	auto target = std::make_shared< DataSenderT< TypeParam > >();
	auto receiver = std::make_shared< DataReceiver >();

	auto conn = source->dataReady.connectLambda( *target, [ target ]( int v ) { target->dataReady( v ); } );
	target->dataReady.connect( *receiver, &DataReceiver::processData );

	source->sendData( 42 );
	EXPECT_EQ( receiver->lastValue, 42 );
	EXPECT_EQ( receiver->callCount, 1 );

	conn.disconnect();

	source->sendData( 99 );
	EXPECT_EQ( receiver->callCount, 1 );
}

TYPED_TEST( EdgeCases, ChainedEventForwarding )
{
	auto source = std::make_shared< DataSenderT< TypeParam > >();
	auto relay1 = std::make_shared< RelayNodeT< TypeParam > >();
	auto relay2 = std::make_shared< RelayNodeT< TypeParam > >();
	auto receiver = std::make_shared< DataReceiver >();

	relay1->setupRelay();
	relay2->setupRelay();

	source->dataReady.connectLambda( *relay1, [ relay1 ]( int v ) { relay1->dataIn( v ); } );
	relay1->dataOut.connectLambda( *relay2, [ relay2 ]( int v ) { relay2->dataIn( v ); } );
	relay2->dataOut.connect( *receiver, &DataReceiver::processData );

	source->sendData( 123 );
	EXPECT_EQ( receiver->lastValue, 123 );
	EXPECT_EQ( receiver->callCount, 1 );
}

TYPED_TEST( EdgeCases, EmptyEventEmission )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();

	// shouldn't throw when there are no connections
	EXPECT_NO_THROW( button->click( 1, 1 ) );

	auto inspector = kmac::stellyra::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().connectionCount, 0u );
}

TYPED_TEST( EdgeCases, DoubleDisconnect )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );

	conn.disconnect();
	EXPECT_NO_THROW( conn.disconnect() );  // safe to call twice

	EXPECT_FALSE( conn.isConnected() );
}

TYPED_TEST( EdgeCases, UseAfterDisconnect )
{
	// a disconnected Connection stays safely inert and the handler receives
	// nothing further
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.disconnect();

	EXPECT_FALSE( conn.isConnected() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_FALSE( conn.isBlocked() );
	EXPECT_NO_THROW( conn.block() );    // no-op
	EXPECT_NO_THROW( conn.unblock() );  // no-op
}

TYPED_TEST( EdgeCases, NullSenderEvent )
{
	// an Event constructed with nullptr sender has no associated Trackable,
	// but it should still connect and emit without crashing, behaving as
	// a plain Direct-connection event with no sender lifetime tracking
	auto handler = std::make_shared< stellyra::Trackable >();
	int callCount = 0;

	stellyra::BasicEvent< TypeParam, std::string > event{ nullptr };

	event.connectLambda( *handler, [ &callCount ]( const std::string& ) { callCount++; } );

	EXPECT_NO_THROW( event.emit( "test message" ) );
	EXPECT_EQ( callCount, 1 );

	// auto-disconnect still works when receiver is destroyed
	handler.reset();
	EXPECT_NO_THROW( event.emit( "after reset" ) );
	EXPECT_EQ( callCount, 1 );
}

TYPED_TEST( EdgeCases, RecursiveEmission )
{
	// button1's handler recurses into button2->click(), and button2's
	// handler recurses back into button1->click() - each button has its
	// own independent EventImpl/mutex, so this isn't reentrancy on a single
	// mutex on the first bounce, but the SECOND bounce (button1 called
	// again while button1's own outer dispatch is still on the stack) is
	// genuine same-mutex reentrancy.  RecursiveMutex/NullMutex support
	// same-thread re-entry; SharedMutex's lock_shared() is documented as
	// re-entry-safe on libstdc++ absent a concurrently blocked writer
	// (which cannot happen here - single thread, no other threads to
	// enqueue a writer) - verified empirically, not just by inspection.
	auto button1 = std::make_shared< TestButtonT< TypeParam > >();
	auto button2 = std::make_shared< TestButtonT< TypeParam > >();
	auto receiver = std::make_shared< TestHandler >();

	std::atomic< int > depth{ 0 };
	std::atomic< int > maxDepth{ 0 };

	button1->clicked.connectLambda( *receiver, [ &button2, &depth, &maxDepth ]( int x, int ) {
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

	button2->clicked.connectLambda( *receiver, [ &button1, &depth, &maxDepth ]( int x, int ) {
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

// ---------------------------------------------------------------------------
// Reentrant disconnect during dispatch - Event/SingleThreadedEvent only
// (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class EdgeCasesReentrant : public ::testing::Test {};

using ReentrantMutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( EdgeCasesReentrant, ReentrantMutexTypes );

TYPED_TEST( EdgeCasesReentrant, SelfDisconnectionDuringEmission )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();

	stellyra::Connection conn;
	conn = button->clicked.connectLambda( *handler, [ &handler, &conn ]( int, int ) {
		handler->callCount++;
		conn.disconnect();
	} );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TYPED_TEST( EdgeCasesReentrant, HandlerDestructionDuringEmission )
{
	// handler2's lambda destroys handler1 mid-dispatch; ~Trackable() calls
	// disconnectAll(), which disconnects handler1's connection on this same
	// button->clicked event while its dispatch is still in progress
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler1 = std::make_shared< TestHandler >();
	auto handler2 = std::make_shared< TestHandler >();

	button->clicked.connect( *handler1, &TestHandler::onClicked );
	button->clicked.connectLambda( *handler2, [ &handler1, &handler2 ]( int, int ) {
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
