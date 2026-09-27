#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>
#include <kmac/pulsar/auto_drain_thread.h>

#include <atomic>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// EventLoop - tested API surface directly
//
// isDraining(), isDrainingOnThread(), shouldDispatchDirectlyOnThread(), and
// migratePendingTo() are all exercised indirectly elsewhere (via
// Trackable::setEventLoop(), sender-context deferral, etc.), but never
// called directly in the test suite.  This file calls each of them directly,
// plus the retainCapacity=false constructor path, which is untested elsewhere.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// isDraining()
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, IsDrainingFalseBeforeAndAfterDrain )
{
	pulsar::EventLoop loop;
	EXPECT_FALSE( loop.isDraining() );

	loop.post( pulsar::EventLoop::Task::create( [] {} ) );
	loop.drain();

	EXPECT_FALSE( loop.isDraining() );
}

TEST( EventLoopDirectApi, IsDrainingTrueWhileTaskRuns )
{
	pulsar::EventLoop loop;
	bool observedDraining = false;

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &observedDraining ] {
		observedDraining = loop.isDraining();
	} ) );

	loop.drain();

	EXPECT_TRUE( observedDraining );
	EXPECT_FALSE( loop.isDraining() );  // cleared by the time drain() returns
}

TEST( EventLoopDirectApi, IsDrainingTrueForNestedPostedTaskToo )
{
	// a task posted from within another task, while drain() is still
	// iterating _active, should also observe isDraining()==true once it
	// eventually runs (drain() keeps looping until _pending is empty too)
	pulsar::EventLoop loop;
	bool outerSawDraining = false;
	bool innerSawDraining = false;

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &outerSawDraining, &innerSawDraining ] {
		outerSawDraining = loop.isDraining();
		loop.post( pulsar::EventLoop::Task::create( [ &loop, &innerSawDraining ] {
			innerSawDraining = loop.isDraining();
		} ) );
	} ) );

	loop.drain();

	EXPECT_TRUE( outerSawDraining );
	EXPECT_TRUE( innerSawDraining );
}

// ---------------------------------------------------------------------------
// isDrainingOnThread()
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, IsDrainingOnThreadFalseWhenIdle )
{
	pulsar::EventLoop loop;
	EXPECT_FALSE( loop.isDrainingOnThread( pulsar::platform::currentThreadId() ) );
}

TEST( EventLoopDirectApi, IsDrainingOnThreadTrueForTheDrainingThread )
{
	pulsar::EventLoop loop;
	bool matchedCurrentThread = false;

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &matchedCurrentThread ] {
		matchedCurrentThread = loop.isDrainingOnThread( pulsar::platform::currentThreadId() );
	} ) );

	loop.drain();
	EXPECT_TRUE( matchedCurrentThread );
}

TEST( EventLoopDirectApi, IsDrainingOnThreadFalseForDifferentThreadId )
{
	// drain on this (the test's) thread; from inside the running task, a
	// background thread's id must not match, even while draining is active
	pulsar::EventLoop loop;
	std::atomic< bool > matchedWrongThread{ true };  // start wrong, expect it flipped to false
	std::atomic< bool > wrongIdReady{ false };
	pulsar::platform::ThreadId otherThreadId{};

	std::thread other( [ &otherThreadId, &wrongIdReady ] {
		otherThreadId = pulsar::platform::currentThreadId();
		wrongIdReady = true;
		// keep the thread alive briefly so its id can't be reused before drain() checks it
		std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
	} );

	while ( ! wrongIdReady.load() )
	{
		std::this_thread::yield();
	}

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &otherThreadId, &matchedWrongThread ] {
		matchedWrongThread = loop.isDrainingOnThread( otherThreadId );
	} ) );

	loop.drain();
	other.join();

	EXPECT_FALSE( matchedWrongThread.load() );
}

// ---------------------------------------------------------------------------
// shouldDispatchDirectlyOnThread()
//
// True if EITHER the thread is registered via setDrainThread() (regardless
// of whether a drain() is active right now), OR the thread is, right now,
// actively running this loop's drain().  Tested independently, since either
// condition alone should be sufficient.
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, ShouldDispatchDirectlyFalseWithNoRegistrationAndNotDraining )
{
	pulsar::EventLoop loop;
	EXPECT_FALSE( loop.shouldDispatchDirectlyOnThread( pulsar::platform::currentThreadId() ) );
}

TEST( EventLoopDirectApi, ShouldDispatchDirectlyTrueWhenRegisteredEvenIfNotDraining )
{
	pulsar::EventLoop loop;
	loop.setDrainThread( pulsar::platform::currentThreadId() );

	// no drain() call at all - registration alone is enough
	EXPECT_TRUE( loop.shouldDispatchDirectlyOnThread( pulsar::platform::currentThreadId() ) );
}

TEST( EventLoopDirectApi, ShouldDispatchDirectlyTrueWhileDrainingEvenWithoutRegistration )
{
	pulsar::EventLoop loop;
	bool trueWhileDraining = false;

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &trueWhileDraining ] {
		trueWhileDraining = loop.shouldDispatchDirectlyOnThread( pulsar::platform::currentThreadId() );
	} ) );

	loop.drain();
	EXPECT_TRUE( trueWhileDraining );

	// neither condition holds once drain() has returned and nothing is registered
	EXPECT_FALSE( loop.shouldDispatchDirectlyOnThread( pulsar::platform::currentThreadId() ) );
}

TEST( EventLoopDirectApi, ShouldDispatchDirectlyFalseForUnregisteredDifferentThread )
{
	pulsar::EventLoop loop;
	loop.setDrainThread( pulsar::platform::currentThreadId() );

	std::atomic< bool > otherThreadDispatchesDirectly{ true };  // start wrong
	std::thread other( [ &loop, &otherThreadDispatchesDirectly ] {
		otherThreadDispatchesDirectly = loop.shouldDispatchDirectlyOnThread( pulsar::platform::currentThreadId() );
	} );
	other.join();

	EXPECT_FALSE( otherThreadDispatchesDirectly.load() );
}

// ---------------------------------------------------------------------------
// migratePendingTo() - called directly (rather than only indirectly via
// Trackable::setEventLoop()).
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, MigratePendingToMoveOnlyMatchingTag )
{
	pulsar::EventLoop src;
	pulsar::EventLoop dst;

	std::vector< int > order;

	src.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 1 ); } ), 100 );  // tag 100 - migrates
	src.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 2 ); } ), 200 );  // tag 200 - stays
	src.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 3 ); } ), 100 );  // tag 100 - migrates

	src.migratePendingTo( &dst, 100 );

	// tag-200 task stays behind and runs on src
	src.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 2 } ) );

	// tag-100 tasks now run on dst, in their original relative order
	order.clear();
	dst.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 1, 3 } ) );
}

TEST( EventLoopDirectApi, MigratePendingToAppendAfterExistingDestTasks )
{
	// migrated tasks land after whatever was already queued in dest -
	// migration does not reorder dest's own pre-existing pending tasks
	pulsar::EventLoop src;
	pulsar::EventLoop dst;
	std::vector< int > order;

	src.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 1 ); } ), 5 );
	dst.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 0 ); } ), 0 );

	src.migratePendingTo( &dst, 5 );

	dst.drain();
	// task 5 was posted before task 0, but tasks do not maintain relative
	// post times and migrate does not attempt to maintain relative ordering
	EXPECT_EQ( order, ( std::vector< int >{ 0, 1 } ) );
}

TEST( EventLoopDirectApi, MigratePendingToLeaveActiveTasksUntouched )
{
	// entries already swapped into _active (mid-drain) are not eligible for
	// migration - migratePendingTo() only ever looks at _pending
	pulsar::EventLoop src;
	pulsar::EventLoop dst;
	std::vector< int > order;

	src.post( pulsar::EventLoop::Task::create( [ &src, &dst, &order ] {
		order.push_back( 1 );  // this task is in _active right now

		// post a second task with the same tag while draining - lands in _pending
		src.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 2 ); } ), 42 );

		// migrating now must not touch the currently-executing _active batch,
		// only the newly (re-)pending one
		src.migratePendingTo( &dst, 42 );
	} ), 42 );

	src.drain();
	// the first task (already active when migratePendingTo() was called)
	// ran to completion on src, as part of the same drain() call
	EXPECT_EQ( order, ( std::vector< int >{ 1 } ) );

	order.clear();
	dst.drain();
	// the second task, still pending at the time of the call, migrated
	EXPECT_EQ( order, ( std::vector< int >{ 2 } ) );
}

TEST( EventLoopDirectApi, MigratePendingToNoOpsForNullDestSelfDestOrZeroTag )
{
	pulsar::EventLoop loop;
	std::vector< int > order;

	loop.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 1 ); } ), 7 );

	EXPECT_NO_THROW( loop.migratePendingTo( nullptr, 7 ) );
	EXPECT_NO_THROW( loop.migratePendingTo( &loop, 7 ) );  // dest == this
	EXPECT_NO_THROW( loop.migratePendingTo( &loop, 0 ) );  // 0 is the reserved "untagged" sentinel

	// task is still there, untouched by any of the above
	loop.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 1 } ) );
}

// ---------------------------------------------------------------------------
// migratePendingTo() invokes dest's post-notification hook whenever it
// actually migrates something - the same hook post() itself invokes after
// enqueueing.  Migrated tasks are pushed directly into dest._pending rather
// than routed through dest->post(), so without this, a destination loop
// with a waiting AutoDrainThread would have no way to learn migrated tasks
// arrived; it would stay parked until something else independently called
// post() or drain() on that loop.
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, MigratePendingToWakesAutoDrainThreadOnDestination )
{
	pulsar::EventLoop src;
	pulsar::EventLoop dst;
	pulsar::AutoDrainThread dstDrainer( dst );  // dst's drain thread is now parked, waiting

	std::atomic< bool > ran{ false };
	src.post( pulsar::EventLoop::Task::create( [ &ran ] { ran = true; } ), 55 );

	src.migratePendingTo( &dst, 55 );

	// no post() and no manual drain() call on dst from here - if the
	// migration didn't notify, the task would simply never run and this
	// would time out rather than fail cleanly
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 500 );
	while ( ! ran.load() )
	{
		ASSERT_LT( std::chrono::steady_clock::now(), deadline )
			<< "migrated task never ran - dst's drain thread was not woken";
		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}

	EXPECT_TRUE( ran.load() );
}

TEST( EventLoopDirectApi, MigratePendingToWithNothingMatchedLeavesTaskOnSource )
{
	// when the tag doesn't match anything, migratedAny is false and the
	// notify gate must not fire (there's nothing in dest to run anyway) -
	// the observable, reliable half of this is that the unmatched task
	// simply stays on src and runs there normally
	pulsar::EventLoop src;
	pulsar::EventLoop dst;
	pulsar::AutoDrainThread dstDrainer( dst );

	bool ran = false;
	src.post( pulsar::EventLoop::Task::create( [ &ran ] { ran = true; } ), 1 );  // tag 1

	src.migratePendingTo( &dst, 999 );  // no match - tag 1 != 999

	src.drain();
	EXPECT_TRUE( ran );  // task never left src
}

// ---------------------------------------------------------------------------
// migratePendingTo() and drain() genuinely contend for the same
// _pendingMutex on the source loop from different threads - deterministic
// proof via PULSAR_TEST_INJECT, rather than a statistical stress test that
// merely hopes the scheduler interleaves the two calls on some iteration.
// ---------------------------------------------------------------------------

namespace {
	std::atomic< bool >* migrateHookEntered = nullptr;
	std::chrono::milliseconds migrateHookSleep{ 0 };

	void migrateBlockingTestHook()
	{
		if ( migrateHookEntered )
		{
			migrateHookEntered->store( true, std::memory_order_release );
		}
		std::this_thread::sleep_for( migrateHookSleep );
	}
}

TEST( EventLoopDirectApi, MigratePendingToBlockConcurrentDrainOnSameSourceLoop )
{
	pulsar::EventLoop src;
	pulsar::EventLoop dst;

	constexpr uint64_t kMigrateTag = 1;
	constexpr uint64_t kStayTag = 2;
	std::atomic< int > totalRun{ 0 };

	// half tagged to migrate, half not - both halves must still run exactly
	// once each, regardless of which loop each one ends up draining on
	for ( int i = 0; i < 10; ++i )
	{
		uint64_t tag = ( i % 2 == 0 ) ? kMigrateTag : kStayTag;
		src.post( pulsar::EventLoop::Task::create( [ &totalRun ] {
			totalRun.fetch_add( 1, std::memory_order_relaxed );
		} ), tag );
	}

	std::atomic< bool > hookEntered{ false };
	migrateHookEntered = &hookEntered;
	migrateHookSleep = std::chrono::milliseconds( 150 );
	src.setMigrateTestHook( &migrateBlockingTestHook );

	std::thread migrator( [ & ] { src.migratePendingTo( &dst, kMigrateTag ); } );

	// wait until migrate has genuinely acquired the lock and is sitting
	// inside its injected sleep before attempting drain() below - this is
	// what makes drain()'s wait a deterministic proof of blocking rather
	// than a race that could resolve either way depending on scheduling
	while ( ! hookEntered.load( std::memory_order_acquire ) )
	{
		std::this_thread::yield();
	}

	auto drainStart = std::chrono::steady_clock::now();
	src.drain();  // must block on _pendingMutex until migrator releases it
	auto drainElapsed = std::chrono::steady_clock::now() - drainStart;

	migrator.join();
	src.setMigrateTestHook( nullptr );  // detach before src goes out of scope

	// drain() could not have completed its swap-check lock_guard until
	// migrate released _pendingMutex, so its own elapsed time should
	// reflect having waited out most of the injected sleep - a generous
	// margin (half) absorbs scheduling jitter while still failing outright
	// if drain() somehow proceeded without waiting at all
	EXPECT_GT( drainElapsed, migrateHookSleep / 2 );

	dst.drain();  // run whatever migrated

	// correctness invariant regardless of exact interleaving: every task
	// ran exactly once, total across both loops matches what was posted
	EXPECT_EQ( totalRun.load(), 10 );
}

// ---------------------------------------------------------------------------
// EventLoop(retainCapacity = false)
//
// With retainCapacity=false, drain() resets _active to a fresh
// default-constructed vector after each pass instead of clearing it in
// place - functionally equivalent either way, but a different code path
// (the "else" branch of drain()'s retainCapacity check).  The only
// externally observable difference is correctness across repeated drain
// cycles, since the internal vector strategy itself can't be inspected
// directly - exercised here from-empty and with nested posting (which
// causes drain()'s outer loop to swap _pending into _active, and reset it,
// more than once per drain() call).
// ---------------------------------------------------------------------------

TEST( EventLoopDirectApi, NoRetainCapacityConstructorDrainsCorrectly )
{
	pulsar::EventLoop loop( false );

	std::vector< int > order;
	loop.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 1 ); } ) );
	loop.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 2 ); } ) );

	loop.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 1, 2 } ) );

	// drain() with nothing pending must be a safe no-op (exercises the
	// "reset _active to a fresh vector" branch with an already-empty batch)
	EXPECT_NO_THROW( loop.drain() );

	// a second full post/drain cycle after _active was reset to a
	// default-constructed vector
	order.clear();
	loop.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 3 ); } ) );
	loop.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 3 } ) );
}

TEST( EventLoopDirectApi, NoRetainCapacityConstructorHandlesNestedPosting )
{
	// nested posting causes drain()'s outer while(true) loop to swap
	// _pending into _active more than once per drain() call - with
	// retainCapacity=false, _active is reset (not just cleared) between
	// those swaps too
	pulsar::EventLoop loop( false );
	std::vector< int > order;

	loop.post( pulsar::EventLoop::Task::create( [ &loop, &order ] {
		order.push_back( 1 );
		loop.post( pulsar::EventLoop::Task::create( [ &order ] { order.push_back( 2 ); } ) );
	} ) );

	loop.drain();
	EXPECT_EQ( order, ( std::vector< int >{ 1, 2 } ) );
}
