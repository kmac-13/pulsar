#include "test_helpers.h"

#include <kmac/stellyra/auto_drain_thread.h>
#include <kmac/stellyra/event_loop.h>

// ---------------------------------------------------------------------------
// Externally-managed EventLoop
//
// A plain EventLoop has no built-in thread of its own: the owner calls
// drain() to process the queue - e.g. once per frame, after a platform
// event pump yields, etc.  AutoDrainThread separately wraps a loop with a
// background thread that drains it automatically whenever work is posted;
// hasDrainThread() reflects whether one is currently attached.
//
// Auto connection resolution uses EventLoop pointer identity:
//   same loop instance  -> Direct (synchronous)
//   different instances -> Deferred (asynchronous to drain())
// ---------------------------------------------------------------------------

TEST( ExternalEventLoop, HasDrainThreadReflectsAutoDrainThread )
{
	stellyra::EventLoop loop;
	EXPECT_FALSE( loop.hasDrainThread() );

	{
		stellyra::AutoDrainThread drainer( loop );

		// setDrainThread() is called from within the background thread's
		// own startup, not synchronously in this constructor - give it a
		// moment to run before checking
		msleep( 20 );
		EXPECT_TRUE( loop.hasDrainThread() );
	}

	// cleared when the AutoDrainThread is destroyed
	EXPECT_FALSE( loop.hasDrainThread() );
}

TEST( ExternalEventLoop, EventsNotProcessedUntilDrained )
{
	stellyra::EventLoop mainLoop;
	stellyra::EventLoop workerLoop;
	stellyra::AutoDrainThread workerDrainer( workerLoop );

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	// different loops -> Auto resolves Deferred
	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	workerLoop.post( stellyra::EventLoop::Task::create( [ &sender ]() { sender->sendData( 42 ); } ) );

	msleep( 30 );
	workerDrainer.stop();  // join, for a happens-before edge below

	// not yet processed - nobody has called mainLoop.drain()
	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop.drain();

	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 42 );
}

TEST( ExternalEventLoop, SameExternalLoopIsDirect )
{
	stellyra::EventLoop sharedLoop;

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &sharedLoop );
	receiver->setEventLoop( &sharedLoop );

	// same loop instance -> Auto resolves Direct
	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	sender->sendData( 99 );

	// the sender has a loop, so emission is deferred to it even though
	// the connection resolves Direct (same loop instance); one
	// drain() call runs both the deferred emission and the handler
	sharedLoop.drain();
	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 99 );
}

TEST( ExternalEventLoop, MultipleEventsDrainedTogether )
{
	stellyra::EventLoop mainLoop;
	stellyra::EventLoop workerLoop;
	stellyra::AutoDrainThread workerDrainer( workerLoop );

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	for ( int i = 1; i <= 5; ++i )
	{
		workerLoop.post( stellyra::EventLoop::Task::create( [ &sender, i ]() { sender->sendData( i ); } ) );
	}

	msleep( 50 );
	workerDrainer.stop();

	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop.drain();

	EXPECT_EQ( receiver->callCount, 5 );
}

TEST( ExternalEventLoop, RepeatedTicks )
{
	stellyra::EventLoop mainLoop;
	stellyra::EventLoop workerLoop;
	stellyra::AutoDrainThread workerDrainer( workerLoop );

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	workerLoop.post( stellyra::EventLoop::Task::create( [ &sender ]() { sender->sendData( 10 ); } ) );
	msleep( 20 );
	mainLoop.drain();
	EXPECT_EQ( receiver->callCount, 1 );

	workerLoop.post( stellyra::EventLoop::Task::create( [ &sender ]() { sender->sendData( 20 ); } ) );
	msleep( 20 );
	mainLoop.drain();
	EXPECT_EQ( receiver->callCount, 2 );

	// should not throw if no events are available to process
	mainLoop.drain();
	EXPECT_EQ( receiver->callCount, 2 );
}

TEST( ExternalEventLoop, MigrationPreservesPendingEvents )
{
	stellyra::EventLoop mainLoop;
	stellyra::EventLoop workerLoop;
	stellyra::AutoDrainThread workerDrainer( workerLoop );

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	workerLoop.post( stellyra::EventLoop::Task::create( [ &sender ]() { sender->sendData( 1 ); } ) );
	msleep( 30 );

	// migrate before draining
	stellyra::EventLoop mainLoop2;
	receiver->setEventLoop( &mainLoop2 );

	mainLoop.drain();   // should be empty - event migrated
	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop2.drain();  // pending event is here
	EXPECT_EQ( receiver->callCount, 1 );
}
