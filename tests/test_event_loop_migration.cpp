#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>

#include <chrono>
#include <vector>

// ---------------------------------------------------------------------------
// EventLoop task migration
//
// Trackable::setEventLoop() migrates any of that receiver's pending deferred
// tasks from its old loop to its new one.  These tests dig into the edge
// cases of that guarantee: what happens to a task already mid-drain when
// migration occurs, whether migration preserves cross-event posting order,
// and whether it only ever touches the migrating receiver's own tasks when
// several receivers share a loop.
//
// Sender owns its own EventLoop, so every emission is itself sender-deferred
// first (queued to s.loop, since nothing is registered as its drain thread)
// before the connection's own resolution (Direct/Deferred/None) is even
// considered.  Draining s.loop is therefore a required step after every
// s.ev(...) call below, separate from draining whichever loop the receiver
// ends up posted to.
// ---------------------------------------------------------------------------

namespace {

template< typename EventT >
struct Sender : pulsar::Trackable
{
	pulsar::EventLoop loop;
	EventT ev{ this };
	Sender() { setEventLoop( &loop ); }
};

struct Recv : pulsar::Trackable
{
	std::vector< int > received;
	void onEvent( int v ) { received.push_back( v ); }
};

} // namespace

// ---------------------------------------------------------------------------
// Cross-MutexType parity: sender-side deferral, migration, and priority
// ordering under Deferred dispatch all behave identically for Event,
// SharedEvent, and SingleThreadedEvent.  No test in this file spawns real
// threads or uses once(), so all three MutexType variants are safe here.
// ---------------------------------------------------------------------------

template< typename EventT >
class EventLoopMigration : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SharedEvent< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( EventLoopMigration, EventTypes );

TYPED_TEST( EventLoopMigration, PendingTaskMovesToNewLoopBeforeDrain )
{
	Sender< TypeParam > s;
	pulsar::EventLoop oldLoop, newLoop;

	Recv r;
	r.setEventLoop( &oldLoop );
	s.ev.template connect< &Recv::onEvent >( r );

	s.ev( 0 );       // sender-side deferral: task first lands in s.loop
	s.loop.drain();  // now the connection resolves and posts to oldLoop

	// migrate before oldLoop is ever drained
	r.setEventLoop( &newLoop );

	oldLoop.drain();
	EXPECT_TRUE( r.received.empty() );  // task was migrated out

	newLoop.drain();
	ASSERT_EQ( r.received.size(), 1u );
	EXPECT_EQ( r.received[ 0 ], 0 );
}

TYPED_TEST( EventLoopMigration, ActiveDrainBatchIsNotMigrated )
{
	// a task already pulled into the active drain batch stays on the old
	// loop even if migration happens mid-drain, from within another task in
	// that same batch - migration only affects the *pending* queue
	Sender< TypeParam > s;
	pulsar::EventLoop oldLoop, newLoop;

	Recv r;
	r.setEventLoop( &oldLoop );
	s.ev.template connect< &Recv::onEvent >( r );

	s.ev( 0 );       // sender-side deferral: task first lands in s.loop
	s.loop.drain();  // now the connection resolves and posts to oldLoop, not yet drained

	bool migrated = false;
	oldLoop.post( pulsar::EventLoop::Task::create( [ & ]() {
		r.setEventLoop( &newLoop );  // migrate mid-drain
		migrated = true;
	} ) );

	oldLoop.drain();
	EXPECT_TRUE( migrated );
	// the original task was already in the active batch when migration
	// happened, so it completed on oldLoop
	ASSERT_EQ( r.received.size(), 1u );
	EXPECT_EQ( r.received[ 0 ], 0 );

	newLoop.drain();
	EXPECT_EQ( r.received.size(), 1u );  // nothing left to migrate
}

TYPED_TEST( EventLoopMigration, PreservesInterleavedPostingOrderAcrossEvents )
{
	// a receiver connected to two different events on the same sender:
	// tasks posted in interleaved order must still drain in that same
	// order after migration, not grouped by event
	struct TwoEventSender : pulsar::Trackable
	{
		pulsar::EventLoop loop;
		TypeParam e1{ this };
		TypeParam e2{ this };
		TwoEventSender() { setEventLoop( &loop ); }
	};

	TwoEventSender s;
	pulsar::EventLoop oldLoop, newLoop;

	std::vector< int > fired;
	pulsar::Trackable receiver;
	receiver.setEventLoop( &oldLoop );

	s.e1.connectLambda( receiver, [ &fired ]( int v ) { fired.push_back( v ); } );
	s.e2.connectLambda( receiver, [ &fired ]( int v ) { fired.push_back( v ); } );

	s.e1( 1 );
	s.e2( 2 );
	s.e1( 3 );
	s.loop.drain();  // sender-side deferral: resolve all three, posting to oldLoop

	receiver.setEventLoop( &newLoop );

	oldLoop.drain();
	EXPECT_TRUE( fired.empty() );

	newLoop.drain();
	EXPECT_EQ( fired, ( std::vector< int >{ 1, 2, 3 } ) );
}

TYPED_TEST( EventLoopMigration, OnlyMigratingReceiversTasksMove )
{
	// two receivers share one loop; only one of them migrates, its
	// counterpart's pending task must stay put
	Sender< TypeParam > s;
	pulsar::EventLoop sharedLoop, newLoop;

	struct NamedRecv : pulsar::Trackable
	{
		std::vector< std::string >* log;
		std::string name;
		void on( int ) { log->push_back( name ); }
	};

	std::vector< std::string > fired;
	NamedRecv r1, r2;
	r1.log = r2.log = &fired;
	r1.name = "r1";
	r2.name = "r2";

	r1.setEventLoop( &sharedLoop );
	r2.setEventLoop( &sharedLoop );

	s.ev.template connect< &NamedRecv::on >( r1 );
	s.ev.template connect< &NamedRecv::on >( r2 );

	s.ev( 0 );  // sender-side deferral: resolve both, posting one task per receiver to sharedLoop
	s.loop.drain();

	r1.setEventLoop( &newLoop );  // only r1 migrates

	sharedLoop.drain();
	EXPECT_EQ( fired, ( std::vector< std::string >{ "r2" } ) );

	newLoop.drain();
	EXPECT_EQ( fired, ( std::vector< std::string >{ "r2", "r1" } ) );
}

TYPED_TEST( EventLoopMigration, DeferredBecomesNoneWhenReceiverLoopCleared )
{
	Sender< TypeParam > s;
	pulsar::EventLoop rcvLoop;
	Recv r;
	r.setEventLoop( &rcvLoop );
	s.ev.template connect< &Recv::onEvent >( r );

	s.ev( 1 );
	s.loop.drain();
	rcvLoop.drain();
	ASSERT_EQ( r.received.size(), 1u );

	// clearing the receiver's loop mid-flight resolves future Auto
	// connections to None - dropped, not crashed
	r.setEventLoop( nullptr );
	s.ev( 2 );
	s.loop.drain();
	rcvLoop.drain();
	EXPECT_EQ( r.received.size(), 1u );  // unchanged - dropped

	// restoring the loop brings Deferred delivery back
	r.setEventLoop( &rcvLoop );
	s.ev( 3 );
	s.loop.drain();
	rcvLoop.drain();
	ASSERT_EQ( r.received.size(), 2u );
	EXPECT_EQ( r.received[ 1 ], 3 );
}

// ---------------------------------------------------------------------------
// EventLoop::drain() reentrancy
//
// These two don't touch an Event/TypeParam at all - pure EventLoop
// mechanics - so they stay plain TEST()s rather than running 3x for no
// benefit under the typed suite.
// ---------------------------------------------------------------------------

TEST( EventLoopMigrationUntyped, NestedDrainCallIsANoOp )
{
	// drain() calling itself reentrantly (e.g. a posted task calls
	// drain() on its own loop) must be a safe no-op, not double-process
	// the queue or deadlock - guarded by the same CAS that also protects
	// concurrent cross-thread drain() calls
	pulsar::EventLoop loop;
	int count = 0;

	loop.post( pulsar::EventLoop::Task::create( [ & ]() {
		++count;
		loop.drain();  // reentrant - must do nothing
		++count;
	} ) );
	loop.post( pulsar::EventLoop::Task::create( [ & ]() { ++count; } ) );

	loop.drain();
	EXPECT_EQ( count, 3 );
}

TEST( EventLoopMigrationUntyped, DrainThreadRegistrationAccessors )
{
	pulsar::EventLoop loop;
	EXPECT_FALSE( loop.hasDrainThread() );

	loop.setDrainThread( pulsar::platform::currentThreadId() );
	EXPECT_TRUE( loop.hasDrainThread() );
	EXPECT_EQ( loop.drainThread(), pulsar::platform::currentThreadId() );

	loop.clearDrainThread();
	EXPECT_FALSE( loop.hasDrainThread() );
}

// ---------------------------------------------------------------------------
// Priority ordering still applies when dispatch is Deferred
// ---------------------------------------------------------------------------

TYPED_TEST( EventLoopMigration, PriorityAppliesUnderDeferredDispatch )
{
	Sender< TypeParam > s;
	pulsar::EventLoop receiverLoop;

	struct OrderRecv : pulsar::Trackable
	{
		int id;
		std::vector< int >* order;
		explicit OrderRecv( int i, std::vector< int >* o ) : id( i ), order( o ) {}
		void on( int ) { order->push_back( id ); }
	};

	std::vector< int > order;
	OrderRecv lo( 1, &order ), hi( 10, &order );
	lo.setEventLoop( &receiverLoop );
	hi.setEventLoop( &receiverLoop );

	s.ev.template connect< &OrderRecv::on >( lo, 1u );
	s.ev.template connect< &OrderRecv::on >( hi, 10u );

	s.ev( 0 );
	EXPECT_TRUE( order.empty() );  // deferred - not yet run
	s.loop.drain();                // sender-side deferral resolves, posting to receiverLoop
	EXPECT_TRUE( order.empty() );  // still not run - only posted to receiverLoop

	receiverLoop.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 10, 1 } ) );
}

// ---------------------------------------------------------------------------
// Auto re-resolves to Direct once sender and receiver loops match, even if
// the connection was originally made while they differed
// ---------------------------------------------------------------------------

TYPED_TEST( EventLoopMigration, ReResolvesToDirectWhenSenderLoopChangesToMatch )
{
	pulsar::Anchor senderAnchor;
	TypeParam ev{ &senderAnchor };
	pulsar::EventLoop sharedLoop, senderLoop;

	Recv r;
	r.setEventLoop( &sharedLoop );

	// sender starts on a different loop -> Deferred
	senderAnchor.setEventLoop( &senderLoop );
	ev.template connect< &Recv::onEvent >( r );

	ev( 1 );
	EXPECT_TRUE( r.received.empty() );
	senderLoop.drain();  // sender-side deferral resolves; connection is Deferred -> posts to sharedLoop
	EXPECT_TRUE( r.received.empty() );
	sharedLoop.drain();
	ASSERT_EQ( r.received.size(), 1u );

	// move the sender onto the receiver's loop -> connection now resolves to
	// Direct, but sender-side deferral is a separate, earlier gate that
	// still applies regardless of the connection's own resolution - nobody
	// is registered/draining sharedLoop at this exact moment, so the
	// emission itself still defers once, and the handler fires as part of
	// that same drain (immediately, with no further posting to itself)
	senderAnchor.setEventLoop( &sharedLoop );
	ev( 2 );
	EXPECT_EQ( r.received.size(), 1u );  // not yet - sender-deferred first
	sharedLoop.drain();
	ASSERT_EQ( r.received.size(), 2u );
	EXPECT_EQ( r.received[ 1 ], 2 );
}

// ---------------------------------------------------------------------------
// Multiple deferred handlers on the same emission each receive their own
// correctly-captured copy of the arguments
// ---------------------------------------------------------------------------

TYPED_TEST( EventLoopMigration, SharedArgumentCaptureAcrossDeferredHandlers )
{
	Sender< TypeParam > s;
	pulsar::EventLoop receiverLoop;

	struct MultiRecv : pulsar::Trackable
	{
		std::vector< int >* vals;
		void onA( int v ) { vals->push_back( v * 10 ); }
		void onB( int v ) { vals->push_back( v * 100 ); }
	};

	std::vector< int > vals;
	MultiRecv r;
	r.vals = &vals;
	r.setEventLoop( &receiverLoop );

	s.ev.template connect< &MultiRecv::onA >( r );
	s.ev.template connect< &MultiRecv::onB >( r );

	s.ev( 7 );
	EXPECT_TRUE( vals.empty() );
	s.loop.drain();  // sender-side deferral resolves, posting to receiverLoop
	EXPECT_TRUE( vals.empty() );

	receiverLoop.drain();
	ASSERT_EQ( vals.size(), 2u );
	EXPECT_EQ( vals[ 0 ], 70 );
	EXPECT_EQ( vals[ 1 ], 700 );
}
