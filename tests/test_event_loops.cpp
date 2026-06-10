#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Event Loops
// ---------------------------------------------------------------------------

TEST( EventLoops, DeferredConnections )
{
	auto loop = pulsar::EventLoop::makeAutoProcessed();
	loop.start();

	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();
	handler->setEventLoop( &loop );

	button->clicked.connect( handler, &TestHandler::onClicked, pulsar::ConnectionType::Auto );

	button->click( 100, 200 );

	msleep( 50 );

	EXPECT_EQ( handler->callCount, 1 );
	EXPECT_EQ( handler->lastX, 100 );
	EXPECT_EQ( handler->lastY, 200 );

	loop.stop();
}

TEST( EventLoops, ConnectionInfoInspection )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( handler, &TestHandler::onClicked );
	button->clicked.connect( handler, &TestHandler::onClicked );
	button->clicked.connect( handler, &TestHandler::onClicked );

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

	auto loop1 = pulsar::EventLoop::makeAutoProcessed();
	loop1.start();
	receiver->setEventLoop( &loop1 );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	sender->sendData( 1 );
	sender->sendData( 2 );
	sender->sendData( 3 );

	msleep( 50 );

	auto loop2 = pulsar::EventLoop::makeAutoProcessed();
	loop2.start();
	receiver->setEventLoop( &loop2 );

	sender->sendData( 4 );
	sender->sendData( 5 );

	msleep( 50 );

	EXPECT_EQ( receiver->callCount, 5 );
	EXPECT_EQ( receiver->receivedValues.size(), 5u );

	loop1.stop();
	loop2.stop();
}

TEST( EventLoops, ManualProcessEvents )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	// external mode - caller drives processEvents()
	auto loop = pulsar::EventLoop::makeManualProcessed();
	receiver->setEventLoop( &loop );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	sender->sendData( 42 );

	loop.processEvents();

	EXPECT_EQ( receiver->callCount, 1 );
	EXPECT_EQ( receiver->lastValue, 42 );
}

TEST( EventLoops, StopWithPendingEvents )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver = std::make_shared< DataReceiver >();

	auto loop = pulsar::EventLoop::makeAutoProcessed();
	loop.start();
	receiver->setEventLoop( &loop );

	sender->dataReady.connect( receiver, &DataReceiver::processData );

	for ( int i = 0; i < 100; ++i )
	{
		sender->sendData( i );
	}

	loop.stop();

	EXPECT_GE( receiver->callCount, 0 );
	EXPECT_LE( receiver->callCount, 100 );
}

TEST( EventLoops, MultipleEventLoops )
{
	auto sender = std::make_shared< DataSender >();
	auto receiver1 = std::make_shared< DataReceiver >();
	auto receiver2 = std::make_shared< DataReceiver >();
	auto receiver3 = std::make_shared< DataReceiver >();

	auto loop1 = pulsar::EventLoop::makeAutoProcessed();
	auto loop2 = pulsar::EventLoop::makeAutoProcessed();
	auto loop3 = pulsar::EventLoop::makeAutoProcessed();
	loop1.start();
	loop2.start();
	loop3.start();

	receiver1->setEventLoop( &loop1 );
	receiver2->setEventLoop( &loop2 );
	receiver3->setEventLoop( &loop3 );

	sender->dataReady.connect( receiver1, &DataReceiver::processData );
	sender->dataReady.connect( receiver2, &DataReceiver::processData );
	sender->dataReady.connect( receiver3, &DataReceiver::processData );

	sender->sendData( 42 );

	msleep( 50 );

	EXPECT_EQ( receiver1->callCount, 1 );
	EXPECT_EQ( receiver2->callCount, 1 );
	EXPECT_EQ( receiver3->callCount, 1 );

	loop1.stop();
	loop2.stop();
	loop3.stop();
}
