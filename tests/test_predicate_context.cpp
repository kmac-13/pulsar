#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// PredicateContext tests
//
// Verifies the distinction between PredicateContext::Receiver (default,
// evaluated at invocation time) and PredicateContext::Sender (evaluated at
// emission time in triggerImpl before any queuing).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

class Source : public pulsar::Object
{
public:
	pulsar::Event< int > valueChanged { this };

	explicit Source() : valueChanged( this ) {}
};

class Sink : public pulsar::Object
{
public:
	int callCount = 0;
	int lastValue = -1;

	void onValue( int v ) { callCount++; lastValue = v; }
	void reset() { callCount = 0; lastValue = -1; }
};

// ---------------------------------------------------------------------------
// PredicateContext::Receiver (default) - existing behaviour
// ---------------------------------------------------------------------------

TEST( PredicateContext, ReceiverContextPassingPredicate )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->valueChanged.connectIf(
		sink,
		&Sink::onValue,
		[]( int v ) { return v > 5; },
		pulsar::PredicateContext::Receiver );

	src->valueChanged( 3 );
	EXPECT_EQ( sink->callCount, 0 );

	src->valueChanged( 10 );
	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastValue, 10 );
}

TEST( PredicateContext, ReceiverContextIsDefault )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	// omitting PredicateContext should behave identically to Receiver
	src->valueChanged.connectIf(
		sink,
		&Sink::onValue,
		[]( int v ) { return v > 5; } );

	src->valueChanged( 3 );
	EXPECT_EQ( sink->callCount, 0 );

	src->valueChanged( 10 );
	EXPECT_EQ( sink->callCount, 1 );
}

// ---------------------------------------------------------------------------
// PredicateContext::Sender - evaluated at emission time
// ---------------------------------------------------------------------------

TEST( PredicateContext, SenderContextPassingPredicate )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->valueChanged.connectIf(
		sink,
		&Sink::onValue,
		[]( int v ) { return v > 5; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( 3 );
	EXPECT_EQ( sink->callCount, 0 );

	src->valueChanged( 10 );
	EXPECT_EQ( sink->callCount, 1 );
	EXPECT_EQ( sink->lastValue, 10 );
}

TEST( PredicateContext, SenderContextMultipleEmissions )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	src->valueChanged.connectIf(
		sink,
		&Sink::onValue,
		[]( int v ) { return v % 2 == 0; },  // even values only
		pulsar::PredicateContext::Sender );

	src->valueChanged( 1 );
	src->valueChanged( 2 );
	src->valueChanged( 3 );
	src->valueChanged( 4 );

	EXPECT_EQ( sink->callCount, 2 );
	EXPECT_EQ( sink->lastValue, 4 );
}

TEST( PredicateContext, SenderContextLambdaHandler )
{
	auto src = std::make_shared< Source >();
	auto receiver = std::make_shared< pulsar::Object >();

	int callCount = 0;
	src->valueChanged.connectIf(
		receiver,
		[ &callCount ]( int ) { callCount++; },
		[]( int v ) { return v > 0; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( -1 );
	EXPECT_EQ( callCount, 0 );

	src->valueChanged( 1 );
	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Both contexts coexist on the same event
// ---------------------------------------------------------------------------

TEST( PredicateContext, BothContextsOnSameEvent )
{
	auto src = std::make_shared< Source >();
	auto sink1 = std::make_shared< Sink >();
	auto sink2 = std::make_shared< Sink >();

	// sink1: receiver context, threshold > 5
	src->valueChanged.connectIf(
		sink1,
		&Sink::onValue,
		[]( int v ) { return v > 5; },
		pulsar::PredicateContext::Receiver );

	// sink2: sender context, threshold > 5
	src->valueChanged.connectIf(
		sink2,
		&Sink::onValue,
		[]( int v ) { return v > 5; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( 3 );
	EXPECT_EQ( sink1->callCount, 0 );
	EXPECT_EQ( sink2->callCount, 0 );

	src->valueChanged( 10 );
	EXPECT_EQ( sink1->callCount, 1 );
	EXPECT_EQ( sink2->callCount, 1 );
}

// ---------------------------------------------------------------------------
// Sender context does not affect other connections on the same event
// ---------------------------------------------------------------------------

TEST( PredicateContext, SenderContextDoesNotAffectOtherConnections )
{
	auto src = std::make_shared< Source >();
	auto sinkCond = std::make_shared< Sink >();
	auto sinkAlways = std::make_shared< Sink >();

	// unconditional connection
	src->valueChanged.connect( sinkAlways, &Sink::onValue );

	// sender-context conditional connection - always false
	src->valueChanged.connectIf(
		sinkCond,
		&Sink::onValue,
		[]( int ) { return false; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( 42 );

	EXPECT_EQ( sinkAlways->callCount, 1 );  // unconditional always fires
	EXPECT_EQ( sinkCond->callCount, 0 );    // predicate blocked it
}

// ---------------------------------------------------------------------------
// Disconnect works for sender-context connections
// ---------------------------------------------------------------------------

TEST( PredicateContext, SenderContextDisconnect )
{
	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< Sink >();

	auto conn = src->valueChanged.connectIf(
		sink,
		&Sink::onValue,
		[]( int ) { return true; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( 1 );
	EXPECT_EQ( sink->callCount, 1 );

	conn.disconnect();
	src->valueChanged( 2 );
	EXPECT_EQ( sink->callCount, 1 );
}

TEST( PredicateContext, SenderContextAutoDisconnectOnReceiverDestruction )
{
	auto src = std::make_shared< Source >();
	int callCount = 0;

	{
		auto sink = std::make_shared< Sink >();
		src->valueChanged.connectIf(
			sink,
			&Sink::onValue,
			[]( int ) { return true; },
			pulsar::PredicateContext::Sender );
		src->valueChanged( 1 );
		callCount = sink->callCount;
	}
	// sink destroyed - should not crash
	src->valueChanged( 2 );

	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Partial argument matching works with sender context (method pointer path)
// ---------------------------------------------------------------------------

TEST( PredicateContext, SenderContextWithPartialArgs )
{
	class ZeroArgSink : public pulsar::Object
	{
	public:
		int callCount = 0;
		void onFire() { callCount++; }
	};

	auto src = std::make_shared< Source >();
	auto sink = std::make_shared< ZeroArgSink >();

	// event fires (int), method takes () - partial arg drop via method pointer path
	src->valueChanged.connectIf(
		sink,
		&ZeroArgSink::onFire,
		[]( int v ) { return v > 0; },
		pulsar::PredicateContext::Sender );

	src->valueChanged( -1 );
	EXPECT_EQ( sink->callCount, 0 );

	src->valueChanged( 1 );
	EXPECT_EQ( sink->callCount, 1 );
}
