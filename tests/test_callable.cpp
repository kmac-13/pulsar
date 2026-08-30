#include "test_helpers.hpp"

#include "kmac/pulsar/callable.h"

#include <type_traits>

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

static int freeFnCalls = 0;
static int freeFunction( int x )
{
	++freeFnCalls;
	return x * 2;
}

static int lastA = 0;
static int lastB = 0;
static int lastC = 0;
static void freeThree( int a, int b, int c )
{
	lastA = a;
	lastB = b;
	lastC = c;
}
static void freeOne( int a )
{
	lastA = a;
	lastB = 0;
	lastC = 0;
}
static void freeZero()
{
	lastA = lastB = lastC = -1;
}

struct Widget
{
	int value = 0;
	int lastA = 0;
	int lastB = 0;
	int lastC = 0;

	int onEvent( int x )
	{
		value += x;
		return value;
	}

	void onThree( int a, int b, int c )
	{
		lastA = a;
		lastB = b;
		lastC = c;
	}

	void onOne( int a )
	{
		lastA = a;
		lastB = 0;
		lastC = 0;
	}

	void onZero()
	{
		lastA = lastB = lastC = -1;
	}
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST( Callable, FreeFunction )
{
	freeFnCalls = 0;
	auto c = pulsar::Callable< int( int ) >::create< &freeFunction >();
	EXPECT_TRUE( c );
	EXPECT_EQ( c( 5 ), 10 );
	EXPECT_EQ( freeFnCalls, 1 );
}

TEST( Callable, MethodPointer )
{
	Widget w;
	auto c = pulsar::Callable< int( int ) >::create< &Widget::onEvent >( &w );
	EXPECT_TRUE( c );
	EXPECT_EQ( c( 3 ), 3 );
	EXPECT_EQ( c( 4 ), 7 );
	EXPECT_EQ( w.value, 7 );
	EXPECT_TRUE( c.isOwner( &w ) );
}

TEST( Callable, NonCapturingLambda )
{
	auto lambda = []( int x ) { return x + 100; };
	auto c = pulsar::Callable< int( int ) >::create( lambda );
	EXPECT_TRUE( c );
	EXPECT_EQ( c( 1 ), 101 );
}

TEST( Callable, CapturingLambdaOwning )
{
	int captured = 42;
	auto c = pulsar::Callable< int( int ) >::create( [ captured ]( int x ) { return x + captured; } );
	EXPECT_TRUE( c );
	EXPECT_EQ( c( 8 ), 50 );
}

TEST( Callable, MoveConstruction )
{
	int captured = 7;
	auto c1 = pulsar::Callable< int( int ) >::create( [ captured ]( int x ) { return x * captured; } );
	auto c2 = std::move( c1 );
	EXPECT_FALSE( c1 );
	EXPECT_TRUE( c2 );
	EXPECT_EQ( c2( 3 ), 21 );
}

TEST( Callable, MoveAssignmentFreesPriorOwned )
{
	static int destructions = 0;
	destructions = 0;

	struct Tracked
	{
		~Tracked() { ++destructions; }
		int operator()( int x ) const { return x; }
	};

	auto c = pulsar::Callable< int( int ) >::create( Tracked{} );
	// temporary Tracked{} destroyed at end of create() full-expression
	EXPECT_EQ( destructions, 1 );

	// overwrite - should destroy the heap-owned copy
	c = pulsar::Callable< int( int ) >::create< &freeFunction >();
	EXPECT_EQ( destructions, 2 );
}

TEST( Callable, DefaultConstructedIsEmpty )
{
	pulsar::Callable< int( int ) > c;
	EXPECT_FALSE( c );
	EXPECT_FALSE( static_cast< bool >( c ) );
}

TEST( Callable, Equality )
{
	Widget w1, w2;
	auto c1a = pulsar::Callable< int( int ) >::create< &Widget::onEvent >( &w1 );
	auto c1b = pulsar::Callable< int( int ) >::create< &Widget::onEvent >( &w1 );
	auto c2 = pulsar::Callable< int( int ) >::create< &Widget::onEvent >( &w2 );

	EXPECT_EQ( c1a, c1b );
	EXPECT_NE( c1a, c2 );
}

TEST( Callable, MoveOnlyStaticAsserts )
{
	using C = pulsar::Callable< void() >;
	static_assert( ! std::is_copy_constructible< C >::value, "must not be copy-constructible" );
	static_assert( ! std::is_copy_assignable< C >::value, "must not be copy-assignable" );
	static_assert( std::is_move_constructible< C >::value, "must be move-constructible" );
	static_assert( std::is_move_assignable< C >::value, "must be move-assignable" );
}

TEST( Callable, DestructorFreesOwnedFunctor )
{
	static int destructions = 0;
	destructions = 0;

	struct Tracked2
	{
		~Tracked2() { ++destructions; }
		void operator()() const {}
	};

	{
		auto c = pulsar::Callable< void() >::create( Tracked2{} );
		(void)c;
		EXPECT_EQ( destructions, 1 );  // temporary destroyed after create()
	}
	EXPECT_EQ( destructions, 2 );  // heap copy freed on scope exit
}

TEST( Callable, CreatePartialMemberFull )
{
	Widget w;
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &Widget::onThree >( &w );
	c( 1, 2, 3 );
	EXPECT_EQ( w.lastA, 1 );
	EXPECT_EQ( w.lastB, 2 );
	EXPECT_EQ( w.lastC, 3 );
}

TEST( Callable, CreatePartialMemberDropTrailing )
{
	Widget w;
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &Widget::onOne >( &w );
	c( 10, 20, 30 );
	EXPECT_EQ( w.lastA, 10 );
	EXPECT_EQ( w.lastB, 0 );
	EXPECT_EQ( w.lastC, 0 );
}

TEST( Callable, CreatePartialMemberDropAll )
{
	Widget w;
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &Widget::onZero >( &w );
	c( 100, 200, 300 );
	EXPECT_EQ( w.lastA, -1 );
	EXPECT_EQ( w.lastB, -1 );
	EXPECT_EQ( w.lastC, -1 );
}

TEST( Callable, CreatePartialMoveAndIsOwner )
{
	Widget w;
	auto c1 = pulsar::Callable< void( int, int, int ) >::createPartial< &Widget::onOne >( &w );
	EXPECT_TRUE( c1.isOwner( &w ) );

	auto c2 = std::move( c1 );
	EXPECT_FALSE( c1 );
	EXPECT_TRUE( c2.isOwner( &w ) );
	c2( 7, 8, 9 );
	EXPECT_EQ( w.lastA, 7 );
}

TEST( Callable, CreatePartialFreeFunctionFull )
{
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &freeThree >();
	c( 1, 2, 3 );
	EXPECT_EQ( lastA, 1 );
	EXPECT_EQ( lastB, 2 );
	EXPECT_EQ( lastC, 3 );
}

TEST( Callable, CreatePartialFreeFunctionDropTrailing )
{
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &freeOne >();
	c( 10, 20, 30 );
	EXPECT_EQ( lastA, 10 );
	EXPECT_EQ( lastB, 0 );
	EXPECT_EQ( lastC, 0 );
}

TEST( Callable, CreatePartialFreeFunctionDropAll )
{
	auto c = pulsar::Callable< void( int, int, int ) >::createPartial< &freeZero >();
	c( 100, 200, 300 );
	EXPECT_EQ( lastA, -1 );
	EXPECT_EQ( lastB, -1 );
	EXPECT_EQ( lastC, -1 );
}

TEST( Callable, CreatePartialFreeFunctionNonVoidReturn )
{
	freeFnCalls = 0;
	// freeFunction takes 1 arg; called via Callable<int(int,int)>, second arg dropped
	auto c = pulsar::Callable< int( int, int ) >::createPartial< &freeFunction >();
	int r = c( 21, 999 );
	EXPECT_EQ( r, 42 );
}
