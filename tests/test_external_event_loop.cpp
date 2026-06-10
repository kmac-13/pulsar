#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Externally-managed EventLoop  (EventLoop::makeManualProcessed())
//
// No background thread is spawned.  The owner calls processEvents() to drain
// the queue - e.g. once per frame, after a platform event pump yields, etc.
//
// Auto connection resolution uses EventLoop pointer identity:
//   same loop instance  -> Direct (synchronous)
//   different instances -> Deferred (asynchronous to processEvents())
// ---------------------------------------------------------------------------

TEST( ExternalEventLoop, IsSelfThreadedFlag )
{
	auto selfThreaded = pulsar::EventLoop::makeAutoProcessed();
	EXPECT_TRUE( selfThreaded.isManagedInternally() );

	auto external = pulsar::EventLoop::makeManualProcessed();
	EXPECT_FALSE( external.isManagedInternally() );
}

TEST( ExternalEventLoop, EventsNotProcessedUntilDrained )
{
	auto mainLoop = pulsar::EventLoop::makeManualProcessed();
	auto workerLoop = pulsar::EventLoop::makeAutoProcessed();
	workerLoop.start();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	// different loops -> Auto resolves Deferred
	sender->dataReady.connect( receiver, &DataReceiver::processData );

	workerLoop.postEvent( [ &sender ]() { sender->sendData( 42 ); } );

	msleep( 30 );

	// not yet processed - nobody has called mainLoop.processEvents()
	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop.processEvents();

	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 42 );

	workerLoop.stop();
}

TEST( ExternalEventLoop, SameExternalLoopIsDirect )
{
	auto sharedLoop = pulsar::EventLoop::makeManualProcessed();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &sharedLoop );
	receiver->setEventLoop( &sharedLoop );

	// same loop instance -> Auto resolves Direct
	sender->dataReady.connect( receiver, &DataReceiver::processData );

	sender->sendData( 99 );

	// the sender has a loop, so emission is deferred to it even though
	// the connection resolves Direct (same loop instance); one
	// processEvents() call runs both the deferred emission and the handler
	sharedLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 99 );
}

TEST( ExternalEventLoop, MultipleEventsDrainedTogether )
{
	auto mainLoop = pulsar::EventLoop::makeManualProcessed();
	auto workerLoop = pulsar::EventLoop::makeAutoProcessed();
	workerLoop.start();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	for ( int i = 1; i <= 5; ++i )
	{
		workerLoop.postEvent( [ &sender, i ]() { sender->sendData( i ); } );
	}

	msleep( 50 );

	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop.processEvents();

	EXPECT_EQ( receiver->callCount, 5 );

	workerLoop.stop();
}

TEST( ExternalEventLoop, RepeatedTicks )
{
	auto mainLoop = pulsar::EventLoop::makeManualProcessed();
	auto workerLoop = pulsar::EventLoop::makeAutoProcessed();
	workerLoop.start();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	workerLoop.postEvent( [ &sender ]() { sender->sendData( 10 ); } );
	msleep( 20 );
	mainLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );

	workerLoop.postEvent( [ &sender ]() { sender->sendData( 20 ); } );
	msleep( 20 );
	mainLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 2 );

	// should not throw if no events are available to process
	mainLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 2 );

	workerLoop.stop();
}

TEST( ExternalEventLoop, StartStopAreNoOps )
{
	auto mainLoop = pulsar::EventLoop::makeManualProcessed();

	// an externally-managed EventLoop shouldn't throw when starting/stopping
	mainLoop.start();
	mainLoop.stop();
	mainLoop.start();
	mainLoop.stop();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();
	receiver->setEventLoop( &mainLoop );
	sender->dataReady.connect( receiver, &DataReceiver::processData );
	sender->sendData( 7 );
	mainLoop.processEvents();
	EXPECT_EQ( receiver->callCount, 1 );
}

TEST( ExternalEventLoop, MigrationPreservesPendingEvents )
{
	auto mainLoop = pulsar::EventLoop::makeManualProcessed();
	auto workerLoop = pulsar::EventLoop::makeAutoProcessed();
	workerLoop.start();

	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	sender->setEventLoop( &workerLoop );
	receiver->setEventLoop( &mainLoop );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	workerLoop.postEvent( [ &sender ]() { sender->sendData( 1 ); } );
	msleep( 30 );

	// migrate before draining
	auto mainLoop2 = pulsar::EventLoop::makeManualProcessed();
	receiver->setEventLoop( &mainLoop2 );

	mainLoop.processEvents();   // should be empty - event migrated
	EXPECT_EQ( receiver->callCount, 0 );

	mainLoop2.processEvents();  // pending event is here
	EXPECT_EQ( receiver->callCount, 1 );

	workerLoop.stop();
}
