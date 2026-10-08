#include "test_helpers.h"

// ---------------------------------------------------------------------------
// Anchor / Tracked<T>
//
// A receiver doesn't have to derive from Trackable to participate safely in
// a connection: embedding a plain stellyra::Anchor (an alias for Trackable) as
// a member gives it something for the connection machinery to track, and
// stellyra::Tracked<T> bundles that anchor with the receiver reference at the
// connect() call site:
//
//   class PlainReceiver
//   {
//   public:
//       void onValue( int v ) { ... }
//       stellyra::Anchor anchor;
//   };
//
//   PlainReceiver receiver;
//   sender.valueChanged.connect(
//       stellyra::Tracked{ receiver, receiver.anchor }, &PlainReceiver::onValue );
//
// When the anchor is destroyed (typically because the receiver itself was
// destroyed), the connection is severed the same way it would be for a
// Trackable-derived receiver.  setEventLoop()/eventLoop() are called
// directly on the Anchor, since it is a Trackable.
//
// No test here does reentrant connect/disconnect from within its own
// dispatch, and no test spawns real threads, so all three MutexType
// variants (Event, SharedEvent, SingleThreadedEvent) are safe.  Sender
// holds two events of different arity, so it is parametrized on MutexType
// directly (BasicEvent<MutexType, Args...>) rather than a fixed alias.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

namespace {

template< typename MutexType >
class Sender : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int > valueChanged { this };
	stellyra::BasicEvent< MutexType, int, int > twoArgs { this };
};

} // namespace

// non-Trackable-inheriting receiver - the typical use case for Anchor
class PlainReceiver
{
public:
	int callCount = 0;
	int lastValue = -1;

	// declared last among data members - see Anchor's doc comment in
	// trackable.h for why this is a cheap, non-mandatory hardening
	// measure rather than a strict requirement
	stellyra::Anchor anchor;

	void onValue( int v )
	{
		callCount++;
		lastValue = v;
	}

	void onTwo( int a, int b )
	{
		callCount++;
		lastValue = a + b;
	}
};

template< typename MutexType >
class ReceiverLifetimeAnchor : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( ReceiverLifetimeAnchor, MutexTypes );

// ---------------------------------------------------------------------------
// Basic connectivity
// ---------------------------------------------------------------------------

TYPED_TEST( ReceiverLifetimeAnchor, ConnectAndReceive )
{
	Sender< TypeParam > sender;
	PlainReceiver receiver;

	// explicit stellyra::Tracked unnecessary, but included as example
	sender.valueChanged.connect( stellyra::Tracked { receiver, receiver.anchor }, &PlainReceiver::onValue );
	sender.valueChanged( 42 );

	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.lastValue, 42 );
}

TYPED_TEST( ReceiverLifetimeAnchor, MultipleEmissions )
{
	Sender< TypeParam > sender;
	PlainReceiver receiver;

	sender.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );

	sender.valueChanged( 1 );
	sender.valueChanged( 2 );
	sender.valueChanged( 4 );

	EXPECT_EQ( receiver.callCount, 3 );
	EXPECT_EQ( receiver.lastValue, 4 );
}

TYPED_TEST( ReceiverLifetimeAnchor, MultipleArguments )
{
	Sender< TypeParam > sender;
	PlainReceiver receiver;

	sender.twoArgs.connect( { receiver, receiver.anchor }, &PlainReceiver::onTwo );
	sender.twoArgs( 10, 20 );

	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.lastValue, 30 );
}

TYPED_TEST( ReceiverLifetimeAnchor, MultipleSenders )
{
	Sender< TypeParam > sender1;
	Sender< TypeParam > sender2;
	PlainReceiver receiver;

	sender1.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );
	sender2.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );

	sender1.valueChanged( 1 );
	sender2.valueChanged( 2 );

	EXPECT_EQ( receiver.callCount, 2 );
}

// ---------------------------------------------------------------------------
// Lifetime management
// ---------------------------------------------------------------------------

TYPED_TEST( ReceiverLifetimeAnchor, ConnectionSeveredOnReceiverDestruction )
{
	Sender< TypeParam > sender;
	int callCount = 0;

	{
		PlainReceiver receiver;
		sender.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );
		sender.valueChanged( 1 );
		callCount = receiver.callCount;
	}
	// receiver destroyed - connection should be severed

	EXPECT_EQ( callCount, 1 );
	// must not crash or invoke destroyed receiver
	sender.valueChanged( 2 );

	// callCount should still be 1
	EXPECT_EQ( callCount, 1 );
}

TYPED_TEST( ReceiverLifetimeAnchor, ManualDisconnect )
{
	Sender< TypeParam > sender;
	PlainReceiver receiver;

	auto conn = sender.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );
	sender.valueChanged( 1 );
	EXPECT_EQ( receiver.callCount, 1 );

	conn.disconnect();
	sender.valueChanged( 2 );
	EXPECT_EQ( receiver.callCount, 1 );
}

TYPED_TEST( ReceiverLifetimeAnchor, ConnectionHandleIsConnected )
{
	Sender< TypeParam > sender;
	PlainReceiver receiver;

	auto conn = sender.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );
	EXPECT_TRUE( conn.isConnected() );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );
}

TYPED_TEST( ReceiverLifetimeAnchor, SenderDestroyedDoesNotCrash )
{
	PlainReceiver receiver;

	{
		Sender< TypeParam > sender;
		sender.valueChanged.connect( { receiver, receiver.anchor }, &PlainReceiver::onValue );
		sender.valueChanged( 1 );
	}
	// sender destroyed - receiver still alive, just no more events

	EXPECT_EQ( receiver.callCount, 1 );
}

// ---------------------------------------------------------------------------
// The pattern works with any anchor member name
// ---------------------------------------------------------------------------

TYPED_TEST( ReceiverLifetimeAnchor, AnchorPatternWithDifferentMemberName )
{
	class AliasReceiver
	{
	public:
		int callCount = 0;
		stellyra::Anchor lifetimeAnchor;
		void onValue( int ) { callCount++; }
	};

	Sender< TypeParam > sender;
	AliasReceiver receiver;

	sender.valueChanged.connect( { receiver, receiver.lifetimeAnchor }, &AliasReceiver::onValue );
	sender.valueChanged( 99 );

	EXPECT_EQ( receiver.callCount, 1 );
}

// ---------------------------------------------------------------------------
// EventLoop access
// ---------------------------------------------------------------------------

TEST( ReceiverLifetimeAnchorUntyped, AnchorHasNativeEventLoopAccessors )
{
	// doesn't touch a sender Event at all - pure Anchor mechanics, so stays
	// a plain TEST() rather than running 3x for no benefit
	PlainReceiver receiver;

	// eventLoop()/setEventLoop() are called directly on the Anchor, since
	// it is a Trackable
	EXPECT_EQ( receiver.anchor.eventLoop(), nullptr );
}
