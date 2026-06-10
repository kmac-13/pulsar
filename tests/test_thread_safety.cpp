#include "test_helpers.hpp"
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Thread Safety
// ---------------------------------------------------------------------------

TEST( ThreadSafety, ConcurrentEmitAndDisconnect )
{
	auto button = std::make_shared< TestButton >();

	const int numHandlers = 20;
	const int numEmissions = 500;

	std::vector< std::shared_ptr< TestHandler > > handlers;
	for ( int i = 0; i < numHandlers; ++i )
	{
		auto h = std::make_shared< TestHandler >();
		handlers.push_back( h );
		button->clicked.connect( h, &TestHandler::onClicked );
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
					button->clicked.disconnect( handlers[ i ] );
				}

				msleep( 5 );

				for ( int i = start; i < start + 5 && i < (int)handlers.size(); ++i )
				{
					button->clicked.connect( handlers[ i ], &TestHandler::onClicked );
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

TEST( ThreadSafety, CrossThreadEmissionStress )
{
	auto button = std::make_shared< TestButton >();

	const int numLoops = 3;
	const int numEmissions = 500;

	std::vector< std::unique_ptr< pulsar::EventLoop > > loops;
	std::vector< std::shared_ptr< TestHandler > > handlers;

	for ( int i = 0; i < numLoops; ++i )
	{
		loops.push_back( std::make_unique< pulsar::EventLoop >( pulsar::EventLoop::makeAutoProcessed() ) );
		loops.back()->start();

		auto h = std::make_shared< TestHandler >();
		h->setEventLoop( loops.back().get() );
		handlers.push_back( h );

		button->clicked.connect( h, &TestHandler::onClicked, pulsar::ConnectionType::Auto );
	}

	std::vector< std::thread > emitThreads;
	for ( int t = 0; t < 3; ++t )
	{
		emitThreads.emplace_back( [ &button, numEmissions, t ]() {
			for ( int i = 0; i < numEmissions; ++i )
			{
				button->click( t, i );
				if ( i % 10 == 0 ) std::this_thread::yield();
			}
		} );
	}

	for ( auto& t : emitThreads ) t.join();

	msleep( 200 );

	for ( auto& loop : loops ) loop->stop();

	int expectedTotal = numEmissions * 3;
	for ( const auto& h : handlers )
	{
		EXPECT_GT( h->callCount, 0 );
		EXPECT_GE( h->callCount, (int)( expectedTotal * 0.8 ) );
		EXPECT_LE( h->callCount, expectedTotal );
	}
}

TEST( ThreadSafety, RapidConnectDisconnectStress )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

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
			if ( !running )
			{
				break;
			}
			button->clicked.connect( handler, &TestHandler::onClicked );
			connectCount++;
			msleep( 0 );
			button->clicked.disconnect( handler );
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

TEST( ThreadSafety, ConcurrentBlocking )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	auto conn = button->clicked.connect( handler, &TestHandler::onClicked );

	std::atomic< bool > running{ true };

	std::thread emitThread( [ &button, &running ]() {
		int n = 0;
		while ( running && n < 1000 )
		{
			button->click( n, n );
			++n;
			std::this_thread::yield();
		}
	} );

	std::thread blockThread( [ &conn, &running ]() {
		int iters = 0;
		while ( running && iters < 100 )
		{
			conn.block();
			std::this_thread::yield();
			conn.unblock();
			std::this_thread::yield();
			++iters;
		}
	} );

	msleep( 100 );
	running = false;

	emitThread.join();
	blockThread.join();

	// some events were received, some were blocked - both are fine
	EXPECT_GT( handler->callCount, 0 );
	EXPECT_LT( handler->callCount, 1000 );
}

TEST( ThreadSafety, EmitDisconnectRaceCondition )
{
	auto button = std::make_shared< TestButton >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( handler, &TestHandler::onClicked );

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
			button->clicked.disconnect( handler );
			button->clicked.connect( handler, &TestHandler::onClicked );
		}
	} );

	emitThread.join();
	churnThread.join();

	EXPECT_FALSE( crashed );
}
