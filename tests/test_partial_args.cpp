#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Partial argument matching tests
//
// Verifies that handler methods accepting fewer arguments than the event
// provides compile and receive the correct leading arguments.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class Source : public pulsar::Object
{
public:
	pulsar::Event< int > oneArg;
	pulsar::Event< int, int > twoArgs;
	pulsar::Event< int, int, int > threeArgs;
	pulsar::Event< int, std::string > mixedArgs;

	explicit Source()
		: oneArg( this )
		, twoArgs( this )
		, threeArgs( this )
		, mixedArgs( this )
	{
	}
};

class Sink : public pulsar::Object
{
public:
	int callCount = 0;
	int lastA = -1;
	int lastB = -1;
	std::string lastStr;

	void onZero() { callCount++; }
	void onOne( int a ) { callCount++; lastA = a; }
	void onTwo( int a, int b ) { callCount++; lastA = a; lastB = b; }
	void onStr( const std::string& s ) { callCount++; lastStr = s; }

	void reset() { callCount = 0; lastA = -1; lastB = -1; lastStr.clear(); }
};

// ---------------------------------------------------------------------------
// Exact match still works (regression)
// ---------------------------------------------------------------------------

TEST( PartialArgs, ExactMatchOneArg )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->oneArg.connect( sink, &Sink::onOne );
	src->oneArg( 42 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 42 );
}

TEST( PartialArgs, ExactMatchTwoArgs )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->twoArgs.connect( sink, &Sink::onTwo );
	src->twoArgs( 3, 7 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 3 );
	EXPECT_EQ( sink->lastB, 7 );
}

// ---------------------------------------------------------------------------
// Drop trailing args
// ---------------------------------------------------------------------------

TEST( PartialArgs, TwoArgEventOneArgHandler )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	// Event fires (int, int), handler takes (int) - second arg dropped
	src->twoArgs.connect( sink, &Sink::onOne );
	src->twoArgs( 10, 20 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 10 );  // first arg received
}

TEST( PartialArgs, ThreeArgEventOneArgHandler )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->threeArgs.connect( sink, &Sink::onOne );
	src->threeArgs( 5, 6, 7 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 5 );
}

TEST( PartialArgs, ThreeArgEventTwoArgHandler )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->threeArgs.connect( sink, &Sink::onTwo );
	src->threeArgs( 11, 22, 33 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 11 );
	EXPECT_EQ( sink->lastB, 22 );
}

// ---------------------------------------------------------------------------
// Drop all args
// ---------------------------------------------------------------------------

TEST( PartialArgs, TwoArgEventZeroArgHandler )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->twoArgs.connect( sink, &Sink::onZero );
	src->twoArgs( 99, 99 );

	EXPECT_EQ( sink->callCount, 1 );
}

TEST( PartialArgs, ThreeArgEventZeroArgHandler )
{
	auto src  = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->threeArgs.connect( sink, &Sink::onZero );
	src->threeArgs( 1, 2, 3 );

	EXPECT_EQ( sink->callCount, 1 );
}

// ---------------------------------------------------------------------------
// Mixed arg types - take only the first (string) arg from (int, string) event
// ---------------------------------------------------------------------------

TEST( PartialArgs, MixedArgEventFirstArgOnly )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	// Event<int, string>, handler takes (string) would be the second arg -
	// instead test handler taking just the first arg (int)
	src->mixedArgs.connect( sink, &Sink::onOne );
	src->mixedArgs( 77, std::string( "hello" ) );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 77 );
}

// ---------------------------------------------------------------------------
// Multiple emissions still work correctly
// ---------------------------------------------------------------------------

TEST( PartialArgs, MultipleEmissionsPartialHandler )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->twoArgs.connect( sink, &Sink::onOne );

	src->twoArgs( 1, 100 );
	src->twoArgs( 2, 200 );
	src->twoArgs( 3, 300 );

	EXPECT_EQ( sink->callCount, 3 );
	EXPECT_EQ( sink->lastA, 3 );  // last emission
}

// ---------------------------------------------------------------------------
// Partial matching works via connectOnce
// ---------------------------------------------------------------------------

TEST( PartialArgs, ConnectOncePartialArgs )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->twoArgs.connectOnce( sink, &Sink::onOne );
	src->twoArgs( 55, 66 );
	src->twoArgs( 77, 88 );  // should not fire - already disconnected

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 55 );
}

// ---------------------------------------------------------------------------
// Partial matching works via connectWithPriority
// ---------------------------------------------------------------------------

TEST( PartialArgs, ConnectWithPriorityPartialArgs )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->twoArgs.connectWithPriority( sink, &Sink::onOne, 0 );
	src->twoArgs( 44, 55 );

	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 44 );
}

// ---------------------------------------------------------------------------
// Partial matching works via connectIf
// ---------------------------------------------------------------------------

TEST( PartialArgs, ConnectIfPartialArgs )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	// condition receives full args; handler receives only first
	src->twoArgs.connectIf(
		sink,
		&Sink::onOne,
		[]( int a, int ) { return a > 5; } );

	src->twoArgs( 3, 10 );   // condition false - skip
	EXPECT_EQ( sink->callCount, 0 );

	src->twoArgs( 10, 20 );  // condition true
	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastA, 10 );
}

// ---------------------------------------------------------------------------
// Partial matching works via ReceiverLifetimeAnchor
// ---------------------------------------------------------------------------

TEST( PartialArgs, RLAPartialArgs )
{
	class PlainSink
	{
	public:
		int callCount = 0;
		int lastA = -1;

		void onOne( int a ) { callCount++; lastA = a; }

		pulsar::ReceiverLifetimeAnchor< PlainSink > pulsarAnchor { this };
	};

	auto src = std::make_shared< Source >();
	PlainSink sink;

	src->twoArgs.connect( sink.pulsarAnchor, &PlainSink::onOne );
	src->twoArgs( 7, 8 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 7 );
}
