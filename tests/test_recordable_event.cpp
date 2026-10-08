#include "test_helpers.h"

#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// RecordableEvent
//
// Verifies recording, replay, pause/resume, timing statistics, and export
// on top of an ordinary Event: a RecordableEvent behaves like any other
// event for connect()/operator(), but its recorder() can capture emitted
// arguments and later replay them against whatever is currently connected.
//
// BasicRecordableEvent<MutexType, Args...> is parametrized on MutexType the
// same way BasicEvent is (RecordableEvent/SharedRecordableEvent/
// SingleThreadedRecordableEvent are the matching aliases).  No test here does
// reentrant connect/disconnect from within its own dispatch, and no test
// spawns real threads, so all three MutexType variants are safe.
// ---------------------------------------------------------------------------

template< typename MutexType >
class RecordableEvent : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	stellyra::platform::RecursiveMutex,
	stellyra::platform::SharedMutex,
	stellyra::platform::NullMutex >;
TYPED_TEST_SUITE( RecordableEvent, MutexTypes );

TYPED_TEST( RecordableEvent, BasicRecordAndReplay )
{
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

	std::vector< int > received;
	event.connectLambda( receiver, [ &received ]( int v ) { received.push_back( v ); } );

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

TYPED_TEST( RecordableEvent, RecordingLimit )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

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

TYPED_TEST( RecordableEvent, PauseAndResume )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

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

TYPED_TEST( RecordableEvent, StartClearsPreviousRecording )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

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

// Tests that the timing gaps between event triggers are preserved and
// included as part of the replay.
TYPED_TEST( RecordableEvent, ReplayWithTiming )
{
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };
	std::vector< int > received;
	event.connectLambda( receiver, [ &received ]( int v ) { received.push_back( v ); } );

	event.recorder().startRecording();
	event( 1 );
	msleep( 60 );
	event( 2 );
	msleep( 60 );
	event( 3 );
	event.recorder().stopRecording();

	auto totalDuration = event.recorder().getTimingStats().totalDuration;
	ASSERT_GT( totalDuration.count(), 0 );

	received.clear();
	auto start = std::chrono::steady_clock::now();
	event.recorder().replayWithTiming();
	auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_EQ( received, ( std::vector< int >{ 1, 2, 3 } ) );

	// replayWithTiming() preserves the ORIGINAL pacing (no scaling) - elapsed
	// wall-clock time should land close to totalDuration, not near-instant
	// (which would mean the recorded timing was ignored rather than
	// actually respected), and not wildly longer either
	EXPECT_GT( elapsed, totalDuration / 2 );
	EXPECT_LT( elapsed, totalDuration * 2 );
}

// Tests that the timing gaps between event triggers are preserved and
// included as part of the replay, but adjusted according to the speed factor.
// A speed factor of 0.5 means half the speed, so each event will take twice
// as long to trigger after the previous event compared to the original timing,
// and a speed factor of 2.0 means twice the speed, so each event will take half
// as long to trigger after the previous event compared to the original timing.
TYPED_TEST( RecordableEvent, ReplayWithSpeed )
{
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };
	std::vector< int > received;
	event.connectLambda( receiver, [ &received ]( int v ) { received.push_back( v ); } );

	event.recorder().startRecording();
	event( 1 );
	msleep( 60 );
	event( 2 );
	msleep( 60 );
	event( 3 );
	event.recorder().stopRecording();

	// real elapsed time recorded, not just three back-to-back calls with
	// nothing to scale - this is what makes the speed check below meaningful
	auto totalDuration = event.recorder().getTimingStats().totalDuration;
	ASSERT_GT( totalDuration.count(), 0 );

	received.clear();
	auto start = std::chrono::steady_clock::now();
	event.recorder().replayWithSpeed( 10.0 );
	auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_EQ( received, ( std::vector< int >{ 1, 2, 3 } ) );

	// at 10x speed, replay should take roughly totalDuration/10 - bounds are
	// generous to absorb scheduling jitter (particularly on Windows, where
	// default timer resolution is coarse), while still catching a broken or
	// inverted speed multiplier: comfortably faster than an unscaled (1x)
	// replay would be, and not suspiciously close to zero, which would mean
	// the recorded timing was ignored rather than actually scaled
	EXPECT_LT( elapsed, totalDuration / 2 );
	EXPECT_GT( elapsed, totalDuration / 20 );
}

TYPED_TEST( RecordableEvent, TimingStats )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

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

TYPED_TEST( RecordableEvent, ExportToCSV )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

	event.recorder().startRecording();
	event( 42 );
	event( 99 );
	event.recorder().stopRecording();

	// use a relative path so the file lands in the test's working directory
	// regardless of platform - avoids hardcoded /tmp which may not be writable
	const std::string path = "stellyra_test_export.csv";
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

TYPED_TEST( RecordableEvent, DumpRecordings )
{
	stellyra::Trackable sender;
	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };

	event.recorder().startRecording();
	event( 7 );
	event( 13 );
	event.recorder().stopRecording();

	std::ostringstream oss;
	event.recorder().dumpRecordings( oss );

	EXPECT_NE( oss.str().find( "7" ), std::string::npos );
	EXPECT_NE( oss.str().find( "13" ), std::string::npos );
}

TYPED_TEST( RecordableEvent, PerformanceStatistics )
{
	stellyra::Trackable sender;
	stellyra::Trackable receiver;

	stellyra::BasicRecordableEvent< TypeParam, int > event{ &sender };
	event.connectLambda( receiver, []( int ) {} );

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
