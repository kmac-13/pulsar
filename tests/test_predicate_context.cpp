#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>

// ---------------------------------------------------------------------------
// PredicateContext tests
//
// Verifies the distinction between PredicateContext::Sender (evaluated at
// emission time in triggerImpl before any queuing) and
// PredicateContext::Receiver (default, evaluated at invocation time).
//
// Predicates always require exact-arity matching against the event's
// Args... - there is no partial-argument-matching path for predicates the
// way connect<&Method>/connectFree<&Func> provide for handlers (see
// ConnParams::PredicateType in conn_params.h).  PredicateOnPartialArgHandler
// below combines a genuinely partial-arg-matched handler with a full-arity
// predicate; there is no equivalent "partial-arg predicate" to test, since
// that capability doesn't exist - write a full-arity predicate and ignore
// whichever parameters you don't need.
//
// No test here does reentrant connect/disconnect from within its own
// dispatch, and no test spawns real threads, so all three MutexType
// variants (Event, SharedEvent, SingleThreadedEvent) are safe.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

namespace {

template< typename EventT >
class Source : public pulsar::Trackable
{
public:
	EventT valueChanged { this };
};

class Sink : public pulsar::Trackable
{
public:
	int callCount = 0;
	int lastValue = -1;

	void onValue( int v ) { callCount++; lastValue = v; }
	void reset() { callCount = 0; lastValue = -1; }
};

} // namespace

template< typename EventT >
class PredicateContext : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SharedEvent< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( PredicateContext, EventTypes );

// ---------------------------------------------------------------------------
// PredicateContext::Receiver - explicit
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, ReceiverContextPassingPredicate )
{
	Source< TypeParam > src;
	Sink sink;

	src.valueChanged.connectLambda(
		sink,
		[ &sink ]( int v ) { sink.onValue( v ); },
		{ []( int v ) { return v > 5; }, pulsar::PredicateContext::Receiver } );

	src.valueChanged( 3 );
	EXPECT_EQ( sink.callCount, 0 );

	src.valueChanged( 10 );
	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastValue, 10 );
}

// ---------------------------------------------------------------------------
// PredicateContext::Receiver - default
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, ReceiverContextIsDefault )
{
	Source< TypeParam > src;
	Sink sink;

	// omitting PredicateContext behaves identically to passing Receiver
	// NOTE: explicit PredicateType::create is unnecessary, but here for example
	src.valueChanged.connectLambda(
		sink,
		[ &sink ]( int v ) { sink.onValue( v ); },
		{ TypeParam::PredicateType::create( []( int v ) { return v > 5; } ) } );

	src.valueChanged( 3 );
	EXPECT_EQ( sink.callCount, 0 );

	src.valueChanged( 10 );
	EXPECT_EQ( sink.callCount, 1 );
}

// ---------------------------------------------------------------------------
// PredicateContext::Sender - evaluated at emission time
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, SenderContextPassingPredicate )
{
	Source< TypeParam > src;
	Sink sink;

	src.valueChanged.connectLambda(
		sink,
		[ &sink ]( int v ) { sink.onValue( v ); },
		{ []( int v ) { return v > 5; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( 3 );
	EXPECT_EQ( sink.callCount, 0 );

	src.valueChanged( 10 );
	EXPECT_EQ( sink.callCount, 1 );
	EXPECT_EQ( sink.lastValue, 10 );
}

TYPED_TEST( PredicateContext, SenderContextMultipleEmissions )
{
	Source< TypeParam > src;
	Sink sink;

	src.valueChanged.connectLambda(
		sink,
		[ &sink ]( int v ) { sink.onValue( v ); },
		{ []( int v ) { return v % 2 == 0; }, pulsar::PredicateContext::Sender } );  // even values only

	src.valueChanged( 1 );
	src.valueChanged( 2 );
	src.valueChanged( 3 );
	src.valueChanged( 4 );

	EXPECT_EQ( sink.callCount, 2 );
	EXPECT_EQ( sink.lastValue, 4 );
}

TYPED_TEST( PredicateContext, SenderContextLambdaHandler )
{
	Source< TypeParam > src;
	pulsar::Trackable receiver;

	int callCount = 0;
	src.valueChanged.connectLambda(
		receiver,
		[ &callCount ]( int ) { callCount++; },
		{ []( int v ) { return v > 0; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( -1 );
	EXPECT_EQ( callCount, 0 );

	src.valueChanged( 1 );
	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Both contexts coexist on the same event (different connections)
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, BothContextsOnSameEvent )
{
	Source< TypeParam > src;
	Sink sink1;
	Sink sink2;

	// sink1: receiver context, threshold > 5
	src.valueChanged.connectLambda(
		sink1,
		[ &sink1 ]( int v ) { sink1.onValue( v ); },
		{ []( int v ) { return v > 5; }, pulsar::PredicateContext::Receiver } );

	// sink2: sender context, threshold > 5
	src.valueChanged.connectLambda(
		sink2,
		[ &sink2 ]( int v ) { sink2.onValue( v ); },
		{ []( int v ) { return v > 5; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( 3 );
	EXPECT_EQ( sink1.callCount, 0 );
	EXPECT_EQ( sink2.callCount, 0 );

	src.valueChanged( 10 );
	EXPECT_EQ( sink1.callCount, 1 );
	EXPECT_EQ( sink2.callCount, 1 );
}

// ---------------------------------------------------------------------------
// Sender context does not affect other connections on the same event
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, SenderContextDoesNotAffectOtherConnections )
{
	Source< TypeParam > src;
	Sink sinkCond;
	Sink sinkAlways;

	// unconditional connection
	src.valueChanged.connect( sinkAlways, &Sink::onValue );

	// sender-context conditional connection - always false
	src.valueChanged.connectLambda(
		sinkCond,
		[ &sinkCond ]( int v ) { sinkCond.onValue( v ); },
		{ []( int ) { return false; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( 42 );

	EXPECT_EQ( sinkAlways.callCount, 1 );  // unconditional, always fires
	EXPECT_EQ( sinkCond.callCount, 0 );    // predicate blocked it
}

// ---------------------------------------------------------------------------
// Disconnect works for sender-context connections
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, SenderContextDisconnect )
{
	Source< TypeParam > src;
	Sink sink;

	auto conn = src.valueChanged.connectLambda(
		sink,
		[ &sink ]( int v ) { sink.onValue( v ); },
		{ []( int ) { return true; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( 1 );
	EXPECT_EQ( sink.callCount, 1 );

	conn.disconnect();
	src.valueChanged( 2 );
	EXPECT_EQ( sink.callCount, 1 );
}

TYPED_TEST( PredicateContext, SenderContextAutoDisconnectOnReceiverDestruction )
{
	Source< TypeParam > src;
	int callCount = 0;

	{
		Sink sink;
		src.valueChanged.connectLambda(
			sink,
			[ &sink ]( int v ) { sink.onValue( v ); },
			{ []( int ) { return true; }, pulsar::PredicateContext::Sender } );
		src.valueChanged( 1 );
		callCount = sink.callCount;
	}
	// sink destroyed - should not crash
	src.valueChanged( 2 );

	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Predicate works with partial argument matched handler (connect<&Method>,
// method arity shorter than the event's) combined with an explicit
// Sender-context predicate.  The predicate matches the event's full arity
// regardless of the handler's truncated one, same convention as test
// PartialArgs.ConnectPartialArgsWithPredicate.
// ---------------------------------------------------------------------------
TYPED_TEST( PredicateContext, PredicateOnPartialArgHandler )
{
	class ZeroArgSink : public pulsar::Trackable
	{
	public:
		int callCount = 0;
		void onFire() { callCount++; }
	};

	Source< TypeParam > src;
	ZeroArgSink sink;

	// event fires (int), method takes () - Pulsar's own compile-time
	// argument truncation, not a hand-written adapter
	src.valueChanged.template connect< &ZeroArgSink::onFire >(
		sink,
		{ []( int v ) { return v > 0; }, pulsar::PredicateContext::Sender } );

	src.valueChanged( -1 );
	EXPECT_EQ( sink.callCount, 0 );

	src.valueChanged( 1 );
	EXPECT_EQ( sink.callCount, 1 );
}

// ---------------------------------------------------------------------------
// Trackerless connectLambda with predicate - no automatic lifetime tracking,
// same trust model as connectFree()/connectLambda()'s no-tracker overloads.
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, TrackerlessPreWrappedCallables )
{
	Source< TypeParam > src;

	int callCount = 0;
	src.valueChanged.connectLambda(
		TypeParam::HandlerType::create( [ &callCount ]( int ) { callCount++; } ),
		{ []( int v ) { return v > 0; } } );

	src.valueChanged( -1 );
	EXPECT_EQ( callCount, 0 );

	src.valueChanged( 1 );
	EXPECT_EQ( callCount, 1 );
}

TYPED_TEST( PredicateContext, TrackerlessRawFunctorsWithPriority )
{
	Source< TypeParam > src;
	std::vector< int > order;

	// demonstrate different approaches to constructing ConnParams

	src.valueChanged.connectLambda(
		[ &order ]( int ) { order.push_back( 1 ); },
		typename TypeParam::ConnParams( []( int v ) { return v > 0; } ).prio( 1u ) );

	src.valueChanged.connectLambda(
		[ &order ]( int ) { order.push_back( 10 ); },
		src.valueChanged.params( []( int v ) { return v > 0; } ).prio( 10u ) );

	src.valueChanged( 5 );
	EXPECT_EQ( order, ( std::vector< int >{ 10, 1 } ) );  // higher priority first

	order.clear();
	src.valueChanged( -1 );
	EXPECT_TRUE( order.empty() );  // predicate blocks both
}

// ---------------------------------------------------------------------------
// PredicateContext::Receiver on a Deferred connection: the predicate is
// evaluated at drain() time against whatever state the receiver is in
// at that moment, not the state at the moment of trigger() - a value that
// would have passed at trigger time can still be filtered out if receiver
// state changes before the drain actually happens, and vice versa.
// ---------------------------------------------------------------------------

TYPED_TEST( PredicateContext, ReceiverContextDeferredEvaluatesAtDrainTime )
{
	struct ThresholdSink : public pulsar::Trackable
	{
		int minValue = 0;
		std::vector< int > received;

		bool passesThreshold( int v ) { return v >= minValue; }
		void onValue( int v ) { received.push_back( v ); }
	};

	struct DeferredSource : public pulsar::Trackable
	{
		pulsar::EventLoop loop;
		TypeParam ev{ this };
		DeferredSource() { setEventLoop( &loop ); }
	};

	DeferredSource src;
	pulsar::EventLoop sinkLoop;
	ThresholdSink sink;
	sink.setEventLoop( &sinkLoop );
	sink.minValue = 0;

	src.ev.template connect< &ThresholdSink::onValue >(
		sink,
		{ [ &sink ]( int v ) { return sink.passesThreshold( v ); } } );

	// value passes the predicate as it stands right now
	sink.minValue = 5;
	src.ev( 10 );
	src.loop.drain();  // sender-side deferral resolves, posting to sinkLoop
	sinkLoop.drain();
	EXPECT_EQ( sink.received, ( std::vector< int >{ 10 } ) );

	// raise the threshold AFTER trigger() but BEFORE drain() - since the
	// predicate is Receiver-context, it's evaluated at drain time against
	// the *new* threshold, not the one in effect when triggered
	src.ev( 3 );
	src.loop.drain();
	sink.minValue = 20;  // raised before sinkLoop is drained
	sinkLoop.drain();
	EXPECT_EQ( sink.received, ( std::vector< int >{ 10 } ) );  // 3 < 20 - filtered at drain time
}

TYPED_TEST( PredicateContext, DisconnectingPredicateConnectionLeavesPlainConnectIntact )
{
	Source< TypeParam > src;
	Sink always;

	int conditionalCount = 0;
	auto conditionalConn = src.valueChanged.connectLambda(
		always,
		[ &conditionalCount ]( int ) { conditionalCount++; },
		{ []( int v ) { return v > 0; } } );

	src.valueChanged.connect( always, &Sink::onValue );

	src.valueChanged( -1 );
	EXPECT_EQ( always.callCount, 1 );  // unconditional fired
	EXPECT_EQ( conditionalCount, 0 );  // predicate blocked

	conditionalConn.disconnect();

	// always.reset();
	src.valueChanged( -1 );
	EXPECT_EQ( always.callCount, 2 );  // unconditional still works after removing the predicate one
	EXPECT_EQ( conditionalCount, 0 );  // stays disconnected
}
