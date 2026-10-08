#include "test_helpers.h"

// ---------------------------------------------------------------------------
// Pending-removal-during-emission stress tests.
//
// Every test here connects or disconnects reentrantly - from within a
// handler invoked by the very dispatch() call that is iterating this same
// event's connection list.  event_impl.h documents that this is only
// supported for the exclusive-lock MutexTypes (RecursiveMutex/Event,
// NullMutex/SingleThreadedEvent): "Reentrant connect/disconnect from within
// a [SharedEvent] handler ... is not supported" (see dispatch()'s doc
// comment and triggerDepth's @note) because it would require acquiring the
// exclusive lock while the dispatching thread already holds the shared one.
//
// Unlike once() - which is caught at compile time via ConnParams::once()'s
// static_assert against SharedMutex - this general reentrant connect/
// disconnect pattern isn't caught by any static_assert; it deadlocks at
// runtime instead (confirmed: DisconnectLaterConnectionDuringEmission hangs
// under SharedEvent).  So this suite intentionally uses the same type set as
// once()-capable tests (Event, SingleThreadedEvent) and excludes SharedEvent
// entirely, rather than AllEventTypes.
// ---------------------------------------------------------------------------

namespace {

template< typename EventT >
class StressSender : public stellyra::Trackable
{
public:
	EventT sig{ this };
};

class StressReceiver : public stellyra::Trackable
{
public:
	int id;
	int hits = 0;
	stellyra::Connection self;
	StressReceiver( int i ) : id( i ) {}
	void onValue( int )
	{
		++hits;
	}
};

class StressDisconnector : public stellyra::Trackable
{
public:
	int hits = 0;
	stellyra::Connection* target = nullptr;
	void onValue( int )
	{
		++hits;
		if ( target )
		{
			target->disconnect();
		}
	}
};

} // namespace

template< typename EventT >
class PendingRemovalStress : public ::testing::Test {};

using EventTypes = ::testing::Types<
	stellyra::Event< int >,
	stellyra::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( PendingRemovalStress, EventTypes );

TYPED_TEST( PendingRemovalStress, DisconnectLaterConnectionDuringEmission )
{
	auto sender = std::make_unique< StressSender< TypeParam > >();
	auto first = std::make_unique< StressDisconnector >();
	auto second = std::make_unique< StressReceiver >( 2 );
	auto third = std::make_unique< StressReceiver >( 3 );

	stellyra::Connection secondConn = sender->sig.connect(
		*second, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 5 );
	first->target = &secondConn;
	sender->sig.connect( *first, &StressDisconnector::onValue, stellyra::ConnectionType::Direct, 10 );
	sender->sig.connect( *third, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 0 );

	sender->sig( 1 );

	// first ran and disconnected second before second's turn
	EXPECT_EQ( first->hits, 1 );
	EXPECT_EQ( second->hits, 0 );  // skipped this emission
	EXPECT_EQ( third->hits, 1 );   // unaffected

	sender->sig( 1 );
	EXPECT_EQ( first->hits, 2 );
	EXPECT_EQ( second->hits, 0 );  // still disconnected
	EXPECT_EQ( third->hits, 2 );
}

TYPED_TEST( PendingRemovalStress, DisconnectEarlierConnectionDuringEmission )
{
	auto sender = std::make_shared< StressSender< TypeParam > >();
	auto first = std::make_shared< StressReceiver >( 1 );
	auto second = std::make_shared< StressDisconnector >();

	stellyra::Connection firstConn = sender->sig.connect(
		*first, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 10 );
	second->target = &firstConn;
	sender->sig.connect( *second, &StressDisconnector::onValue, stellyra::ConnectionType::Direct, 5 );

	sender->sig( 1 );
	EXPECT_EQ( first->hits, 1 );  // already invoked before second disconnects it
	EXPECT_EQ( second->hits, 1 );

	sender->sig( 1 );
	EXPECT_EQ( first->hits, 1 );  // gone
	EXPECT_EQ( second->hits, 2 );
}

TYPED_TEST( PendingRemovalStress, DisconnectAllDuringEmissionThenReconnect )
{
	StressSender< TypeParam > sender;
	StressReceiver a { 1 };
	StressReceiver b = { 2 };

	TypeParam* eventPtr = &sender.sig;

	class AllDisconnector : public stellyra::Trackable
	{
	public:
		TypeParam* ev = nullptr;
		int hits = 0;
		void onValue( int )
		{
			++hits;
			ev->disconnectAll();
		}
	};
	AllDisconnector disc;
	disc.ev = eventPtr;

	sender.sig.connect( disc, &AllDisconnector::onValue, stellyra::ConnectionType::Direct, 10 );
	sender.sig.connect( a, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 5 );
	sender.sig.connect( b, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 0 );

	sender.sig( 1 );
	EXPECT_EQ( disc.hits, 1 );
	EXPECT_EQ( a.hits, 0 );     // disconnectAll() happened before a's turn
	EXPECT_EQ( b.hits, 0 );

	// list should be compacted to empty; fresh connect must work
	StressReceiver c { 3 };
	sender.sig.connect( c, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 0 );
	sender.sig( 2 );
	EXPECT_EQ( c.hits, 1 );
}

TYPED_TEST( PendingRemovalStress, ConnectDuringEmissionDoesNotFireUntilNextEmission )
{
	auto sender = std::make_shared< StressSender< TypeParam > >();

	class Connector : public stellyra::Trackable
	{
	public:
		TypeParam* ev = nullptr;
		std::shared_ptr< StressReceiver > newReceiver;
		bool connected = false;
		int  hits = 0;

		void onValue( int )
		{
			++hits;
			if ( ! connected )
			{
				connected = true;
				ev->connect( *newReceiver, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 20 );
			}
		}
	};

	auto connector = std::make_shared< Connector >();
	auto newR = std::make_shared< StressReceiver >( 99 );
	auto low = std::make_shared< StressReceiver >( 1 );

	connector->ev = &sender->sig;
	connector->newReceiver = newR;

	sender->sig.connect( *connector, &Connector::onValue, stellyra::ConnectionType::Direct, 10 );
	sender->sig.connect( *low, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 0 );

	sender->sig( 1 );
	EXPECT_EQ( connector->hits, 1 );
	EXPECT_EQ( newR->hits, 0 );       // not fired this emission
	EXPECT_EQ( low->hits, 1 );

	sender->sig( 2 );
	EXPECT_EQ( connector->hits, 2 );
	EXPECT_EQ( newR->hits, 1 );       // now fires
	EXPECT_EQ( low->hits, 2 );
}

TYPED_TEST( PendingRemovalStress, DisconnectFirstElement )
{
	auto sender = std::make_shared< StressSender< TypeParam > >();
	auto first = std::make_shared< StressReceiver >( 1 );
	auto last = std::make_shared< StressDisconnector >();

	stellyra::Connection firstConn = sender->sig.connect(
		*first, &StressReceiver::onValue, stellyra::ConnectionType::Direct, 100 );
	last->target = &firstConn;
	sender->sig.connect( *last, &StressDisconnector::onValue, stellyra::ConnectionType::Direct, 0 );

	sender->sig( 1 );
	EXPECT_EQ( first->hits, 1 );
	EXPECT_EQ( last->hits, 1 );

	sender->sig( 1 );
	EXPECT_EQ( first->hits, 1 );  // gone
	EXPECT_EQ( last->hits, 2 );
}
