#include "test_helpers.hpp"

#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// RecordableEvent
// ---------------------------------------------------------------------------

TEST( RecordableEvent, BasicRecordAndReplay )
{
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	pulsar::RecordableEvent< int > event{ sender.get() };

	std::vector< int > received;
	event.connect( receiver, [ &received ]( int v ) { received.push_back( v ); } );

	event.recorder().startRecording();
	event( 10 );
	event( 20 );
	event( 30 );
	event.recorder().stopRecording();

	EXPECT_EQ( event.recorder().getRecordings().size(), 3u );

	received.clear();
	event.recorder().replay();

	EXPECT_EQ( received.size(), 3u );
	EXPECT_EQ( received[ 0 ], 10 );
	EXPECT_EQ( received[ 1 ], 20 );
	EXPECT_EQ( received[ 2 ], 30 );
}

TEST( RecordableEvent, RecordingLimit )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	// keep only last 3
	event.recorder().startRecording( 3 );
	for ( int i = 1; i <= 10; ++i )
	{
		event( i );
	}
	event.recorder().stopRecording();

	auto recordings = event.recorder().getRecordings();
	EXPECT_EQ( recordings.size(), 3u );
	EXPECT_EQ( std::get< 0 >( recordings[ 0 ].args ), 8 );
	EXPECT_EQ( std::get< 0 >( recordings[ 1 ].args ), 9 );
	EXPECT_EQ( std::get< 0 >( recordings[ 2 ].args ), 10 );
}

TEST( RecordableEvent, PauseAndResume )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	event.recorder().startRecording();
	event( 1 );
	event( 2 );

	event.recorder().pauseRecording();
	event( 3 );  // should NOT be recorded
	event( 4 );  // should NOT be recorded

	event.recorder().resumeRecording();
	event( 5 );
	event.recorder().stopRecording();

	auto recordings = event.recorder().getRecordings();
	EXPECT_EQ( recordings.size(), 3u );
	EXPECT_EQ( std::get< 0 >( recordings[ 0 ].args ), 1 );
	EXPECT_EQ( std::get< 0 >( recordings[ 1 ].args ), 2 );
	EXPECT_EQ( std::get< 0 >( recordings[ 2 ].args ), 5 );
}

TEST( RecordableEvent, StopClearsRecording )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	event.recorder().startRecording();
	event( 1 );
	event( 2 );
	event.recorder().stopRecording();
	EXPECT_EQ( event.recorder().getRecordings().size(), 2u );

	// starting a new recording clears the old one
	event.recorder().startRecording();
	event( 3 );
	event.recorder().stopRecording();
	EXPECT_EQ( event.recorder().getRecordings().size(), 1u );
	EXPECT_EQ( std::get< 0 >( event.recorder().getRecordings()[ 0 ].args ), 3 );
}

TEST( RecordableEvent, ReplayWithSpeed )
{
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	pulsar::RecordableEvent< int > event{ sender.get() };
	std::vector< int > received;
	event.connect( receiver, [ &received ]( int v ) { received.push_back( v ); } );

	event.recorder().startRecording();
	event( 1 );
	event( 2 );
	event( 3 );
	event.recorder().stopRecording();

	received.clear();
	// 10x speed - should still replay all events, just faster
	event.recorder().replayWithSpeed( 10.0 );

	EXPECT_EQ( received.size(), 3u );
	EXPECT_EQ( received[ 0 ], 1 );
	EXPECT_EQ( received[ 1 ], 2 );
	EXPECT_EQ( received[ 2 ], 3 );
}

TEST( RecordableEvent, TimingStats )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	event.recorder().startRecording();
	event( 1 );
	msleep( 10 );
	event( 2 );
	msleep( 10 );
	event( 3 );
	event.recorder().stopRecording();

	auto stats = event.recorder().getTimingStats();
	EXPECT_EQ( stats.count, 3u );
	EXPECT_GT( stats.totalDuration.count(), 0 );
	EXPECT_GT( stats.avgInterval.count(), 0 );
	EXPECT_GE( stats.maxInterval.count(), stats.minInterval.count() );
}

TEST( RecordableEvent, ExportToCSV )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	event.recorder().startRecording();
	event( 42 );
	event( 99 );
	event.recorder().stopRecording();

	// use a relative path so the file lands in the test's working directory
	// regardless of platform - avoids hardcoded /tmp which may not be writable
	const std::string path = "pulsar_test_export.csv";
	event.recorder().exportToCSV( path );

	std::ifstream f( path );
	ASSERT_TRUE( f.is_open() );

	std::string content(
		( std::istreambuf_iterator< char >( f ) ),
		std::istreambuf_iterator< char >() );
	f.close();
	std::remove( path.c_str() );  // clean up

	EXPECT_NE( content.find( "42" ), std::string::npos );
	EXPECT_NE( content.find( "99" ), std::string::npos );
}

TEST( RecordableEvent, DumpRecordings )
{
	auto sender = std::make_shared< pulsar::Object >();
	pulsar::RecordableEvent< int > event{ sender.get() };

	event.recorder().startRecording();
	event( 7 );
	event( 13 );
	event.recorder().stopRecording();

	std::ostringstream oss;
	event.recorder().dumpRecordings( oss );

	EXPECT_NE( oss.str().find( "7" ), std::string::npos );
	EXPECT_NE( oss.str().find( "13" ), std::string::npos );
}

TEST( RecordableEvent, PerformanceStatistics )
{
	auto sender = std::make_shared< pulsar::Object >();
	auto receiver = std::make_shared< pulsar::Object >();

	pulsar::RecordableEvent< int > event{ sender.get() };
	event.connect( receiver, []( int ) {} );

	event.enableStatistics();

	for ( int i = 0; i < 10; ++i )
	{
		event( i );
	}

	auto stats = event.getStatistics();
	EXPECT_EQ( stats.emissionCount, 10u );
	EXPECT_GT( stats.avgEmitTimeNs, 0u );
	EXPECT_EQ( stats.connectionCount, 1u );
}
