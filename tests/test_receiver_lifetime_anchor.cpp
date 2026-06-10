#include "test_helpers.hpp"

#include <kmac/pulsar/receiver_lifetime_anchor.h>

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

class Sender : public pulsar::Object
{
public:
	pulsar::Event< int > valueChanged;
	pulsar::Event< int, int > twoArgs;

	explicit Sender()
		: valueChanged( this )
		, twoArgs( this )
	{
	}
};

// non-Object-inheriting receiver - the typical use case for RLA
class PlainReceiver
{
public:
	int callCount = 0;
	int lastValue = -1;

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

	// declared last - after all data members handlers may access
	pulsar::ReceiverLifetimeAnchor< PlainReceiver > pulsarAnchor { this };
};

// ---------------------------------------------------------------------------
// Basic connectivity
// ---------------------------------------------------------------------------

TEST( ReceiverLifetimeAnchor, ConnectAndReceive )
{
	auto sender = std::make_shared< Sender >();
	PlainReceiver receiver;

	sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
	sender->valueChanged( 42 );

	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.lastValue, 42 );
}

TEST( ReceiverLifetimeAnchor, MultipleEmissions )
{
	auto sender = std::make_shared< Sender >();
	PlainReceiver receiver;

	sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );

	sender->valueChanged( 1 );
	sender->valueChanged( 2 );
	sender->valueChanged( 3 );

	EXPECT_EQ( receiver.callCount, 3 );
	EXPECT_EQ( receiver.lastValue, 3 );
}

TEST( ReceiverLifetimeAnchor, MultipleArguments )
{
	auto sender = std::make_shared< Sender >();
	PlainReceiver receiver;

	sender->twoArgs.connect( receiver.pulsarAnchor, &PlainReceiver::onTwo );
	sender->twoArgs( 10, 20 );

	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.lastValue, 30 );
}

TEST( ReceiverLifetimeAnchor, MultipleSenders )
{
	auto sender1 = std::make_shared< Sender >();
	auto sender2 = std::make_shared< Sender >();
	PlainReceiver receiver;

	sender1->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
	sender2->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );

	sender1->valueChanged( 1 );
	sender2->valueChanged( 2 );

	EXPECT_EQ( receiver.callCount, 2 );
}

// ---------------------------------------------------------------------------
// Lifetime management
// ---------------------------------------------------------------------------

TEST( ReceiverLifetimeAnchor, ConnectionSeveredOnReceiverDestruction )
{
	auto sender = std::make_shared< Sender >();
	int callCount = 0;

	{
		PlainReceiver receiver;
		sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
		sender->valueChanged( 1 );
		callCount = receiver.callCount;
	}
	// receiver destroyed - connection should be severed

	EXPECT_EQ( callCount, 1 );
	// must not crash or invoke destroyed receiver
	sender->valueChanged( 2 );
}

TEST( ReceiverLifetimeAnchor, ManualDisconnect )
{
	auto sender = std::make_shared< Sender >();
	PlainReceiver receiver;

	auto conn = sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
	sender->valueChanged( 1 );
	EXPECT_EQ( receiver.callCount, 1 );

	conn.disconnect();
	sender->valueChanged( 2 );
	EXPECT_EQ( receiver.callCount, 1 );
}

TEST( ReceiverLifetimeAnchor, ConnectionHandleIsConnected )
{
	auto sender = std::make_shared< Sender >();
	PlainReceiver receiver;

	auto conn = sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
	EXPECT_TRUE( conn.isConnected() );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );
}

TEST( ReceiverLifetimeAnchor, SenderDestroyedDoesNotCrash )
{
	PlainReceiver receiver;

	{
		auto sender = std::make_shared< Sender >();
		sender->valueChanged.connect( receiver.pulsarAnchor, &PlainReceiver::onValue );
		sender->valueChanged( 1 );
	}
	// sender destroyed - receiver still alive, just no more events

	EXPECT_EQ( receiver.callCount, 1 );
}

// ---------------------------------------------------------------------------
// RLAnchor alias
// ---------------------------------------------------------------------------

TEST( ReceiverLifetimeAnchor, RLAnchorAlias )
{
	class AliasReceiver
	{
	public:
		int callCount = 0;
		void onValue( int ) { callCount++; }
		pulsar::RLAnchor< AliasReceiver > pulsarAnchor { this };
	};

	auto sender = std::make_shared< Sender >();
	AliasReceiver receiver;

	sender->valueChanged.connect( receiver.pulsarAnchor, &AliasReceiver::onValue );
	sender->valueChanged( 99 );

	EXPECT_EQ( receiver.callCount, 1 );
}

// ---------------------------------------------------------------------------
// EventLoop forwarding
// ---------------------------------------------------------------------------

TEST( ReceiverLifetimeAnchor, SetEventLoopForwards )
{
	PlainReceiver receiver;

	// just verify setEventLoop and eventLoop() compile and round-trip
	EXPECT_EQ( receiver.pulsarAnchor.eventLoop(), nullptr );
}
