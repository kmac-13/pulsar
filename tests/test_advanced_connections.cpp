#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Advanced Connections
// ---------------------------------------------------------------------------

TEST( AdvancedConnections, SingleShotConnection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connectOnce( handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );  // auto-disconnected after first call
}

TEST( AdvancedConnections, PriorityConnections )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::vector< std::string > order;

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Low" ); }, -100 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Normal" ); }, 0 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "High" ); }, 100 );

	button->click( 1, 1 );

	ASSERT_EQ( order.size(), 3u );
	EXPECT_EQ( order[ 0 ], "High" );
	EXPECT_EQ( order[ 1 ], "Normal" );
	EXPECT_EQ( order[ 2 ], "Low" );
}

TEST( AdvancedConnections, SamePriorityFIFO )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::vector< std::string > order;

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "First" ); }, 0 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Second" ); }, 0 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Third" ); }, 0 );

	button->click( 1, 1 );

	ASSERT_EQ( order.size(), 3u );
	EXPECT_EQ( order[ 0 ], "First" );
	EXPECT_EQ( order[ 1 ], "Second" );
	EXPECT_EQ( order[ 2 ], "Third" );
}

TEST( AdvancedConnections, ConditionalConnection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connectIf( handler, &TestHandler::onClicked, []( int x, int y ) { return x > 0 && y > 0; } );

	button->click( 10, 20 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( -5, 10 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 5, -10 );
	EXPECT_EQ( handler->callCount, 1 );

	button->click( 15, 25 );
	EXPECT_EQ( handler->callCount, 2 );
}

TEST( AdvancedConnections, ConnectionBlocking )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );

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

TEST( AdvancedConnections, ConditionalConnectionFiltering )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();
	int  count = 0;

	button->clicked.connectIf( receiver, [ &count ]( int, int ) { count++; }, []( int x, int ) { return x >= 100; } );

	button->click( 50, 50 );
	EXPECT_EQ( count, 0 );

	button->click( 100, 10 );
	EXPECT_EQ( count, 1 );

	button->click( 99, 99 );
	EXPECT_EQ( count, 1 );

	button->click( 200, 5 );
	EXPECT_EQ( count, 2 );
}

TEST( AdvancedConnections, PriorityExecutionOrder )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::vector< int > order;

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( 3 ); }, 50 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( 1 ); }, 200 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( 4 ); }, 25 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( 2 ); }, 100 );

	button->click( 0, 0 );

	ASSERT_EQ( order.size(), 4u );
	EXPECT_EQ( order[ 0 ], 1 );  // priority 200
	EXPECT_EQ( order[ 1 ], 2 );  // priority 100
	EXPECT_EQ( order[ 2 ], 3 );  // priority 50
	EXPECT_EQ( order[ 3 ], 4 );  // priority 25
}

TEST( AdvancedConnections, PriorityWithDynamicConnections )
{
	auto button = std::make_shared< TestButton >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::vector< std::string > order;

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Low" ); }, -10 );

	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "High" ); }, 10 );

	button->click( 0, 0 );
	ASSERT_EQ( order.size(), 2u );
	EXPECT_EQ( order[ 0 ], "High" );
	EXPECT_EQ( order[ 1 ], "Low" );

	order.clear();
	button->clicked.connectWithPriority( receiver, [ &order ]( int, int ) { order.push_back( "Medium" ); }, 0 );

	button->click( 0, 0 );
	ASSERT_EQ( order.size(), 3u );
	EXPECT_EQ( order[ 0 ], "High" );
	EXPECT_EQ( order[ 1 ], "Medium" );
	EXPECT_EQ( order[ 2 ], "Low" );
}
