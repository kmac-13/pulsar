#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>

#include <atomic>

// Tests for reentrant triggerImpl() scenarios - specifically around
// PredicateContext::Sender predicates calling back into the same Event
// (disconnect, disconnectAll, or re-emit) on the same thread while a
// triggerImpl() walk is in progress.
//
// Every test here does reentrant connect/disconnect/re-emit from within
// that same event's own dispatch.  event_impl.h documents that this is
// only supported for the exclusive-lock MutexTypes (RecursiveMutex/Event,
// NullMutex/SingleThreadedEvent) - SharedEvent's shared-lock dispatch path
// would need to acquire the exclusive lock while the dispatching thread
// already holds the shared one, which deadlocks (confirmed empirically in
// test_pending_removal_stress.cpp).  So this suite intentionally excludes
// SharedEvent, using the same restricted type set as once()-capable tests.

namespace {

template< typename EventT >
class ReentrantSender : public pulsar::Trackable
{
public:
	EventT fired{ this };
};

class ReentrantReceiver : public pulsar::Trackable
{
public:
	int last = -1;
	int callCount = 0;
	void onFired( int v )
	{
		last = v;
		++callCount;
	}
};

} // namespace

template< typename EventT >
class Reentrancy : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( Reentrancy, EventTypes );

// A Sender-context predicate calls disconnect() on its own connection handle,
// disconnecting itself.  Verifies no crash while disconnecting a connection
// that dispatch is currently walking, and that the just-disconnected handler
// correctly doesn't fire (isDispatchReady() is re-checked after the predicate
// returns).
TYPED_TEST( Reentrancy, PredicateDisconnectsSelf )
{
	// a PredicateContext::Sender predicate disconnects its own connection
	// during evaluation
	ReentrantSender< TypeParam > sender;
	ReentrantReceiver receiver;

	auto connHandle = std::make_shared< pulsar::Connection >();
	*connHandle = sender.fired.connectLambda(
		receiver,
		[ &receiver ]( int v ) { receiver.onFired( v ); },
		pulsar::ConnectionType::Direct,
		sender.fired.params().when(
			[ connHandle ]( int ) -> bool {
				connHandle->disconnect();
				return true;
			}, pulsar::PredicateContext::Sender ) );

	EXPECT_NO_THROW( sender.fired( 42 ) );

	// disconnect() cleared STATE_CONNECTED before the handler ran;
	// isDispatchReady() gates on that so the handler is not called
	EXPECT_EQ( receiver.callCount, 0 );
	EXPECT_FALSE( connHandle->isConnected() );

	// second emission: connection remains disconnected
	sender.fired( 43 );
	EXPECT_EQ( receiver.callCount, 0 );
}

// A Sender-context predicate calls disconnectAll() on its own event as a
// side effect.  Verifies no crash while disconnecting connections dispatch
// is currently walking, and that the just-disconnected handlers correctly
// don't fire (isDispatchReady() is re-checked after the predicate returns).
TYPED_TEST( Reentrancy, PredicateDisconnectsAll )
{
	// a PredicateContext::Sender predicate calls disconnectAll() on the
	// same Event, affecting every other connection in the same emission
	ReentrantSender< TypeParam > sender;
	ReentrantReceiver receiverA;
	ReentrantReceiver receiverB;

	TypeParam* eventPtr = &sender.fired;

	sender.fired.connectLambda(
		receiverA,
		[ &receiverA ]( int v ) { receiverA.onFired( v ); },
		pulsar::ConnectionType::Direct,
		{ [ eventPtr ]( int ) -> bool {
			eventPtr->disconnectAll();
			return true;
		}, pulsar::PredicateContext::Sender } );

	sender.fired.connect( receiverB, &ReentrantReceiver::onFired, pulsar::ConnectionType::Direct );

	EXPECT_NO_THROW( sender.fired( 100 ) );

	// disconnectAll() disconnects every connection including A itself;
	// the post-predicate isConnected() re-check prevents invocation
	EXPECT_EQ( receiverA.callCount, 0 );
	EXPECT_EQ( receiverB.callCount, 0 );

	// second emission - both gone
	sender.fired( 101 );
	EXPECT_EQ( receiverA.callCount, 0 );
	EXPECT_EQ( receiverB.callCount, 0 );
}

// A Sender-context predicate re-triggers its own event from within its own
// evaluation (recursive triggerImpl() on the same thread).  Verifies this
// doesn't deadlock, and that the inner emission fully completes before the
// outer one resumes and invokes its own handler.
TYPED_TEST( Reentrancy, PredicateReemitsSameEvent )
{
	// a PredicateContext::Sender predicate re-emits the same event on the
	// same thread (recursive triggerImpl()), guards against the lock being
	// held across the recursive call
	ReentrantSender< TypeParam > sender;
	ReentrantReceiver receiver;

	TypeParam* eventPtr = &sender.fired;
	auto recursed = std::make_shared< bool >( false );

	sender.fired.connectLambda(
		receiver,
		[ &receiver ]( int v ) { receiver.onFired( v ); },
		pulsar::ConnectionType::Direct,
		sender.fired.params().when(
			[ eventPtr, recursed ]( int v ) -> bool {
				if ( ! *recursed && v == 1 )
				{
					*recursed = true;
					( *eventPtr )( 2 );  // recursive emission while predicate is running
				}
				return true;
			}, pulsar::PredicateContext::Sender ) );

	EXPECT_NO_THROW( sender.fired( 1 ) );

	// inner emission (v=2) completes fully before the outer handler runs,
	// so callCount=2 and last=1 (outer handler ran last)
	EXPECT_EQ( receiver.callCount, 2 );
	EXPECT_EQ( receiver.last, 1 );
}

TYPED_TEST( Reentrancy, HandlerReemitsSameEvent )
{
	// a Direct handler re-emits the same event from within its own
	// invocation, the recursive triggerImpl() must not deadlock
	// (Event's RecursiveMutex, and SingleThreadedEvent's lock-free
	// NullMutex, both allow same-thread re-entry)
	ReentrantSender< TypeParam > sender;
	int counter = 0;

	class RecursiveHandler : public pulsar::Trackable
	{
	public:
		TypeParam* ev = nullptr;
		int* counter = nullptr;
		int callCount = 0;

		void onFired( int depth )
		{
			++callCount;
			++( *counter );
			if ( depth > 0 )
			{
				( *ev )( depth - 1 );  // recurse
			}
		}
	};

	RecursiveHandler handler;
	handler.ev = &sender.fired;
	handler.counter = &counter;

	sender.fired.connect( handler, &RecursiveHandler::onFired, pulsar::ConnectionType::Direct );

	EXPECT_NO_THROW( sender.fired( 3 ) );

	// depths 3, 2, 1, 0 - handler called 4 times total
	EXPECT_EQ( handler.callCount, 4 );
	EXPECT_EQ( counter, 4 );
}
