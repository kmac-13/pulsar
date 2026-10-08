#include "test_helpers.h"

// ---------------------------------------------------------------------------
// Partial argument matching tests
//
// Verifies that handler methods/functions accepting fewer arguments than an
// event provides still compile and receive the correct leading arguments.
// Partial matching is supported by the NTTP (non-type template parameter)
// forms - connect<Method> and connectFree<Func> - and by the runtime
// (non-NTTP) pointer-to-member-function form, connect(receiver, method).
// ConnParams (.once(), {predicate}/.when(), .prio()) composes with partial
// matching on these same forms rather than being separate connect method
// names.  disconnect/disconnectFree reconstruct the same partial-or-exact
// target that the matching connect call built, so a partial-arity
// connection can be disconnected by name.
//
// Source holds four events of different arities, so parametrization is on
// MutexType directly (BasicEvent<MutexType, Args...>) rather than a fixed
// Event alias.  ConnectOncePartialArgs uses once(), which static_asserts
// against SharedMutex, so it's split into its own suite restricted to
// Event/SingleThreadedEvent; everything else is safe across all three.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

namespace {

template< typename MutexType >
class Source : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int > oneArg { this };
	stellyra::BasicEvent< MutexType, int, int > twoArgs { this };
	stellyra::BasicEvent< MutexType, int, int, int > threeArgs { this };
	stellyra::BasicEvent< MutexType, int, std::string > mixedArgs { this };
};

class Sink : public stellyra::Trackable
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

} // namespace

template< typename MutexType >
class PartialArgs : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( PartialArgs, MutexTypes );

// ---------------------------------------------------------------------------
// Exact match via runtime (non-NTTP) connect()
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, ExactMatchOneArg )
{
	Source< TypeParam > src;
	Sink sink;

	src.oneArg.connect( sink, &Sink::onOne );
	src.oneArg( 42 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 42 );
}

TYPED_TEST( PartialArgs, ExactMatchTwoArgs )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.connect( sink, &Sink::onTwo );
	src.twoArgs( 3, 7 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 3 );
	EXPECT_EQ( sink.lastB, 7 );
}

// ---------------------------------------------------------------------------
// Partial arg (non-NTTP) connect()
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, TwoArgEventOneArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.connect( sink, &Sink::onOne );
	src.twoArgs( 10, 20 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 10 );
}

TYPED_TEST( PartialArgs, ThreeArgEventOneArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.connect( sink, &Sink::onOne );
	src.threeArgs( 5, 6, 7 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 5 );
}

TYPED_TEST( PartialArgs, ThreeArgEventTwoArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.connect( sink, &Sink::onTwo );
	src.threeArgs( 11, 22, 33 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 11 );
	EXPECT_EQ( sink.lastB, 22 );
}

TYPED_TEST( PartialArgs, TwoArgEventZeroArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.connect( sink, &Sink::onZero );
	src.twoArgs( 99, 99 );

	EXPECT_EQ( sink.callCount, 1 );
}

TYPED_TEST( PartialArgs, ThreeArgEventZeroArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.connect( sink, &Sink::onZero );
	src.threeArgs( 1, 2, 3 );

	EXPECT_EQ( sink.callCount, 1 );
}

TYPED_TEST( PartialArgs, MixedArgEventFirstArgOnly )
{
	Source< TypeParam > src;
	Sink sink;

	src.mixedArgs.connect( sink, &Sink::onOne );
	src.mixedArgs( 77, std::string( "hello" ) );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 77 );
}

TYPED_TEST( PartialArgs, MultipleEmissionsPartialHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.connect( sink, &Sink::onOne );

	src.twoArgs( 1, 100 );
	src.twoArgs( 2, 200 );
	src.twoArgs( 3, 300 );

	EXPECT_EQ( sink.callCount, 3 );
	EXPECT_EQ( sink.lastA, 3 );
}

// ---------------------------------------------------------------------------
// Partial matching combined with an explicit priority
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, ConnectWithPriorityPartialArgs )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.template connect< &Sink::onOne >( sink, 0u );
	src.twoArgs( 44, 55 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 44 );
}

// ---------------------------------------------------------------------------
// Partial matching via connect() with predicate - Pred and Handler are
// matched independently, so a partial-arity handler can pair with a
// full-arity predicate.
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, ConnectPartialArgsWithPredicate )
{
	class PredSink : public Sink
	{
	public:
		bool checkFirstGreaterThan5( int a, int ) { return a > 5; }
	};

	Source< TypeParam > src;
	PredSink sink;

	src.twoArgs.template connect< &PredSink::onOne >(
		sink,
		{ [ &sink ]( int a, int b ) { return sink.checkFirstGreaterThan5( a, b ); } } );

	src.twoArgs( 3, 10 );   // condition false - skip
	EXPECT_EQ( sink.callCount, 0 );

	src.twoArgs( 10, 20 );  // condition true
	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 10 );
}

// ---------------------------------------------------------------------------
// Partial matching with a non-Trackable receiver bundled via Tracked<T>
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, AnchoredReceiverPartialArgs )
{
	class PlainSink
	{
	public:
		int callCount = 0;
		int lastA = -1;

		void onOne( int a ) { callCount++; lastA = a; }

		stellyra::Anchor anchor;
	};

	Source< TypeParam > src;
	PlainSink sink;

	src.twoArgs.template connect< &PlainSink::onOne >( { sink, sink.anchor } );
	src.twoArgs( 7, 8 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 7 );
}

// ---------------------------------------------------------------------------
// Partial matching via the NTTP connect<&Method>(receiver) syntax
// ---------------------------------------------------------------------------

TYPED_TEST( PartialArgs, NTTPExactMatchOneArg )
{
	Source< TypeParam > src;
	Sink sink;

	src.oneArg.template connect< &Sink::onOne >( sink );
	src.oneArg( 42 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 42 );
}

TYPED_TEST( PartialArgs, NTTPExactMatchTwoArgs )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.template connect< &Sink::onTwo >( sink );
	src.twoArgs( 3, 7 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 3 );
	EXPECT_EQ( sink.lastB, 7 );
}

TYPED_TEST( PartialArgs, NTTPTwoArgEventOneArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	// event fires (int, int), handler takes (int) - second arg dropped
	src.twoArgs.template connect< &Sink::onOne >( sink );
	src.twoArgs( 10, 20 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 10 );
}

TYPED_TEST( PartialArgs, NTTPThreeArgEventOneArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.template connect< &Sink::onOne >( sink );
	src.threeArgs( 5, 6, 7 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 5 );
}

TYPED_TEST( PartialArgs, NTTPThreeArgEventTwoArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.template connect< &Sink::onTwo >( sink );
	src.threeArgs( 11, 22, 33 );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 11 );
	EXPECT_EQ( sink.lastB, 22 );
}

TYPED_TEST( PartialArgs, NTTPTwoArgEventZeroArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	// drop all args - handler takes no arguments at all
	src.twoArgs.template connect< &Sink::onZero >( sink );
	src.twoArgs( 99, 99 );

	EXPECT_EQ( sink.callCount, 1 );
}

TYPED_TEST( PartialArgs, NTTPThreeArgEventZeroArgHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.threeArgs.template connect< &Sink::onZero >( sink );
	src.threeArgs( 1, 2, 3 );

	EXPECT_EQ( sink.callCount, 1 );
}

TYPED_TEST( PartialArgs, NTTPMixedArgEventFirstArgOnly )
{
	Source< TypeParam > src;
	Sink sink;

	src.mixedArgs.template connect< &Sink::onOne >( sink );
	src.mixedArgs( 77, std::string( "hello" ) );

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 77 );
}

TYPED_TEST( PartialArgs, NTTPMultipleEmissionsPartialHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.template connect< &Sink::onOne >( sink );

	src.twoArgs( 1, 100 );
	src.twoArgs( 2, 200 );
	src.twoArgs( 3, 300 );

	EXPECT_EQ( sink.callCount, 3 );
	EXPECT_EQ( sink.lastA, 3 );  // last emission
}

TYPED_TEST( PartialArgs, NTTPDisconnectPartialHandler )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.template connect< &Sink::onOne >( sink );
	src.twoArgs( 1, 2 );
	EXPECT_EQ( sink.callCount, 1 );

	src.twoArgs.template disconnect< &Sink::onOne >( sink );
	src.twoArgs( 3, 4 );

	EXPECT_EQ( sink.callCount, 1 );  // unchanged - disconnected
}

// ---------------------------------------------------------------------------
// Partial matching via the NTTP connectFree<&Func>() syntax
// ---------------------------------------------------------------------------

namespace
{
	int freeCallCount = 0;
	int freeLastA = -1;
	int freeLastB = -1;

	void resetFreeCounters()
	{
		freeCallCount = 0;
		freeLastA = -1;
		freeLastB = -1;
	}

	void onZeroFree() { freeCallCount++; }
	void onOneFree( int a ) { freeCallCount++; freeLastA = a; }
	void onTwoFree( int a, int b ) { freeCallCount++; freeLastA = a; freeLastB = b; }
}

TYPED_TEST( PartialArgs, NTTPFreeFunctionExactMatch )
{
	Source< TypeParam > src;
	resetFreeCounters();

	src.twoArgs.template connectFree< &onTwoFree >();
	src.twoArgs( 3, 7 );

	EXPECT_EQ( freeCallCount, 1 );
	EXPECT_EQ( freeLastA, 3 );
	EXPECT_EQ( freeLastB, 7 );
}

TYPED_TEST( PartialArgs, NTTPFreeFunctionDropTrailingArgs )
{
	Source< TypeParam > src;
	resetFreeCounters();

	// event fires (int, int, int), function takes (int) - trailing args dropped
	src.threeArgs.template connectFree< &onOneFree >();
	src.threeArgs( 5, 6, 7 );

	EXPECT_EQ( freeCallCount, 1 );
	EXPECT_EQ( freeLastA, 5 );
}

TYPED_TEST( PartialArgs, NTTPFreeFunctionDropAllArgs )
{
	Source< TypeParam > src;
	resetFreeCounters();

	src.twoArgs.template connectFree< &onZeroFree >();
	src.twoArgs( 99, 99 );

	EXPECT_EQ( freeCallCount, 1 );
}

TYPED_TEST( PartialArgs, NTTPFreeFunctionDisconnectPartialHandler )
{
	Source< TypeParam > src;
	resetFreeCounters();

	src.twoArgs.template connectFree< &onOneFree >();
	src.twoArgs( 1, 2 );
	EXPECT_EQ( freeCallCount, 1 );

	src.twoArgs.template disconnectFree< &onOneFree >();
	src.twoArgs( 3, 4 );

	EXPECT_EQ( freeCallCount, 1 );  // unchanged - disconnected
}

// ---------------------------------------------------------------------------
// Partial matching via connect() with Once() - once() static_asserts against
// SharedMutex, so this is its own suite restricted to Event/SingleThreadedEvent.
// ---------------------------------------------------------------------------

template< typename MutexType >
class PartialArgsOnce : public ::testing::Test {};

using OnceCapableMutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( PartialArgsOnce, OnceCapableMutexTypes );

TYPED_TEST( PartialArgsOnce, ConnectOncePartialArgs )
{
	Source< TypeParam > src;
	Sink sink;

	src.twoArgs.template connect< &Sink::onOne >( sink, src.twoArgs.params().once() );
	src.twoArgs( 55, 66 );
	src.twoArgs( 77, 88 );  // should not fire - already disconnected

	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastA, 55 );
}
