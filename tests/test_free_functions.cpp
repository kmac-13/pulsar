#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Free Functions and Lambdas
//
// Split into two typed suites:
//   - FreeFunctions (AllEventTypes): no reentrant connect/disconnect from
//     within dispatch, no once().
//   - FreeFunctionsRestricted (Event/SingleThreadedEvent only):
//     FreeFunctionsAndLambdas ends with a once() connection, which
//     static_asserts against SharedMutex - the whole test moves here rather
//     than fragmenting its single sequential narrative.
// ---------------------------------------------------------------------------

template< typename MutexType >
class FreeFunctions : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( FreeFunctions, MutexTypes );

TYPED_TEST( FreeFunctions, StaticFunctionConnection )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
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

TYPED_TEST( FreeFunctions, MixedConnectionTypes )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();
	int lambdaCalls = 0;

	button->clicked.connect( *handler, &TestHandler::onClicked );
	button->clicked.connectLambda( [ &lambdaCalls ]( int, int ) { lambdaCalls++; } );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( lambdaCalls, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );

	handler.reset();

	button->click( 2, 2 );
	EXPECT_EQ( lambdaCalls, 2 );

	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( FreeFunctions, DisconnectFreeFunctionByPointer )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	button->clicked.connectFree( staticClickHandler );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	button->clicked.disconnectFree( staticClickHandler );

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, DisconnectNonCapturingLambda )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();

	auto lambda = []( int, int ) {};
	button->clicked.connectFree( lambda );
	button->click( 1, 1 );

	button->clicked.disconnectFree( static_cast< void ( * )( int, int ) >( lambda ) );
	button->click( 2, 2 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, DisconnectSpecificFreeFunction )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();

	button->clicked.connectFree( staticClickHandler );
	button->clicked.connectFree( anotherStaticHandler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );

	button->clicked.disconnectFree( staticClickHandler );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );

	button->clicked.disconnectFree( anotherStaticHandler );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, CapturingLambdaRequiresHandle )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	int callCount = 0;

	auto conn = button->clicked.connectLambda( [ &callCount ]( int, int ) {
		callCount++;
	} );

	button->click( 1, 1 );
	EXPECT_EQ( callCount, 1 );

	conn.disconnect();

	button->click( 2, 2 );
	EXPECT_EQ( callCount, 1 );
}

TYPED_TEST( FreeFunctions, MixedFreeFunctionsAndLambdas )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	int lambdaCalls = 0;
	_staticCallCount = 0;

	button->clicked.connectFree( staticClickHandler );
	auto conn = button->clicked.connectLambda( [ &lambdaCalls ]( int, int ) {
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
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, NTTPConnectAndInvoke )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	auto conn = button->clicked.template connectFree< &staticClickHandler >();
	EXPECT_TRUE( conn.isConnected() );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 2 );
}

TYPED_TEST( FreeFunctions, NTTPDisconnectFreeFunction )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	button->clicked.template connectFree< &staticClickHandler >();

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	button->clicked.template disconnectFree< &staticClickHandler >();

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, NTTPDisconnectLeavesOtherFunctionConnected )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	// mix: staticClickHandler via NTTP connectFree, anotherStaticHandler
	// via runtime connectFree - the NTTP disconnect below must only
	// remove the staticClickHandler connection
	button->clicked.template connectFree< &staticClickHandler >();
	button->clicked.connectFree( anotherStaticHandler );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );

	button->clicked.template disconnectFree< &staticClickHandler >();
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );

	button->clicked.disconnectFree( anotherStaticHandler );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

// ---------------------------------------------------------------------------
// Trackable-anchored free-function connections (connectFree with an explicit
// tracker).  A free function has no receiver of its own, so the tracker is a
// separate object whose lifetime bounds the connection: when it is destroyed
// the connection is removed automatically, without holding the Connection.
// ---------------------------------------------------------------------------

namespace
{
	int partialFreeCalls = 0;

	// single-arg free function, used as a partial-arity target on the
	// two-arg `clicked` event (the trailing int is dropped)
	void partialFreeHandler( int )
	{
		partialFreeCalls++;
	}
}

TYPED_TEST( FreeFunctions, TrackedRuntimeFreeFunctionAutoDisconnects )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	auto tracker = std::make_shared< TestHandler >();
	button->clicked.connectFree( *tracker, staticClickHandler );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );

	tracker.reset();  // tracker destruction removes the connection

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, TrackedNTTPFreeFunctionAutoDisconnects )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	auto tracker = std::make_shared< TestHandler >();
	button->clicked.template connectFree< &staticClickHandler >( *tracker );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 1 );

	tracker.reset();

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, TrackedNTTPPartialArityAutoDisconnects )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	partialFreeCalls = 0;

	auto tracker = std::make_shared< TestHandler >();
	button->clicked.template connectFree< &partialFreeHandler >( *tracker );  // drops 2nd int

	button->click( 7, 8 );
	EXPECT_EQ( partialFreeCalls, 1 );

	tracker.reset();

	button->click( 9, 10 );
	EXPECT_EQ( partialFreeCalls, 1 );
}

TYPED_TEST( FreeFunctions, TrackedFreeFunctionExplicitDisconnectThenTrackerDeath )
{
	// after an explicit disconnectFree, a subsequent tracker destruction must
	// be a safe no-op (its record refers to an already-removed connection)
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_staticCallCount = 0;

	auto tracker = std::make_shared< TestHandler >();
	button->clicked.connectFree( *tracker, staticClickHandler );

	button->clicked.disconnectFree( staticClickHandler );

	button->click( 1, 1 );
	EXPECT_EQ( _staticCallCount, 0 );

	tracker.reset();

	button->click( 2, 2 );
	EXPECT_EQ( _staticCallCount, 0 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( FreeFunctions, TrackedFreeFunctionParamOrderingsCompile )
{
	// tracked connectFree accepts both (type, params) and (params, type),
	// matching the untracked connectFree overload set
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto tracker = std::make_shared< TestHandler >();
	auto& ev = button->clicked;

	ev.connectFree( *tracker, staticClickHandler, pulsar::ConnectionType::Direct );
	ev.connectFree( *tracker, staticClickHandler, pulsar::ConnectionType::Direct, ev.params().prio( 5 ) );
	ev.connectFree( *tracker, staticClickHandler, ev.params().prio( 5 ) );
	ev.connectFree( *tracker, staticClickHandler, ev.params().prio( 5 ), pulsar::ConnectionType::Direct );

	ev.template connectFree< &staticClickHandler >( *tracker, pulsar::ConnectionType::Direct );
	ev.template connectFree< &staticClickHandler >( *tracker, ev.params().prio( 3 ) );
	ev.template connectFree< &staticClickHandler >( *tracker, ev.params().prio( 3 ), pulsar::ConnectionType::Direct );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 7u );
}

// ---------------------------------------------------------------------------
// Restricted suite: once(). Event/SingleThreadedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class FreeFunctionsRestricted : public ::testing::Test {};

using RestrictedMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( FreeFunctionsRestricted, RestrictedMutexTypes );

TYPED_TEST( FreeFunctionsRestricted, FreeFunctionsAndLambdas )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();

	int lambdaCalls = 0;
	auto conn1 = button->clicked.connectLambda( [ &lambdaCalls ]( int, int ) { lambdaCalls++; } );

	EXPECT_TRUE( conn1.isConnected() );

	button->click( 1, 1 );
	EXPECT_EQ( lambdaCalls, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( lambdaCalls, 2 );

	int lastX = 0, lastY = 0;
	button->clicked.connectLambda( [ &lastX, &lastY ]( int x, int y ) {
		lastX = x;
		lastY = y;
	} );

	button->click( 10, 20 );
	EXPECT_EQ( lastX, 10 );
	EXPECT_EQ( lastY, 20 );
	EXPECT_EQ( lambdaCalls, 3 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().directConnectionCount, 2u );
	EXPECT_EQ( inspector.getEventInfo().deferredConnectionCount, 0u );

	conn1.disconnect();
	EXPECT_FALSE( conn1.isConnected() );

	button->click( 30, 40 );
	EXPECT_EQ( lambdaCalls, 3 );
	EXPECT_EQ( lastX, 30 );
	EXPECT_EQ( lastY, 40 );

	button->clicked.disconnectAll();
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );

	lastX = lastY = 0;
	button->click( 50, 60 );
	EXPECT_EQ( lastX, 0 );
	EXPECT_EQ( lastY, 0 );

	int onceCalls = 0;
	button->clicked.connectLambda( [ &onceCalls ]( int, int ) { onceCalls++; }, button->clicked.params().once() );

	button->click( 1, 1 );
	EXPECT_EQ( onceCalls, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( onceCalls, 1 );

	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}
