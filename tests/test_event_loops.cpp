#include "test_helpers.hpp"

#include <kmac/pulsar/auto_drain_thread.h>
#include <kmac/pulsar/event_inspector.h>
#include <kmac/pulsar/event_loop.h>

// ---------------------------------------------------------------------------
// Event Loops
// ---------------------------------------------------------------------------

TEST( EventLoops, DeferredConnections )
{
	pulsar::EventLoop loop;
	pulsar::AutoDrainThread drainer( loop );

	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	handler->setEventLoop( &loop );

	button->clicked.connect( *handler, &TestHandler::onClicked, pulsar::ConnectionType::Auto );

	button->click( 100, 200 );

	msleep( 50 );
	drainer.stop();  // join, so the read below has a happens-before edge

	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 100 );
	EXPECT_EQ( handler->lastY, 200 );
}

TEST( EventLoops, ConnectionInfoInspection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );
	button->clicked.connect( *handler, &TestHandler::onClicked );
	button->clicked.connect( *handler, &TestHandler::onClicked );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	auto info = inspector.getEventInfo();

	EXPECT_EQ( info.connectionCount, 3u );
	EXPECT_EQ( info.activeConnectionCount, 3u );
	EXPECT_EQ( info.directConnectionCount, 3u );
	EXPECT_EQ( info.deferredConnectionCount, 0u );
	EXPECT_EQ( info.blockedConnectionCount, 0u );
}

TEST( EventLoops, MigrationWithPendingEvents )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	pulsar::EventLoop loop1;
	pulsar::AutoDrainThread drainer1( loop1 );
	receiver->setEventLoop( &loop1 );

	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	sender->sendData( 1 );
	sender->sendData( 2 );
	sender->sendData( 3 );

	msleep( 50 );

	pulsar::EventLoop loop2;
	pulsar::AutoDrainThread drainer2( loop2 );
	// any of sender's tasks still pending in loop1 migrate to loop2 here
	receiver->setEventLoop( &loop2 );

	sender->sendData( 4 );
	sender->sendData( 5 );

	msleep( 50 );
	drainer1.stop();  // join both, for a happens-before edge on the read below
	drainer2.stop();

	EXPECT_EQ( receiver->callCount, 5 );
	EXPECT_EQ( receiver->receivedValues.size(), 5u );
}

TEST( EventLoops, ManualProcessEvents )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	// manual mode - caller drives drain()
	pulsar::EventLoop loop;
	receiver->setEventLoop( &loop );

	sender->dataReady.connect( *receiver, &DataReceiver::processData );

	sender->sendData( 42 );

	loop.drain();

	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 42 );
}

TEST( EventLoops, StopWithPendingEvents )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	pulsar::EventLoop loop;
	{
		pulsar::AutoDrainThread drainer( loop );
		receiver->setEventLoop( &loop );

		for ( int i = 0; i < 100; ++i )
		{
			sender->sendData( i );
		}

		// drainer stops (and joins) here, at scope exit, possibly mid-queue
	}

	EXPECT_GE( receiver->callCount, 0 );
	EXPECT_LE( receiver->callCount, 100 );
}

TEST( EventLoops, MultipleEventLoops )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver1 = std::make_shared< DataReceiver >();
	auto receiver2 = std::make_shared< DataReceiver >();
	auto receiver3 = std::make_shared< DataReceiver >();

	pulsar::EventLoop loop1;
	pulsar::EventLoop loop2;
	pulsar::EventLoop loop3;
	pulsar::AutoDrainThread drainer1( loop1 );
	pulsar::AutoDrainThread drainer2( loop2 );
	pulsar::AutoDrainThread drainer3( loop3 );

	receiver1->setEventLoop( &loop1 );
	receiver2->setEventLoop( &loop2 );
	receiver3->setEventLoop( &loop3 );

	sender->dataReady.connect( *receiver1, &DataReceiver::processData );
	sender->dataReady.connect( *receiver2, &DataReceiver::processData );
	sender->dataReady.connect( *receiver3, &DataReceiver::processData );

	sender->sendData( 42 );

	msleep( 50 );
	drainer1.stop();
	drainer2.stop();
	drainer3.stop();

	EXPECT_EQ( receiver1->callCount, 1 );
	EXPECT_EQ( receiver2->callCount, 1 );
	EXPECT_EQ( receiver3->callCount, 1 );
}
