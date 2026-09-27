#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Advanced Connections
//
// Split into two typed suites:
//   - AdvancedConnections (AllEventTypes): priority ordering, blocking,
//     predicates, and FIFO tie-breaking - none of it reentrant.
//   - AdvancedConnectionsRestricted (Event/SingleThreadedEvent only):
//     once() (static_asserts against SharedMutex), plus the two reentrant
//     connect/disconnect-during-dispatch tests, which deadlock under
//     SharedEvent's shared-lock dispatch path (confirmed empirically - see
//     test_pending_removal_stress.cpp / test_reentrancy.cpp).
// ---------------------------------------------------------------------------

template< typename MutexType >
class AdvancedConnections : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( AdvancedConnections, MutexTypes );

TYPED_TEST( AdvancedConnections, SamePriorityFIFO )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	std::vector< std::string > order;

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "First" ); } );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "Second" ); } );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "Third" ); } );

	button->click( 1, 1 );

	ASSERT_EQ( order.size(), 3u );
	EXPECT_EQ( order[ 0 ], "First" );
	EXPECT_EQ( order[ 1 ], "Second" );
	EXPECT_EQ( order[ 2 ], "Third" );
}

TYPED_TEST( AdvancedConnections, ConditionalConnection )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connectLambda(
		*handler,
		[ handler = handler.get() ]( int x, int y ) { handler->onClicked( x, y ); },  // handler
		{ []( int x, int y ) { return x > 0 && y > 0; } } );                          // predicate

	button->click( 10, 20 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( -5, 10 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 5, -10 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 15, 25 );
	EXPECT_EQ( handler->callCount, 2 );
}

TYPED_TEST( AdvancedConnections, ConnectionBlocking )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	auto conn = button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.block();
	EXPECT_TRUE( conn.isBlocked() );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );

	conn.unblock();
	EXPECT_FALSE( conn.isBlocked() );

	button->click( 3, 3 );
	EXPECT_EQ( handler->callCount, 2 );
}

TYPED_TEST( AdvancedConnections, PriorityExecutionOrder )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	std::vector< int > order;

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 3 ); }, 50 );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 1 ); }, 200 );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 4 ); }, 25 );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 2 ); }, 100 );

	button->click( 0, 0 );

	ASSERT_EQ( order.size(), 4u );
	EXPECT_EQ( order[ 0 ], 1 );  // priority 200
	EXPECT_EQ( order[ 1 ], 2 );  // priority 100
	EXPECT_EQ( order[ 2 ], 3 );  // priority 50
	EXPECT_EQ( order[ 3 ], 4 );  // priority 25
}

// similar to AdvancedConnections.PriorityExecutionOrder, but events are triggered interspersed with connections
TYPED_TEST( AdvancedConnections, PriorityWithDynamicConnections )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	std::vector< std::string > order;

	// priority is unsigned (no negative-priority concept); use 0/10 to
	// express "low/high" - higher values run first
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "Low" ); }, 0 );

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "High" ); }, 10 );

	button->click( 0, 0 );
	ASSERT_EQ( order.size(), 2u );
	EXPECT_EQ( order[ 0 ], "High" );
	EXPECT_EQ( order[ 1 ], "Low" );

	order.clear();
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( "Medium" ); }, 5 );

	button->click( 0, 0 );
	ASSERT_EQ( order.size(), 3u );
	EXPECT_EQ( order[ 0 ], "High" );
	EXPECT_EQ( order[ 1 ], "Medium" );
	EXPECT_EQ( order[ 2 ], "Low" );
}

// ---------------------------------------------------------------------------
// Priority with the NTTP free-function connect form
// ---------------------------------------------------------------------------

namespace
{
	std::vector< int >* nttpPriorityOrder = nullptr;
	void nttpPriorityA( int, int ) { nttpPriorityOrder->push_back( 1 ); }
	void nttpPriorityB( int, int ) { nttpPriorityOrder->push_back( 2 ); }
	void nttpPriorityC( int, int ) { nttpPriorityOrder->push_back( 3 ); }
}

TYPED_TEST( AdvancedConnections, NTTPFreeFunctionPriority )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();

	std::vector< int > order;
	nttpPriorityOrder = &order;

	button->clicked.template connectFree< &nttpPriorityA >( 1u );
	button->clicked.template connectFree< &nttpPriorityC >( 3u );
	button->clicked.template connectFree< &nttpPriorityB >( 2u );

	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 3, 2, 1 } ) );

	button->clicked.disconnectAll();
}

// ---------------------------------------------------------------------------
// Zero-priority (default) connections stay FIFO among themselves and fire
// after every non-zero-priority connection, regardless of connection order.
// ---------------------------------------------------------------------------

TYPED_TEST( AdvancedConnections, MixedPriorityAndDefaultOrdering )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	std::vector< int > order;

	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 10 ); } );  // default 0
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 20 ); } );  // default 0
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 5 ); }, 5u );
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 30 ); } );  // default 0

	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 5, 10, 20, 30 } ) );
}

// ---------------------------------------------------------------------------
// blockGuard() nesting: the event stays blocked until every outstanding
// guard has been released, not just the most recently acquired one.
// ---------------------------------------------------------------------------

TYPED_TEST( AdvancedConnections, BlockGuardNesting )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );

	{
		auto g1 = button->clicked.blockGuard();

		{
			auto g2 = button->clicked.blockGuard();
			button->click( 1, 1 );
			EXPECT_EQ( handler->callCount, 0 );  // blocked by both g1 and g2
		}
		// g2 released - still blocked by g1

		button->click( 2, 2 );
		EXPECT_EQ( handler->callCount, 0 );
	}
	// g1 released - unblocked

	button->click( 3, 3 );
	EXPECT_EQ( handler->callCount, 1 );
}

// ---------------------------------------------------------------------------
// Restricted suite: once() and reentrant connect/disconnect-during-dispatch.
// Event/SingleThreadedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class AdvancedConnectionsRestricted : public ::testing::Test {};

using RestrictedMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( AdvancedConnectionsRestricted, RestrictedMutexTypes );

TYPED_TEST( AdvancedConnectionsRestricted, SingleShotConnection )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.template connect< &TestHandler::onClicked >( *handler, button->clicked.params().once() );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );  // auto-disconnected after first call
}

// ---------------------------------------------------------------------------
// Priority combined with reentrancy: a connection made from within a
// higher-numbered-priority handler is excluded from the emission already in
// progress, and correctly takes its place in priority order on the next one.
// ---------------------------------------------------------------------------

TYPED_TEST( AdvancedConnectionsRestricted, ReentrantConnectDuringPriorityDispatch )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_shared< pulsar::Trackable >();  // can't be unique_ptr due to capturing lambda

	std::vector< int > order;
	bool added = false;
	pulsar::Connection addedConn;

	button->clicked.connectLambda( *receiver, [ &, receiver ]( int, int ) {
		order.push_back( 5 );
		if ( ! added )
		{
			added = true;
			// higher priority than the connection currently dispatching -
			// still excluded from this emission, since the active handler
			// snapshot was already taken
			addedConn = button->clicked.connectLambda(
				*receiver, [ &order ]( int, int ) { order.push_back( 99 ); }, 99u );
		}
	}, 5u );
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 0 ); }, 0u );

	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 5, 0 } ) );  // reentrant connection excluded

	order.clear();
	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 99, 5, 0 } ) );  // now correctly ordered first
}

TYPED_TEST( AdvancedConnectionsRestricted, ReentrantDisconnectDuringPriorityDispatch )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto receiver = std::make_unique< pulsar::Trackable >();

	std::vector< int > order;
	pulsar::Connection selfConn;

	selfConn = button->clicked.connectLambda( *receiver, [ &order, &selfConn ]( int, int ) {
		order.push_back( 10 );
		selfConn.disconnect();  // disconnect self mid-dispatch
	}, 10u );
	button->clicked.connectLambda( *receiver, [ &order ]( int, int ) { order.push_back( 0 ); }, 0u );

	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 10, 0 } ) );  // both still fire this time

	order.clear();
	button->click( 0, 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 0 } ) );  // priority-10 handler gone
}
