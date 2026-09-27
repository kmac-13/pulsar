#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>
#include <kmac/pulsar/auto_drain_thread.h>
#include <kmac/pulsar/event_inspector.h>

#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Thread Safety
// ---------------------------------------------------------------------------

// Every test here spawns real std::thread instances that connect, disconnect,
// block, and emit concurrently on the SAME event - the genuine multi-thread
// stress case.  Restricted to Event/SharedEvent (both real mutexes);
// SingleThreadedEvent's NullMutex is documented UB under real cross-thread
// access ("using it from more than one thread is undefined behaviour"), so
// it is deliberately excluded here rather than included and left to
// (possibly silently) misbehave.

template< typename MutexType >
class ThreadSafety : public ::testing::Test {};

using ThreadSafeMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex >;
TYPED_TEST_SUITE( ThreadSafety, ThreadSafeMutexTypes );

TYPED_TEST( ThreadSafety, ConcurrentEmitAndDisconnect )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();

	const int numHandlers = 20;
	const int numEmissions = 500;

	std::vector< std::shared_ptr< TestHandler > > handlers;
	for ( int i = 0; i < numHandlers; ++i )
	{
		auto h = std::make_shared< TestHandler >();
		handlers.push_back( h );
		button->clicked.connect( *h, &TestHandler::onClicked );
	}

	std::atomic< bool > running{ true };

	std::vector< std::thread > emitThreads;
	for ( int t = 0; t < 2; ++t )
	{
		emitThreads.emplace_back( [ &button, &running, numEmissions ]() {
			int n = 0;
			while ( running && n < numEmissions )
			{
				button->click( n % 100, n % 100 );
				++n;
				if ( n % 10 == 0 )
				{
					std::this_thread::yield();
				}
			}
		} );
	}

	std::vector< std::thread > disconnectThreads;
	for ( int t = 0; t < 2; ++t )
	{
		disconnectThreads.emplace_back( [ &button, &handlers, &running, t ]() {
			int iters = 0;
			while ( running && iters < 50 )
			{
				int start = t * 5;
				for ( int i = start; i < start + 5 && i < (int)handlers.size(); ++i )
				{
					button->clicked.disconnect( *handlers[ i ] );
				}

				msleep( 5 );

				for ( int i = start; i < start + 5 && i < (int)handlers.size(); ++i )
				{
					button->clicked.connect( *handlers[ i ], &TestHandler::onClicked );
				}

				++iters;
				std::this_thread::yield();
			}
		} );
	}

	msleep( 200 );
	running = false;

	for ( auto& t : emitThreads )
	{
		t.join();
	}
	for ( auto& t : disconnectThreads )
	{
		t.join();
	}

	// final emission must not crash
	EXPECT_NO_THROW( button->click( 999, 999 ) );

	int total = 0;
	for ( const auto& h : handlers )
	{
		total += h->callCount;
	}
	EXPECT_GT( total, 0 );
}

TYPED_TEST( ThreadSafety, CrossThreadEmissionStress )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();

	const int numLoops = 3;
	const int numEmissions = 500;

	struct LoopAndDrainer
	{
		pulsar::EventLoop loop;
		pulsar::AutoDrainThread drainer{ loop };
	};

	std::vector< std::unique_ptr< LoopAndDrainer > > loops;
	std::vector< std::shared_ptr< TestHandler > > handlers;

	for ( int i = 0; i < numLoops; ++i )
	{
		loops.push_back( std::make_unique< LoopAndDrainer >() );

		auto h = std::make_shared< TestHandler >();
		h->setEventLoop( &loops.back()->loop );
		handlers.push_back( h );

		button->clicked.connect( *h, &TestHandler::onClicked, pulsar::ConnectionType::Auto );
	}

	std::vector< std::thread > emitThreads;
	for ( int t = 0; t < 3; ++t )
	{
		emitThreads.emplace_back( [ &button, numEmissions, t ]() {
			for ( int i = 0; i < numEmissions; ++i )
			{
				button->click( t, i );
				if ( i % 10 == 0 )
				{
					std::this_thread::yield();
				}
			}
		} );
	}

	for ( auto& t : emitThreads )
	{
		t.join();
	}

	msleep( 200 );

	// explicitly destroy the loops now (rather than waiting for scope exit),
	// so each AutoDrainThread's destructor joins its background thread here;
	// std::thread::join() establishes a happens-before edge with everything
	// that thread did, which is what makes reading callCount below race-free
	loops.clear();

	int expectedTotal = numEmissions * 3;
	for ( const auto& h : handlers )
	{
		EXPECT_GT( h->callCount, 0 );
		EXPECT_GE( h->callCount, (int)( expectedTotal * 0.8 ) );
		EXPECT_LE( h->callCount, expectedTotal );
	}
}

TYPED_TEST( ThreadSafety, RapidConnectDisconnectStress )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	std::atomic< bool > running{ true };
	std::atomic< int > connectCount{ 0 };
	std::atomic< int > disconnectCount{ 0 };

	std::thread emitThread( [ &button, &running ]() {
		int n = 0;
		while ( running )
		{
			button->click( n, n );
			++n;
			std::this_thread::yield();
		}
	} );

	std::thread churnThread( [ &button, &handler, &running, &connectCount, &disconnectCount ]() {
		for ( int i = 0; i < 500; ++i )
		{
			if ( ! running )
			{
				break;
			}

			button->clicked.connect( *handler, &TestHandler::onClicked );
			connectCount++;
			msleep( 0 );
			button->clicked.disconnect( *handler );
			disconnectCount++;
			std::this_thread::yield();
		}
	} );

	msleep( 100 );
	running = false;
	emitThread.join();
	churnThread.join();

	EXPECT_GT( connectCount.load(), 50 );
	EXPECT_GT( disconnectCount.load(), 50 );

	// clean up and verify stable state
	for ( int i = 0; i < 5; ++i )
	{
		button->click( 999, 999 );
		msleep( 1 );
	}

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_LE( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( ThreadSafety, ConcurrentBlocking )
{
	// Connection-level block()/unblock() exists, but with only one
	// connection on this event, event-level block()/unblock() produces
	// the identical observable effect, so it stands in directly
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );

	// block upfront so the first batch of emissions is guaranteed to be
	// suppressed - avoids the race where the emit thread finishes all
	// iterations before the block thread is scheduled
	button->clicked.block();

	std::atomic< bool > running{ true };
	std::atomic< int > emitted{ 0 };

	std::thread emitThread( [ &button, &running, &emitted ]() {
		while ( running )
		{
			int n = emitted.fetch_add( 1, std::memory_order_relaxed );
			button->click( n, n );
			std::this_thread::yield();
		}
	} );

	// let a batch of emissions fire while blocked, then unblock
	msleep( 20 );
	button->clicked.unblock();
	msleep( 80 );
	running = false;

	emitThread.join();

	// some events were blocked (before unblock), some received (after) -
	// callCount must be strictly less than total emitted
	EXPECT_GT( handler->callCount, 0 );
	EXPECT_LT( handler->callCount, emitted.load() );
}

TYPED_TEST( ThreadSafety, EmitDisconnectRaceCondition )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );

	std::atomic< bool > crashed{ false };

	std::thread emitThread( [ &button, &crashed ]() {
		try
		{
			for ( int i = 0; i < 100000; ++i )
			{
				button->click( i, i );
			}
		}
		catch ( ... )
		{
			crashed = true;
		}
	} );

	std::thread churnThread( [ &button, &handler ]() {
		for ( int i = 0; i < 10000; ++i )
		{
			button->clicked.disconnect( *handler );
			button->clicked.connect( *handler, &TestHandler::onClicked );
		}
	} );

	emitThread.join();
	churnThread.join();

	EXPECT_FALSE( crashed );
}

TYPED_TEST( ThreadSafety, RapidConcurrentEmitAndChurn )
{
	// stress test: multiple threads emitting simultaneously while multiple
	// other threads are rapidly connecting and disconnecting overlapping
	// sets of handlers; validates that the locking model is correct under
	// genuine concurrent pressure - no crash, no deadlock, stable state
	// after the storm
	//
	// this is more aggressive than ConcurrentEmitAndDisconnect:
	//   - 4 emit threads vs 2
	//   - 4 churn threads vs 2, operating on overlapping handler sets
	//   - 50 handlers vs 20
	//   - churn threads reconnect immediately with no sleep between
	//     disconnect and reconnect, maximising the window where the
	//     connection list is being mutated concurrently with emission

	const int NUM_HANDLERS = 50;
	const int NUM_EMIT_THREADS = 4;
	const int NUM_CHURN_THREADS = 4;
	const int CHURN_ITERS = 200;
	const int EMIT_ITERS = 2000;

	auto button = std::make_unique< TestButtonT< TypeParam > >();

	std::vector< std::shared_ptr< TestHandler > > handlers;
	handlers.reserve( NUM_HANDLERS );
	for ( int i = 0; i < NUM_HANDLERS; ++i )
	{
		auto h = std::make_shared< TestHandler >();
		handlers.push_back( h );
		button->clicked.connect( *h, &TestHandler::onClicked );
	}

	std::atomic< bool > running{ true };
	std::atomic< int > totalEmissions{ 0 };

	// Emit threads: fire the event as fast as possible
	std::vector< std::thread > emitThreads;
	for ( int t = 0; t < NUM_EMIT_THREADS; ++t )
	{
		emitThreads.emplace_back( [ &button, &running, &totalEmissions, t, EMIT_ITERS ]() {
			int n = 0;
			while ( running && n < EMIT_ITERS )
			{
				button->click( t, n );
				totalEmissions.fetch_add( 1, std::memory_order_relaxed );
				++n;
			}
		} );
	}

	// churn threads: rapidly disconnect and reconnect overlapping handler
	// subsets with no delay between operations, maximising concurrent
	// mutation pressure; adjacent churn threads share handlers at the
	// boundary to exercise simultaneous disconnect of the same connection
	// from two threads (which must be safe - disconnect is idempotent)
	std::vector< std::thread > churnThreads;
	for ( int t = 0; t < NUM_CHURN_THREADS; ++t )
	{
		churnThreads.emplace_back( [ &button, &handlers, &running, t, NUM_HANDLERS, NUM_CHURN_THREADS, CHURN_ITERS ]() {
			// each churn thread owns a slice of handlers, with +/-2 overlap
			// at the boundaries to force simultaneous disconnect races
			int sliceSize = NUM_HANDLERS / NUM_CHURN_THREADS;
			int start = std::max( 0, t * sliceSize - 2 );
			int end = std::min( NUM_HANDLERS, ( t + 1 ) * sliceSize + 2 );

			for ( int iter = 0; iter < CHURN_ITERS && running; ++iter )
			{
				// disconnect all in slice
				for ( int i = start; i < end; ++i )
				{
					button->clicked.disconnect( *handlers[ i ] );
				}

				std::this_thread::yield();

				// reconnect all in slice
				for ( int i = start; i < end; ++i )
				{
					button->clicked.connect( *handlers[ i ], &TestHandler::onClicked );
				}

				std::this_thread::yield();
			}
		} );
	}

	// let it run for a bounded time regardless of iteration counts
	msleep( 300 );
	running = false;

	for ( auto& t : emitThreads )
	{
		t.join();
	}
	for ( auto& t : churnThreads )
	{
		t.join();
	}

	// post-storm: disconnect all, reconnect cleanly (exactly once each),
	// and verify the event is stable and delivers to all handlers
	button->clicked.disconnectAll();
	for ( auto& h : handlers )
	{
		button->clicked.connect( *h, &TestHandler::onClicked );
	}

	// record baseline counts after the storm (handlers may have been connected
	// multiple times by overlapping churn slices, giving varying counts)
	std::vector< int > baseCounts;
	baseCounts.reserve( handlers.size() );
	for ( const auto& h : handlers )
	{
		baseCounts.push_back( h->callCount );
	}

	// all handlers should receive exactly one post-storm emission
	button->click( 0, 0 );
	for ( int i = 0; i < (int)handlers.size(); ++i )
	{
		EXPECT_EQ( handlers[ i ]->callCount, baseCounts[ i ] + 1 )
			<< "Handler " << i << " did not receive exactly one post-storm emission";
	}

	// connection list should be stable with exactly NUM_HANDLERS active connections
	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, (size_t)NUM_HANDLERS );

	EXPECT_GT( totalEmissions.load(), 0 );
}
