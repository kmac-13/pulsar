#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>
#include <kmac/pulsar/auto_drain_thread.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

// ---------------------------------------------------------------------------
// Invokers
//
// Verifies how ConnectionType (Direct/Deferred/Auto) resolves against
// EventLoop topology: Direct always calls synchronously; Deferred posts to
// the receiver's EventLoop (dropped if it has none); Auto compares sender
// and receiver loops and picks Direct when they match, Deferred otherwise.
//
// AutoDrainThread wraps an EventLoop with a background thread that wakes on
// post() and drains it automatically, so a receiver's Deferred connections
// fire without the test manually calling loop.drain().
//
// InvokerReceiver's fields are atomic because Deferred/Auto-resolved-to-
// Deferred handlers run on the AutoDrainThread's background thread while
// the test reads them back on the main thread after only an msleep() -
// atomics (rather than a real happens-before edge from joining the drain
// thread) keep every read/write pair race-free without preventing the
// drain thread from continuing to run for the rest of the test.
//
// Most tests here use AutoDrainThread, which spawns a real background
// thread that eventually invokes the receiver's handler via
// sender.fired's own dispatch machinery - genuine cross-thread delivery,
// same category as test_sender_deferral.cpp.  This is exactly the use
// case SingleThreadedEvent's docs warn is undefined behaviour ("using it
// from more than one thread"), so this suite is restricted to the two
// thread-safe MutexTypes (Event, SharedEvent) and excludes
// SingleThreadedEvent, even though no two tests ever touch the same event
// *concurrently* (the delivery is still genuinely cross-thread).
// ---------------------------------------------------------------------------

namespace {

template< typename EventT >
class InvokerSender : public pulsar::Trackable
{
public:
	EventT fired{ this };
};

class InvokerReceiver : public pulsar::Trackable
{
public:
	std::atomic< int > last{ -1 };
	std::atomic< int > callCount{ 0 };
	void onFired( int v )
	{
		last.store( v, std::memory_order_relaxed );
		callCount.fetch_add( 1, std::memory_order_relaxed );
	}
};

} // namespace

template< typename EventT >
class Invokers : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SharedEvent< int > >;
TYPED_TEST_SUITE( Invokers, EventTypes );

TYPED_TEST( Invokers, DirectConnection )
{
	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;
	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Direct );
	sender.fired( 42 );
	EXPECT_EQ( receiver.last, 42 );
	EXPECT_EQ( receiver.callCount, 1 );
}

TYPED_TEST( Invokers, DeferredLoopSetBeforeConnect )
{
	pulsar::EventLoop loop;
	pulsar::AutoDrainThread drainer( loop );

	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;
	receiver.setEventLoop( &loop );

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Deferred );
	sender.fired( 100 );

	msleep( 50 );
	EXPECT_EQ( receiver.last, 100 );
	EXPECT_EQ( receiver.callCount, 1 );
}

TYPED_TEST( Invokers, DeferredNoLoopIsDropped )
{
	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Deferred );
	sender.fired( 200 );

	msleep( 10 );
	EXPECT_EQ( receiver.callCount, 0 );
	EXPECT_EQ( receiver.last, -1 );
}

TYPED_TEST( Invokers, DeferredLoopGainedAfterConnect )
{
	pulsar::EventLoop loop;
	pulsar::AutoDrainThread drainer( loop );

	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Deferred );

	// emit while no loop - should be dropped
	sender.fired( 300 );
	msleep( 10 );
	EXPECT_EQ( receiver.callCount, 0 );

	// assign loop - resolveInvokers() switches invokeNoOp -> invokeQueueDispatch
	receiver.setEventLoop( &loop );

	sender.fired( 301 );
	msleep( 50 );
	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.last, 301 );
}

TYPED_TEST( Invokers, DeferredLoopLostAfterConnect )
{
	pulsar::EventLoop loop;
	pulsar::AutoDrainThread drainer( loop );

	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;
	receiver.setEventLoop( &loop );

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Deferred );

	sender.fired( 400 );
	msleep( 50 );
	EXPECT_EQ( receiver.callCount, 1 );

	// remove the loop - resolveInvokers() switches invokeQueueDispatch -> invokeNoOp
	receiver.setEventLoop( nullptr );

	sender.fired( 401 );
	msleep( 10 );
	EXPECT_EQ( receiver.callCount, 1 );  // second emission dropped
	EXPECT_EQ( receiver.last, 400 );
}

TYPED_TEST( Invokers, AutoSameLoopInvokesDirect )
{
	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;
	// both on no loop -> Auto resolves to Direct

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Auto );
	sender.fired( 500 );
	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.last, 500 );
}

TYPED_TEST( Invokers, AutoDifferentLoopQueues )
{
	pulsar::EventLoop loop;
	pulsar::AutoDrainThread drainer( loop );

	InvokerSender< TypeParam > sender;
	InvokerReceiver receiver;
	receiver.setEventLoop( &loop );

	// sender has no loop -> receiverLoop != senderLoop -> queued

	sender.fired.connect( receiver, &InvokerReceiver::onFired, pulsar::ConnectionType::Auto );
	sender.fired( 600 );

	msleep( 50 );
	EXPECT_EQ( receiver.callCount, 1 );
	EXPECT_EQ( receiver.last, 600 );
}

TYPED_TEST( Invokers, MultipleDirectConnections )
{
	InvokerSender< TypeParam > sender;
	// unique_ptr for stable addresses: Trackable is non-copyable and
	// non-movable, so a vector of InvokerReceiver values would break on
	// reallocation
	std::vector< std::unique_ptr< InvokerReceiver > > receivers;
	for ( int i = 0; i < 20; ++i )
	{
		auto r = std::make_unique< InvokerReceiver >();
		sender.fired.connect( *r, &InvokerReceiver::onFired, pulsar::ConnectionType::Direct );
		receivers.push_back( std::move( r ) );
	}

	sender.fired( 700 );
	for ( const auto& r : receivers )
	{
		EXPECT_EQ( r->last, 700 );
		EXPECT_EQ( r->callCount,  1 );
	}
}

// ---------------------------------------------------------------------------
// AutoDrainThread::stop() drains anything still in flight, then cleanly
// detaches - the EventLoop reverts to needing a manual drain() afterward.
//
// Doesn't touch a pulsar::Event at all - pure EventLoop/AutoDrainThread
// mechanics - so stays a plain TEST() rather than running twice for no
// benefit under the typed suite.
// ---------------------------------------------------------------------------

TEST( InvokersUntyped, AutoDrainThreadStopClearsHook )
{
	pulsar::EventLoop loop;
	std::atomic< int > count{ 0 };

	{
		pulsar::AutoDrainThread drainer( loop );
		loop.post( pulsar::EventLoop::Task::create( [ &count ] { ++count; } ) );

		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
		while ( count.load() < 1 )
		{
			ASSERT_LT( std::chrono::steady_clock::now(), deadline ) << "timed out waiting for auto-drain";
			msleep( 1 );
		}

		drainer.stop();
		EXPECT_FALSE( drainer.isRunning() );
	}

	// after stop(), post() must not crash even with no drainer attached
	std::atomic< int > afterStop{ 0 };
	loop.post( pulsar::EventLoop::Task::create( [ &afterStop ] { ++afterStop; } ) );

	// nothing drains it automatically anymore - must be done manually
	EXPECT_EQ( afterStop.load(), 0 );
	loop.drain();
	EXPECT_EQ( afterStop.load(), 1 );
}

// ---------------------------------------------------------------------------
// stop() (and by extension the destructor) must drain the loop one final
// time before exiting, so a task posted right before stop() is called is
// not silently abandoned - regression test for a gap where threadLoop()
// checked _running before draining on wake, so the final wakeup triggered
// by stop() exited immediately without ever calling _loop.drain().
// ---------------------------------------------------------------------------

TEST( InvokersUntyped, AutoDrainThreadStopDrainsFinalPendingTask )
{
	pulsar::EventLoop loop;
	std::atomic< bool > ran{ false };

	pulsar::AutoDrainThread drainer( loop );

	// post and immediately stop, racing the drain thread's own wakeup
	// against stop()'s - if stop() doesn't force one final drain, this
	// task is never guaranteed to run before the thread exits
	loop.post( pulsar::EventLoop::Task::create( [ &ran ] { ran = true; } ) );
	drainer.stop();

	EXPECT_TRUE( ran.load() );
}

TEST( InvokersUntyped, AutoDrainThreadDestructorDrainsFinalPendingTask )
{
	pulsar::EventLoop loop;
	std::atomic< bool > ran{ false };

	{
		pulsar::AutoDrainThread drainer( loop );
		loop.post( pulsar::EventLoop::Task::create( [ &ran ] { ran = true; } ) );
		// drainer destroyed here - destructor calls stop()
	}

	EXPECT_TRUE( ran.load() );
}
