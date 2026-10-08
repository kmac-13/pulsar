#include "test_helpers.h"

#include <kmac/stellyra/event_loop.h>
#include <kmac/stellyra/auto_drain_thread.h>

#include <atomic>
#include <chrono>

// ---------------------------------------------------------------------------
// Sender Deferral
//
// When a sender Trackable has an associated EventLoop, any emission of one
// of its Events that occurs outside that loop's drain context is automatically
// deferred: the entire emission is posted to the sender's loop and runs when
// the loop next drains.
//
// This serialises all emissions to a single context without requiring the
// caller to hold any locks, and is similar to Qt's thread affinity, except
// Stellyra defers the emission itself if outside of the sender's EventLoop/thread
// context while Qt processes the emission synchronously from whichever thread
// called it.  Using EventLoop, Stellyra can have multiple manual loops coexist
// on one thread and still dispatch correctly.
//
// If the sender has no associated EventLoop, emissions always run directly
// on the calling thread.
//
// Split into two typed suites:
//   - SenderDeferral (AllEventTypes): every trigger happens on whichever
//     thread the test itself runs on, or is driven purely by manual
//     drain() calls - no genuine cross-thread delivery.
//   - SenderDeferralThreadSafe (Event/SharedEvent only): tests that
//     actually trigger from a background std::thread (or an AutoDrainThread's
//     own background thread) - this is exactly the use case
//     SingleThreadedEvent's docs warn is undefined behaviour ("using it from
//     more than one thread"), matching the pattern already used in
//     test_invokers.cpp / test_thread_safety.cpp.
// ---------------------------------------------------------------------------

template< typename MutexType >
class SenderDeferral : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( SenderDeferral, MutexTypes );

// ---------------------------------------------------------------------------
// Test: No loop on sender - emission is always direct
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, NoLoopIsAlwaysDirect )
{
	stellyra::Trackable sender;  // no loop
	stellyra::Trackable receiver;

	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	std::atomic< int > callCount{ 0 };
	event.connectLambda( receiver, [ &callCount ]( int ) { callCount++; } );

	event( 1 );
	EXPECT_EQ( callCount.load(), 1 );  // executed immediately, no loop to defer to

	event( 2 );
	EXPECT_EQ( callCount.load(), 2 );
}

// ---------------------------------------------------------------------------
// Test: Manual loop - emit from outside drain -> deferred until drain()
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, ManualLoopDefersUntilDrain )
{
	stellyra::EventLoop loop;
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	sender.setEventLoop( &loop );

	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	std::atomic< int > callCount{ 0 };
	event.connectLambda( receiver, [ &callCount ]( int ) { callCount++; }, stellyra::ConnectionType::Direct );

	// emit from outside the drain - should be deferred, not executed yet
	event( 1 );
	EXPECT_EQ( callCount.load(), 0 );

	event( 2 );
	EXPECT_EQ( callCount.load(), 0 );

	// drain the loop - both deferred emissions should now execute
	loop.drain();
	EXPECT_EQ( callCount.load(), 2 );
}

// ---------------------------------------------------------------------------
// Test: Emit from inside drain context - executes directly (no re-deferral)
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, EmitFromInsideDrainIsDirect )
{
	stellyra::EventLoop loop;
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	sender.setEventLoop( &loop );

	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	std::atomic< int > callCount{ 0 };
	std::atomic< bool > handledImmediately{ false };

	event.connectLambda( receiver, [ &callCount ]( int ) { callCount++; }, stellyra::ConnectionType::Direct );

	// post a task that emits from within the drain context
	loop.post( stellyra::EventLoop::Task::create( [ & ]() {
		event( 42 );
		// if deferral happened, callCount would still be 0 here;
		// if direct, it is already 1
		handledImmediately = ( callCount.load() == 1 );
	} ) );

	loop.drain();

	EXPECT_TRUE( handledImmediately );
	EXPECT_EQ( callCount.load(), 1 );
}

// ---------------------------------------------------------------------------
// Test: Two manual loops on one thread - each defers to its own loop
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, TwoManualLoopsInOneThread )
{
	stellyra::EventLoop loop1;
	stellyra::EventLoop loop2;

	stellyra::Trackable sender1;
	stellyra::Trackable sender2;
	stellyra::Trackable receiver;

	sender1.setEventLoop( &loop1 );
	sender2.setEventLoop( &loop2 );

	stellyra::BasicEvent< TypeParam, int > event1{ &sender1 };
	stellyra::BasicEvent< TypeParam, int > event2{ &sender2 };

	std::atomic< int > count1{ 0 };
	std::atomic< int > count2{ 0 };

	event1.connectLambda( receiver, [ &count1 ]( int ) { count1++; }, stellyra::ConnectionType::Direct );
	event2.connectLambda( receiver, [ &count2 ]( int ) { count2++; }, stellyra::ConnectionType::Direct );

	// emit both - both should be deferred to their respective loops
	event1( 1 );
	event2( 1 );

	EXPECT_EQ( count1.load(), 0 );
	EXPECT_EQ( count2.load(), 0 );

	// drain loop1 only - only event1 should fire
	loop1.drain();
	EXPECT_EQ( count1.load(), 1 );
	EXPECT_EQ( count2.load(), 0 );

	// drain loop2 - event2 fires
	loop2.drain();
	EXPECT_EQ( count1.load(), 1 );
	EXPECT_EQ( count2.load(), 1 );
}

// ----------------------------------------------------------------------------
// Test: Deferral interacts correctly with receiver-side Deferred connections
//
// Sender has loop A, receiver has loop B, connection is Auto (resolves
// Deferred).  Emit (manually, from this thread) -> deferred to loop A -> runs
// in loop A drain -> Deferred connection posts to loop B -> runs in loop B
// drain.
// ----------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, DeferralWithDeferredReceiver )
{
	stellyra::EventLoop senderLoop;
	stellyra::EventLoop receiverLoop;

	DataSenderT< TypeParam > sender;
	DataReceiver receiver;

	sender.setEventLoop( &senderLoop );
	receiver.setEventLoop( &receiverLoop );

	// Auto connection: sender and receiver on different loops -> Deferred
	sender.dataReady.connect( receiver, &DataReceiver::processData );

	// emit from outside both loops - deferred to senderLoop
	sender.sendData( 42 );

	// nothing processed yet
	EXPECT_EQ( receiver.callCount, 0 );

	// drain senderLoop - emission runs, Deferred invocation is posted to receiverLoop
	senderLoop.drain();
	EXPECT_EQ( receiver.callCount, 0 );  // still not in receiverLoop yet

	// drain receiverLoop - handler runs
	receiverLoop.drain();
	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.lastValue, 42 );
}

// ---------------------------------------------------------------------------
// Test: A thread explicitly registered as a loop's drain thread (via
// setDrainThread(), as AutoDrainThread does internally) fires directly even
// when it is not, at this exact moment, inside an active drain() call - this
// is the "registered" half of shouldDispatchDirectlyOnThread, distinct from
// the "currently draining" half covered by EmitFromInsideDrainIsDirect.
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferral, RegisteredThreadFiresDirectlyWhenIdle )
{
	stellyra::EventLoop loop;
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	sender.setEventLoop( &loop );
	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	int callCount = 0;
	event.connectLambda( receiver, [ &callCount ]( int ) { callCount++; }, stellyra::ConnectionType::Direct );

	// register the current thread without ever calling drain()
	loop.setDrainThread( stellyra::platform::currentThreadId() );

	event( 1 );

	// fired synchronously - registration alone is enough, no drain() needed
	EXPECT_EQ( callCount, 1 );
}

// ---------------------------------------------------------------------------
// Genuine cross-thread delivery.  Event/SharedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename MutexType >
class SenderDeferralThreadSafe : public ::testing::Test {};

using ThreadSafeMutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex >;
TYPED_TEST_SUITE( SenderDeferralThreadSafe, ThreadSafeMutexTypes );

// ---------------------------------------------------------------------------
// Test: Auto-draining loop - emit from different thread -> deferred
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferralThreadSafe, AutoLoopDefersFromOtherThread )
{
	stellyra::EventLoop loop;
	stellyra::AutoDrainThread drainer( loop );

	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	sender.setEventLoop( &loop );

	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	std::atomic< int > callCount{ 0 };
	event.connectLambda( receiver, [ &callCount ]( int ) { callCount++; }, stellyra::ConnectionType::Direct );

	// emit from a separate thread - should be deferred to the loop
	std::thread t( [ & ]() { event( 99 ); } );
	t.join();

	// handler has not run yet on this thread, give the loop time to drain
	// msleep( 50 );

	EXPECT_EQ( callCount.load(), 1 );
}

// ---------------------------------------------------------------------------
// Test: Real-world pattern - object with owned EventLoop
//
// An object that owns its own EventLoop and registers itself as the sender.
// Calls from any external thread are safely serialised to the object's loop.
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferralThreadSafe, OwnedLoopSerialisation )
{
	class DataSource : public stellyra::Trackable
	{
	private:
		stellyra::EventLoop _loop;
		stellyra::AutoDrainThread _drainer;

	public:
		stellyra::BasicEvent< TypeParam, int > dataReady{ this };

		DataSource()
			: _drainer( _loop )
		{
			setEventLoop( &_loop );
		}

		// may be called from any thread
		void produce( int value )
		{
			dataReady( value );  // deferred to _loop if called from outside
		}
	};

	DataSource source;
	stellyra::Trackable receiver;

	std::atomic< int > callCount{ 0 };
	source.dataReady.connectLambda( receiver,
		[ &callCount ]( int ) { callCount++; }, stellyra::ConnectionType::Direct );

	// call produce from two threads simultaneously - both should be safely deferred
	std::thread t1( [ & ]() { source.produce( 1 ); } );
	std::thread t2( [ & ]() { source.produce( 2 ); } );
	t1.join();
	t2.join();

	// msleep( 50 );

	EXPECT_EQ( callCount.load(), 2 );
}

// ---------------------------------------------------------------------------
// Test: A chain of nested re-emissions triggered from within a handler
// already running on the drain thread all stay direct - not just the first
// reentrant call, but every subsequent one in the chain.
// ---------------------------------------------------------------------------

TYPED_TEST( SenderDeferralThreadSafe, NestedEmissionOnDrainThreadStaysDirect )
{
	stellyra::EventLoop loop;
	stellyra::AutoDrainThread drainer( loop );

	stellyra::Trackable sender;
	stellyra::Trackable receiver;
	sender.setEventLoop( &loop );
	stellyra::BasicEvent< TypeParam, int > event{ &sender };

	std::atomic< int > depth{ 0 };
	std::atomic< bool > nestedFired{ false };

	event.connectLambda( receiver, [ & ]( int v ) {
		int d = ++depth;
		if ( d == 1 )
		{
			// re-trigger from the drain thread itself - must stay direct,
			// not requeue through the sender-deferral gate again
			event( v + 1 );
		}
		if ( d == 2 )
		{
			nestedFired = true;
		}
		--depth;
	}, stellyra::ConnectionType::Direct );

	// first trigger comes from a different thread - must defer to the drain thread
	std::thread worker( [ & ]() { event( 1 ); } );
	worker.join();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
	while ( ! nestedFired.load() )
	{
		ASSERT_LT( std::chrono::steady_clock::now(), deadline ) << "timed out waiting for nested emission";
		msleep( 1 );
	}
	EXPECT_TRUE( nestedFired.load() );
}
