#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Combining Events and Connection Groups
// ---------------------------------------------------------------------------

TEST( CombiningAndGroups, CombiningEventLogicalAnd )
{
	class TestObject : public pulsar::Object
	{
	public:
		pulsar::CombiningEvent< bool, pulsar::Combiners::LogicalAnd<>, int > validate{ this };
	};

	auto obj = std::make_shared< TestObject >();
	auto receiver = std::make_shared< pulsar::Object >();

	obj->validate.connect( receiver, []( int value ) -> bool { return value > 0; } );
	obj->validate.connect( receiver, []( int value ) -> bool { return value < 100; } );

	EXPECT_TRUE( obj->validate.emit( 10  ) );   // positive AND < 100
	EXPECT_FALSE( obj->validate.emit( -5  ) );  // not positive
	EXPECT_FALSE( obj->validate.emit( 150 ) );  // not < 100
}

TEST( CombiningAndGroups, CombiningEventLogicalAndWithCheckers )
{
	auto checker1 = std::make_shared< Checker >();
	auto checker2 = std::make_shared< Checker >();

	pulsar::CombiningEvent< bool, pulsar::Combiners::LogicalAnd<>, std::string > validate{ nullptr };

	validate.connect( checker1, &Checker::check );
	validate.connect( checker2, &Checker::check );

	checker1->expectedResult = true;
	checker2->expectedResult = true;
	EXPECT_TRUE( validate.emit( "test" ) );

	checker1->expectedResult = true;
	checker2->expectedResult = false;
	EXPECT_FALSE( validate.emit( "test" ) );

	checker1->expectedResult = false;
	checker2->expectedResult = false;
	EXPECT_FALSE( validate.emit( "test" ) );
}

TEST( CombiningAndGroups, CombiningEventLogicalOr )
{
	class TestObject : public pulsar::Object
	{
	public:
		pulsar::CombiningEvent< bool, pulsar::Combiners::LogicalOr<>, std::string > validate{ this };
	};

	auto obj = std::make_shared< TestObject >();
	auto checker1 = std::make_shared< Checker >();
	auto checker2 = std::make_shared< Checker >();

	obj->validate.connect( checker1, &Checker::check );
	obj->validate.connect( checker2, &Checker::check );

	checker1->expectedResult = true;
	checker2->expectedResult = false;
	EXPECT_TRUE( obj->validate.emit( "test" ) );

	checker1->expectedResult = false;
	checker2->expectedResult = false;
	EXPECT_FALSE( obj->validate.emit( "test" ) );
}

TEST( CombiningAndGroups, ScopedConnection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	{
		auto scoped = button->clicked.connect( handler, &TestHandler::onClicked ).scoped();

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );

		// scoped destroyed here - auto-disconnects
	}

	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );
}

TEST( CombiningAndGroups, ScopedConnectionAutoDisconnect )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	{
		auto scoped = button->clicked.connect( handler, &TestHandler::onClicked ).scoped();

		button->click( 1, 1 );
		EXPECT_EQ( handler->callCount, 1 );
		EXPECT_TRUE( scoped.isConnected() );
	}

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 0 );
}

TEST( CombiningAndGroups, ConnectionGroups )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

	EXPECT_EQ( group.size(), 3u );
	EXPECT_EQ( group.activeCount(), 3u );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 3 );

	group.disconnectAll();
	EXPECT_EQ( group.size(), 0u );

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 0 );
}

TEST( CombiningAndGroups, ConnectionGroupBlocking )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	pulsar::ConnectionGroup group;
	group += button->clicked.connect( handler, &TestHandler::onClicked );
	group += button->clicked.connect( handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );

	handler->reset();
	group.blockAll();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 0 );

	group.unblockAll();
	button->click( 3, 3 );
	EXPECT_EQ( handler->callCount, 2 );
}
