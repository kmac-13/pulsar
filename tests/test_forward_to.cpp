#include "test_helpers.h"

#include <kmac/stellyra/event_loop.h>

// ---------------------------------------------------------------------------
// forwardTo
//
// Event-chaining convenience: connect one event so that triggering it
// triggers another event.  Routes through the same partial-arg machinery
// connect<Method> already uses (Callable::createPartial targeting the
// target event's own operator()), so the target's parameter list must be
// the first-N prefix of the source's.
//
// Most of the argument-adaptation behaviour is already covered by
// test_partial_args.cpp (createPartial is the same machinery underneath);
// this file focuses on the surface forwardTo actually adds: auto-Trackable
// pickup from the target, cross-loop composition, and disconnect.
//
//   Event<int, std::string> mixedArgs;
//   Event<std::string> strOnly;
//   mixedArgs.forwardTo( strOnly );  // fails: string vs int at position 0
//
//   Event<int> oneArg;
//   Event<int, int> twoArgs;
//   oneArg.forwardTo( twoArgs );     // fails: createPartial arity static_assert
//
// Widget/LoopedSource/LoopedTarget each hold events of more than one arity,
// so - unlike files with a single Args signature - parity across MutexType
// is parametrized on the MutexType itself (not a fixed Event alias), with
// each fixture rebuilding its events as BasicEvent<MutexType, Args...>.
// No test here spawns real threads or uses once(), so all three MutexType
// variants (RecursiveMutex/Event, SharedMutex/SharedEvent, NullMutex/
// SingleThreadedEvent) are safe.
// ---------------------------------------------------------------------------

namespace {

template< typename MutexType >
class Widget : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int, int > moveEv { this };  // (x, y)
	stellyra::BasicEvent< MutexType, int > xChanged { this };     // (x)
	stellyra::BasicEvent< MutexType > moved { this };             // ()
};

template< typename MutexType >
struct LoopedSource : public stellyra::Trackable
{
	stellyra::EventLoop loop;
	stellyra::BasicEvent< MutexType, int > xChanged { this };
	LoopedSource() { setEventLoop( &loop ); }
};

template< typename MutexType >
struct LoopedTarget : public stellyra::Trackable
{
	stellyra::EventLoop loop;
	stellyra::BasicEvent< MutexType > moved { this };
	LoopedTarget() { setEventLoop( &loop ); }
};

} // namespace

template< typename MutexType >
class ForwardTo : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( ForwardTo, MutexTypes );

// ---------------------------------------------------------------------------
// Basic forwarding - partial and exact arity
// ---------------------------------------------------------------------------

TYPED_TEST( ForwardTo, DropsAllArgs )
{
	Widget< TypeParam > w;
	int calls = 0;
	w.moved.connectLambda( w, [ & ]() { calls++; } );

	w.xChanged.forwardTo( w.moved );
	w.xChanged( 5 );

	EXPECT_EQ( calls, 1 );
}

TYPED_TEST( ForwardTo, DropsTrailingArgFromTwoArgSource )
{
	Widget< TypeParam > w;
	int calls = 0;
	w.moved.connectLambda( w, [ & ]() { calls++; } );

	w.moveEv.forwardTo( w.moved );
	w.moveEv( 1, 2 );

	EXPECT_EQ( calls, 1 );
}

TYPED_TEST( ForwardTo, KeepsFirstNPrefix )
{
	Widget< TypeParam > w;
	stellyra::BasicEvent< TypeParam, int > firstOnly { &w };
	int lastX = -1;
	firstOnly.connectLambda( w, [ & ]( int x ) { lastX = x; } );

	w.moveEv.forwardTo( firstOnly );
	w.moveEv( 7, 9 );

	EXPECT_EQ( lastX, 7 );
}

TYPED_TEST( ForwardTo, ExactArityForwardsDirectly )
{
	Widget< TypeParam > w;
	stellyra::BasicEvent< TypeParam, int > mirror { &w };
	int last = -1;
	mirror.connectLambda( w, [ & ]( int x ) { last = x; } );

	w.xChanged.forwardTo( mirror );
	w.xChanged( 99 );

	EXPECT_EQ( last, 99 );
}

TYPED_TEST( ForwardTo, MultipleTriggersForwardEachTime )
{
	Widget< TypeParam > w;
	int calls = 0;
	w.moved.connectLambda( w, [ & ]() { calls++; } );

	w.xChanged.forwardTo( w.moved );
	w.xChanged( 1 );
	w.xChanged( 2 );
	w.xChanged( 3 );

	EXPECT_EQ( calls, 3 );
}

// ---------------------------------------------------------------------------
// Auto-Trackable pickup and lifetime
// ---------------------------------------------------------------------------

TYPED_TEST( ForwardTo, TrackerIsAutoSourcedFromTargetOwner )
{
	// no tracker is passed explicitly - forwardTo pulls it from the
	// target's own owner (w.moved.owner() == &w)
	Widget< TypeParam > w;
	int calls = 0;
	w.moved.connectLambda( w, [ & ]() { calls++; } );

	stellyra::Connection c = w.xChanged.forwardTo( w.moved );
	EXPECT_TRUE( c.isConnected() );
}

TYPED_TEST( ForwardTo, DestroyingTargetsOwnerSeversTheForward )
{
	Widget< TypeParam > source;
	{
		Widget< TypeParam > target;
		source.xChanged.forwardTo( target.moved );

		int calls = 0;
		target.moved.connectLambda( target, [ & ]() { calls++; } );
		source.xChanged( 1 );
		EXPECT_EQ( calls, 1 );
	}
	// target destroyed here - Trackable::disconnectAll() should sever
	// the forwarding connection on source.xChanged

	// must not dangle-trigger into the destroyed target
	source.xChanged( 2 );
}

TYPED_TEST( ForwardTo, UntrackedWhenTargetHasNoOwner )
{
	// a target with no owner Trackable still forwards, just without automatic
	// lifetime binding - same trust model as connectFree() with no tracker
	Widget< TypeParam > w;
	stellyra::BasicEvent< TypeParam > orphanTarget;  // no owner
	int calls = 0;
	orphanTarget.connectLambda( [ & ]() { calls++; } );

	w.xChanged.forwardTo( orphanTarget );
	w.xChanged( 1 );

	EXPECT_EQ( calls, 1 );
}

// ---------------------------------------------------------------------------
// Returned Connection
// ---------------------------------------------------------------------------

TYPED_TEST( ForwardTo, ReturnedConnectionExplicitlyDisconnects )
{
	Widget< TypeParam > w;
	int calls = 0;
	w.moved.connectLambda( w, [ & ]() { calls++; } );

	stellyra::Connection c = w.xChanged.forwardTo( w.moved );
	w.xChanged( 1 );
	EXPECT_EQ( calls, 1 );

	c.disconnect();
	w.xChanged( 2 );
	EXPECT_EQ( calls, 1 );
}

// ---------------------------------------------------------------------------
// Cross-loop composition - the new surface forwardTo introduces
//
// Source and target are owned by different EventLoops.  The forwarding
// connection itself resolves Deferred (source's owner loop != target's
// owner loop), so it lands on target's loop; target's own trigger then
// follows target's own thread affinity for its downstream handlers, not
// source's - verified here by checking the downstream handler only fires
// once target's loop is drained, never source's.
// ---------------------------------------------------------------------------

TYPED_TEST( ForwardTo, CrossLoopForwardLandsInTargetsContext )
{
	LoopedSource< TypeParam > src;
	LoopedTarget< TypeParam > tgt;

	int calls = 0;
	tgt.moved.connectLambda( tgt, [ & ]() { calls++; } );

	src.xChanged.forwardTo( tgt.moved );

	src.xChanged( 1 );      // sender-side deferral: lands in src.loop first
	src.loop.drain();       // forwarding connection resolves + posts to tgt.loop
	EXPECT_EQ( calls, 0 );  // not yet delivered to target's context

	tgt.loop.drain();       // target's own trigger runs here, dispatching Direct
	EXPECT_EQ( calls, 1 );
}

TYPED_TEST( ForwardTo, CrossLoopForwardDoesNotFireOnSourcesLoop )
{
	LoopedSource< TypeParam > src;
	LoopedTarget< TypeParam > tgt;

	int calls = 0;
	tgt.moved.connectLambda( tgt, [ & ]() { calls++; } );

	src.xChanged.forwardTo( tgt.moved );

	src.xChanged( 1 );
	src.loop.drain();

	// draining src.loop again must not re-deliver - the handler only ever
	// runs on tgt.loop
	src.loop.drain();
	EXPECT_EQ( calls, 0 );
}
