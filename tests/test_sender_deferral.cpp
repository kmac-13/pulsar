#include "test_helpers.hpp"
#include <atomic>

// ---------------------------------------------------------------------------
// Sender Deferral
//
// When a sender Object has an associated EventLoop, any emission of one of
// its Events that occurs outside that loop's drain context is automatically
// deferred: the entire emission is posted to the sender's loop and runs when
// the loop next drains.
//
// This serialises all emissions to a single context without requiring the
// caller to hold any locks, and is the Pulsar equivalent of Qt's thread
// affinity - except Pulsar uses EventLoop identity rather than OS thread
// identity, so multiple manual loops can coexist on one thread and still
// dispatch correctly.
//
// If the sender has no associated EventLoop, emissions always run directly
// on the calling thread.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Test 1: No loop on sender - emission is always direct
// ---------------------------------------------------------------------------

TEST( SenderDeferral, NoLoopIsAlwaysDirect )
{
	auto sender = std::make_shared< pulsar::Object >();  // no loop
	auto receiver = std::make_shared< pulsar::Object >();

	pulsar::Event< int > event{ sender.get() };

	std::atomic< int > callCount{ 0 };
	event.connect( receiver, [ &callCount ]( int ) { callCount++; } );

	event( 1 );
	EXPECT_EQ( callCount.load(), 1 );  // executed immediately, no loop to defer to

	event( 2 );
	EXPECT_EQ( callCount.load(), 2 );
}

// ---------------------------------------------------------------------------
// Test 2: Manual loop - emit from outside drain → deferred until processEvents
// ---------------------------------------------------------------------------

TEST( SenderDeferral, ManualLoopDefersUntilProcessEvents )
{
	auto loop = pulsar::EventLoop::makeManualProcessed();
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	sender->setEventLoop( &loop );

	pulsar::Event< int > event{ sender.get() };

	std::atomic< int > callCount{ 0 };
	event.connect( receiver, [ &callCount ]( int ) { callCount++; } );

	// emit from outside the drain - should be deferred, not executed yet
	event( 1 );
	EXPECT_EQ( callCount.load(), 0 );

	event( 2 );
	EXPECT_EQ( callCount.load(), 0 );

	// drain the loop - both deferred emissions should now execute
	loop.processEvents();
	EXPECT_EQ( callCount.load(), 2 );
}

// ---------------------------------------------------------------------------
// Test 3: Emit from inside drain context - executes directly (no re-deferral)
// ---------------------------------------------------------------------------

TEST( SenderDeferral, EmitFromInsideDrainIsDirect )
{
	auto loop = pulsar::EventLoop::makeManualProcessed();
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	sender->setEventLoop( &loop );

	pulsar::Event< int > event{ sender.get() };

	std::atomic< int > callCount{ 0 };
	std::atomic< bool > handledImmediately{ false };

	event.connect( receiver, [ &callCount ]( int ) { callCount++; } );

	// post a task that emits from within the drain context
	loop.postEvent( [ & ]() {
		event( 42 );
		// if deferral happened, callCount would still be 0 here;
		// if direct, it is already 1
		handledImmediately = ( callCount.load() == 1 );
	} );

	loop.processEvents();

	EXPECT_TRUE( handledImmediately );
	EXPECT_EQ( callCount.load(), 1 );
}

// ---------------------------------------------------------------------------
// Test 4: Auto-processed loop - emit from different thread → deferred
// ---------------------------------------------------------------------------

TEST( SenderDeferral, AutoLoopDefersFromOtherThread )
{
	auto loop = pulsar::EventLoop::makeAutoProcessed();
	loop.start();

	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	sender->setEventLoop( &loop );

	pulsar::Event< int > event{ sender.get() };

	std::atomic< int > callCount{ 0 };
	event.connect( receiver, [ &callCount ]( int ) { callCount++; } );

	// emit from a separate thread - should be deferred to the loop
	std::thread t( [ & ]() { event( 99 ); } );
	t.join();

	// handler has not run yet on this thread
	// give the loop time to drain
	msleep( 50 );

	EXPECT_EQ( callCount.load(), 1 );

	loop.stop();
}

// ---------------------------------------------------------------------------
// Test 5: Two manual loops on one thread - each defers to its own loop
// ---------------------------------------------------------------------------

TEST( SenderDeferral, TwoManualLoopsInOneThread )
{
	auto loop1 = pulsar::EventLoop::makeManualProcessed();
	auto loop2 = pulsar::EventLoop::makeManualProcessed();

	auto sender1 = std::make_shared< pulsar::Object >();
	auto sender2 = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	sender1->setEventLoop( &loop1 );
	sender2->setEventLoop( &loop2 );

	pulsar::Event< int > event1{ sender1.get() };
	pulsar::Event< int > event2{ sender2.get() };

	std::atomic< int > count1{ 0 };
	std::atomic< int > count2{ 0 };

	event1.connect( receiver, [ &count1 ]( int ) { count1++; } );
	event2.connect( receiver, [ &count2 ]( int ) { count2++; } );

	// emit both - both should be deferred to their respective loops
	event1( 1 );
	event2( 1 );

	EXPECT_EQ( count1.load(), 0 );
	EXPECT_EQ( count2.load(), 0 );

	// drain loop1 only - only event1 should fire
	loop1.processEvents();
	EXPECT_EQ( count1.load(), 1 );
	EXPECT_EQ( count2.load(), 0 );

	// drain loop2 - event2 fires
	loop2.processEvents();
	EXPECT_EQ( count1.load(), 1 );
	EXPECT_EQ( count2.load(), 1 );
}

// ----------------------------------------------------------------------------
// Test 6: Deferral interacts correctly with receiver-side Deferred connections
//
// Sender has loop A, receiver has loop B, connection is Auto (resolves Deferred).
// Emit from external thread → deferred to loop A → runs in loop A drain →
// Deferred connection posts to loop B → runs in loop B drain.
// ----------------------------------------------------------------------------

TEST( SenderDeferral, DeferralWithDeferredReceiver )
{
	auto senderLoop = pulsar::EventLoop::makeManualProcessed();
	auto receiverLoop = pulsar::EventLoop::makeManualProcessed();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &senderLoop );
	receiver->setEventLoop( &receiverLoop );

	// Auto connection: sender and receiver on different loops → Deferred
	sender->dataReady.connect( receiver, &DataReceiver::processData );

	// emit from outside both loops - deferred to senderLoop
	sender->sendData( 42 );

	// nothing processed yet
	EXPECT_EQ( receiver->callCount, 0 );

	// drain senderLoop - emission runs, Deferred invocation is posted to receiverLoop
	senderLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 0 );  // still not in receiverLoop yet

	// drain receiverLoop - handler runs
	receiverLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 42 );
}

// ---------------------------------------------------------------------------
// Test 7: Real-world pattern - object with owned EventLoop
//
// An object that owns its own EventLoop and registers itself as the sender.
// Calls from any external thread are safely serialised to the object's loop.
// ---------------------------------------------------------------------------

TEST( SenderDeferral, OwnedLoopSerialisation )
{
	class DataSource : public pulsar::Object
	{
	public:
		pulsar::Event< int > dataReady{ this };

		DataSource()
		{
			setEventLoop( &_loop );
			_loop.start();
		}

		~DataSource()
		{
			_loop.stop();
		}

		// may be called from any thread
		void produce( int value )
		{
			dataReady( value );  // deferred to _loop if called from outside
		}

	private:
		pulsar::EventLoop _loop = pulsar::EventLoop::makeAutoProcessed();
	};

	auto source = std::make_shared< DataSource >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::atomic< int > callCount{ 0 };
	source->dataReady.connect( receiver, [ &callCount ]( int ) { callCount++; } );

	// call produce from two threads simultaneously - both should be safely deferred
	std::thread t1( [ & ]() { source->produce( 1 ); } );
	std::thread t2( [ & ]() { source->produce( 2 ); } );
	t1.join();
	t2.join();

	msleep( 50 );

	EXPECT_EQ( callCount.load(), 2 );
}
