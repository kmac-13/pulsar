#ifndef KMAC_PULSAR_EVENT_RECORDER_H
#define KMAC_PULSAR_EVENT_RECORDER_H

/**
 * @file event_recorder.h
 * @brief Capture, replay, and export triggered events.
 *
 * EventRecorder is the recording subsystem used by RecordableEvent.  It can
 * also be used standalone by calling setEvent() to attach it to any Event.
 *
 * Features:
 * - start / pause / resume / stop recording
 * - optional bounded-queue mode (keep only the last N emissions)
 * - replay at original timing, scaled speed, or instant
 * - export to CSV for offline analysis
 * - timing statistics (min/max/avg inter-emission intervals)
 *
 * All public methods are thread-safe.
 *
 * @code
 * pulsar::RecordableEvent< float > reading { this };
 *
 * reading.recorder().startRecording( 100 );   // keep last 100 emissions
 * // ... run sensor for a while ...
 * reading.recorder().stopRecording();
 * reading.recorder().exportToCSV( "sensor_log.csv" );
 * reading.recorder().replayWithSpeed( 2.0 );  // replay at 2x speed
 * @endcode
 */

#include <kmac/pulsar/pulsar_fwd.h>

#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace kmac {
namespace pulsar {

/**
 * @brief A single captured event trigger, arguments plus timing information.
 *
 * @tparam Args the argument types of the associated Event
 */
template< typename... Args >
struct EventRecord
{
	std::tuple< Args... > args;                       ///< the event arguments
	std::chrono::steady_clock::time_point timestamp;  ///< absolute wall-clock time of the event trigger
	std::chrono::nanoseconds relativeTime;            ///< time elapsed since recording started

	/**
	 * @brief Construct an EventRecord.
	 *
	 * @param arguments the trigger argument values
	 * @param ts absolute timestamp
	 * @param rt time since recording start
	 */
	EventRecord( Args... arguments, std::chrono::steady_clock::time_point ts, std::chrono::nanoseconds rt );
};

/**
 * @brief Summary of inter-emission timing.
 *
 * All durations are nanoseconds, maningful only when at least two
 * emissions have been recorded.
 */
struct TimingStats
{
	std::size_t count;                       ///< number of recorded events
	std::chrono::nanoseconds minInterval;    ///< shortest gap between consecutive events
	std::chrono::nanoseconds maxInterval;    ///< longest gap between consecutive events
	std::chrono::nanoseconds avgInterval;    ///< average gap between consecutive events
	std::chrono::nanoseconds totalDuration;  ///< relativeTime of the last recording
};

/**
 * @brief Records, replays, and exports event triggers.
 *
 * @tparam Args the argument types of the Event being recorded
 */
template< typename... Args >
class EventRecorder
{
private:
	Event< Args... >* _event;
	RecordingMode _mode;
	std::size_t _maxRecordings;                                 ///< 0 = unlimited
	std::chrono::steady_clock::time_point _recordingStartTime;
	std::deque< EventRecord< Args... > > _recordings;
	mutable std::mutex _mutex;

public:
	/**
	 * @brief Construct a recorder, optionally associated with an event.
	 *
	 * @param event Event to record and replay, may be nullptr; set later via setEvent()
	 */
	explicit EventRecorder( Event< Args... >* event = nullptr );

	/**
	 * @brief Destructor - stops recording if active.
	 */
	~EventRecorder();

	// ======================================================================
	// Recording Control
	// ======================================================================

	/**
	 * @brief Returns true if recording is currently active (not paused or stopped).
	 */
	bool isRecording() const;

	/**
	 * @brief Returns the current recording mode.
	 */
	RecordingMode mode() const;

	/**
	 * @brief Begin recording events.
	 *
	 * If recording is already active this is a no-op.  If recording was
	 * previously stopped, existing recordings are cleared before starting.
	 * If recording was paused, it resumes without clearing (use
	 * resumeRecording() to be explicit).
	 *
	 * @param maxRecordings maximum number of recordings to keep, when the limit is reached,
	 * the oldest recording is discarded (bounded-queue behaviour); pass 0 for unlimited
	 */
	void startRecording( size_t maxRecordings = 0 );

	/**
	 * @brief Temporarily suspend recording without discarding existing data.
	 *
	 * Only valid while recording.  Events triggered during a pause are not recorded.
	 * Call resumeRecording() to continue.
	 */
	void pauseRecording();

	/**
	 * @brief Resume a paused recording session.
	 *
	 * Only valid while paused.  Does not reset the start time.
	 */
	void resumeRecording();

	/**
	 * @brief Stop recording.
	 *
	 * Existing recordings are retained and can still be replayed or exported.
	 * Call clearRecordings() to discard them.
	 */
	void stopRecording();

	// ======================================================================
	// Recording Management
	// ======================================================================

	/**
	 * @brief Record one emission.
	 *
	 * Called automatically by RecordableEvent::triggerImpl() when recording
	 * is active.  Can also be called manually on a standalone recorder.
	 *
	 * Does nothing if not in Recording mode.
	 *
	 * @param args the argument values supplied during event trigger
	 */
	void recordEmission( Args... args );

	/**
	 * @brief Discard all recorded events.
	 */
	void clearRecordings();

	/**
	 * @brief Returns the number of recorded events.
	 */
	size_t recordingCount() const;

	/**
	 * @brief Return a copy of all recorded events.
	 *
	 * Thread-safe snapshot.
	 */
	std::deque< EventRecord< Args... > > getRecordings() const;

	// ======================================================================
	// Replay
	// ======================================================================

	/**
	 * @brief Replay all recordings immediately (no timing delays).
	 *
	 * Triggers the associated event once per recording in original order.
	 *
	 * @throws std::runtime_error if no event is associated
	 */
	void replay();

	/**
	 * @brief Replay recordings with the original inter-emission delays.
	 *
	 * Blocks the calling thread between events to preserve the original
	 * timing.
	 *
	 * @throws std::runtime_error if no event is associated
	 */
	void replayWithTiming();

	/**
	 * @brief Replay recordings at a scaled speed.
	 *
	 * Delays are divided by @p speedMultiplier: values > 1.0 speed up
	 * playback, values < 1.0 slow it down.
	 *
	 * @param speedMultiplier playback speed multiplier, must be > 0
	 *
	 * @throws std::runtime_error if no event is associated
	 * @throws std::invalid_argument if speedMultiplier <= 0
	 */
	void replayWithSpeed( double speedMultiplier );

	/**
	 * @brief Replay a subset of recordings immediately.
	 *
	 * @param startIdx inclusive start index (0-based)
	 * @param endIdx exclusive end index, clamped to recordingCount()
	 *
	 * @throws std::runtime_error if no event is associated
	 */
	void replayRange( size_t startIdx, size_t endIdx );

	// ======================================================================
	// Analysis & Export
	// ======================================================================

	/**
	 * @brief Compute inter-emission timing statistics.
	 *
	 * Returns zeros if fewer than two events have been recorded.
	 */
	TimingStats getTimingStats() const;

	/**
	 * @brief Print a human-readable listing of all recordings to @p out.
	 *
	 * Each recording is shown with its index, relative time (+Xms), and
	 * argument values.
	 *
	 * @param out any output stream
	 */
	template< typename OStream >
	void dumpRecordings( OStream& out ) const;

	/**
	 * @brief Write all recordings to a CSV file.
	 *
	 * Columns: Index, RelativeTime_ns, RelativeTime_ms, Arg0, Arg1, ...
	 *
	 * @param filename path to the output file
	 * @throws std::runtime_error if the file cannot be opened
	 */
	void exportToCSV( const std::string& filename ) const;

	/**
	 * @brief Associate this recorder with a different event.
	 *
	 * Useful when constructing a recorder standalone and later attaching it.
	 * Does not clear existing recordings.
	 *
	 * @param event new event to record and replay (may be nullptr to detach)
	 */
	void setEvent( Event< Args... >* event );

private:
	// -------------------------------------------------------------------------
	// Helpers for printing tuples
	// -------------------------------------------------------------------------

	template< typename OStream, typename Tuple, size_t... IndexSequence >
	void printTupleImpl( OStream& out, const Tuple& t, std::index_sequence< IndexSequence... > ) const;

	template< typename OStream, typename Tuple >
	void printTuple( OStream& out, const Tuple& t ) const;

	template< size_t... IndexSequence >
	std::string writeTupleCSVHeader( std::index_sequence< IndexSequence... > ) const;

	template< typename Tuple, size_t... IndexSequence >
	std::string writeTupleCSVImpl( const Tuple& t, std::index_sequence< IndexSequence... > ) const;

	template< typename Tuple >
	std::string writeTupleCSV( const Tuple& t ) const;
};

/**
 * @brief Alias for users that prefer signal/emit terminology.
 */
template< typename... Args >
using SignalRecorder = EventRecorder< Args... >;


//
// IMPLEMENTATION
//

template< typename... Args >
EventRecord< Args... >::EventRecord(
	Args... arguments,
	std::chrono::steady_clock::time_point ts,
	std::chrono::nanoseconds rt )
	: args( std::make_tuple( arguments... ) )
	, timestamp( ts )
	, relativeTime( rt )
{
}

template< typename... Args >
EventRecorder< Args... >::EventRecorder( Event< Args... >* event )
	: _event( event )
	, _mode( RecordingMode::Disabled )
	, _maxRecordings( 0 )
	, _recordingStartTime( std::chrono::steady_clock::now() )
{
}

template< typename... Args >
EventRecorder< Args... >::~EventRecorder()
{
	stopRecording();
}

template< typename... Args >
void EventRecorder< Args... >::startRecording( size_t maxRecordings )
{
	std::lock_guard< std::mutex > lock( _mutex );

	// check if already recording (no-op)
	if ( _mode == RecordingMode::Recording )
	{
		return;
	}

	if ( _mode == RecordingMode::Disabled )
	{
		// fresh start: clear any stale data from a previous session
		_recordings.clear();
	}

	_mode = RecordingMode::Recording;
	_maxRecordings = maxRecordings;
	_recordingStartTime = std::chrono::steady_clock::now();
}

template< typename... Args >
void EventRecorder< Args... >::pauseRecording()
{
	std::lock_guard< std::mutex > lock( _mutex );
	if ( _mode == RecordingMode::Recording )
	{
		_mode = RecordingMode::Paused;
	}
}

template< typename... Args >
void EventRecorder< Args... >::resumeRecording()
{
	std::lock_guard< std::mutex > lock( _mutex );
	if ( _mode == RecordingMode::Paused )
	{
		_mode = RecordingMode::Recording;
	}
}

template< typename... Args >
void EventRecorder< Args... >::stopRecording()
{
	std::lock_guard< std::mutex > lock( _mutex );
	_mode = RecordingMode::Disabled;
}

template< typename... Args >
bool EventRecorder< Args... >::isRecording() const
{
	std::lock_guard< std::mutex > lock( _mutex );
	return _mode == RecordingMode::Recording;
}

template< typename... Args >
RecordingMode EventRecorder< Args... >::mode() const
{
	std::lock_guard< std::mutex > lock( _mutex );
	return _mode;
}

template< typename... Args >
void EventRecorder< Args... >::recordEmission( Args... args )
{
	std::lock_guard< std::mutex > lock( _mutex );

	if ( _mode != RecordingMode::Recording )
	{
		return;
	}

	auto now = std::chrono::steady_clock::now();
	auto relative = std::chrono::duration_cast< std::chrono::nanoseconds >( now - _recordingStartTime );

	_recordings.emplace_back( args..., now, relative );

	// bounded-queue: drop oldest entry when limit is exceeded
	if ( _maxRecordings > 0 && _recordings.size() > _maxRecordings )
	{
		_recordings.pop_front();
	}
}

template< typename... Args >
void EventRecorder< Args... >::clearRecordings()
{
	std::lock_guard< std::mutex > lock( _mutex );
	_recordings.clear();
}

template< typename... Args >
std::size_t EventRecorder< Args... >::recordingCount() const
{
	std::lock_guard< std::mutex > lock( _mutex );
	return _recordings.size();
}

template< typename... Args >
std::deque< EventRecord< Args... > > EventRecorder< Args... >::getRecordings() const
{
	std::lock_guard< std::mutex > lock( _mutex );
	return _recordings;  // returns a copy
}

template< typename... Args >
void EventRecorder< Args... >::replay()
{
	if ( ! _event )
	{
		throw std::runtime_error( "Cannot replay: no event associated" );
	}

	// mutex protected
	std::deque< EventRecord< Args... > > recordings = getRecordings();

	for ( const auto& recording : recordings )
	{
		std::apply( [ this ]( Args... args ) {
			( *_event )( std::forward< Args >( args )... );
		}, recording.args );
	}
}

template< typename... Args >
void EventRecorder< Args... >::replayWithTiming()
{
	if ( ! _event )
	{
		throw std::runtime_error( "Cannot replay: no event associated" );
	}

	// mutex protected
	std::deque< EventRecord< Args... > > recordings = getRecordings();

	if ( recordings.empty() )
	{
		return;
	}

	auto startTime = std::chrono::steady_clock::now();

	for ( const auto& recording : recordings )
	{
		auto targetTime = startTime + recording.relativeTime;
		std::this_thread::sleep_until( targetTime );

		std::apply( [ this ]( Args... args ) {
			( *_event )( std::forward< Args >( args )... );
		}, recording.args );
	}
}

template< typename... Args >
void EventRecorder< Args... >::replayWithSpeed( double speedMultiplier )
{
	if ( ! _event )
	{
		throw std::runtime_error( "Cannot replay: no event associated" );
	}

	if ( speedMultiplier <= 0.0 )
	{
		throw std::invalid_argument( "Speed multiplier must be positive" );
	}

	// mutex protected
	std::deque< EventRecord< Args... > > recordings = getRecordings();

	if ( recordings.empty() )
	{
		return;
	}

	auto startTime = std::chrono::steady_clock::now();

	for ( const auto& recording : recordings )
	{
		auto scaledTime = std::chrono::duration_cast< std::chrono::nanoseconds >( recording.relativeTime / speedMultiplier );
		auto targetTime = startTime + scaledTime;
		std::this_thread::sleep_until( targetTime );

		std::apply( [ this ]( Args... args ) {
			( *_event )( std::forward< Args >( args )... );
		}, recording.args );
	}
}

template< typename... Args >
void EventRecorder< Args... >::replayRange( size_t startIdx, size_t endIdx )
{
	if ( ! _event )
	{
		throw std::runtime_error( "Cannot replay: no event associated" );
	}

	std::deque< EventRecord< Args... > > recordings;
	{
		std::lock_guard< std::mutex > lock( _mutex );

		if ( endIdx > _recordings.size() )
		{
			endIdx = _recordings.size();
		}

		if ( startIdx >= endIdx )
		{
			return;
		}

		recordings.assign( _recordings.begin() + startIdx, _recordings.begin() + endIdx );
	}

	for ( const auto& recording : recordings )
	{
		std::apply( [ this ]( Args... args ) {
			( *_event )( std::forward< Args >( args )... );
		}, recording.args );
	}
}

template< typename... Args >
TimingStats EventRecorder< Args... >::getTimingStats() const
{
	std::lock_guard< std::mutex > lock( _mutex );

	TimingStats stats;
	stats.count = _recordings.size();

	if ( _recordings.empty() )
	{
		stats.minInterval = std::chrono::nanoseconds( 0 );
		stats.maxInterval = std::chrono::nanoseconds( 0 );
		stats.avgInterval = std::chrono::nanoseconds( 0 );
		stats.totalDuration = std::chrono::nanoseconds( 0 );
		return stats;
	}

	stats.totalDuration = _recordings.back().relativeTime;

	if ( _recordings.size() < 2 )
	{
		stats.minInterval = std::chrono::nanoseconds( 0 );
		stats.maxInterval = std::chrono::nanoseconds( 0 );
		stats.avgInterval = std::chrono::nanoseconds( 0 );
		return stats;
	}

	std::chrono::nanoseconds minInterval = std::chrono::nanoseconds::max();
	std::chrono::nanoseconds maxInterval = std::chrono::nanoseconds::min();
	std::chrono::nanoseconds totalInterval( 0 );

	for ( size_t i = 1; i < _recordings.size(); ++i )
	{
		auto interval = _recordings[ i ].relativeTime - _recordings[ i - 1 ].relativeTime;
		minInterval = std::min( minInterval, interval );
		maxInterval = std::max( maxInterval, interval );
		totalInterval += interval;
	}

	stats.minInterval = minInterval;
	stats.maxInterval = maxInterval;
	stats.avgInterval = totalInterval / ( _recordings.size() - 1 );

	return stats;
}

template< typename... Args >
template< typename OStream >
void EventRecorder< Args... >::dumpRecordings( OStream& out ) const
{
	std::lock_guard< std::mutex > lock( _mutex );

	out << "Event Recordings (" << _recordings.size() << " total)\n";
	out << "================================================================================\n\n";

	if ( _recordings.empty() )
	{
		out << "No recordings\n";
		return;
	}

	for ( size_t i = 0; i < _recordings.size(); ++i )
	{
		const auto& rec = _recordings[ i ];
		auto ms = std::chrono::duration_cast< std::chrono::milliseconds >( rec.relativeTime );

		out << "Recording #" << ( i + 1 ) << ":\n";
		out << "  Time: +" << ms.count() << "ms\n";
		out << "  Args: ";
		printTuple( out, rec.args );
		out << "\n\n";
	}
}

template< typename... Args >
void EventRecorder< Args... >::setEvent( Event< Args... >* event )
{
	_event = event;
}

template< typename... Args >
void EventRecorder< Args... >::exportToCSV( const std::string& filename ) const
{
	std::lock_guard< std::mutex > lock( _mutex );

	std::ofstream file( filename );
	if ( ! file.is_open() )
	{
		throw std::runtime_error( "Failed to open file: " + filename );
	}

	file << "Index,RelativeTime_ns,RelativeTime_ms";
	file << writeTupleCSVHeader( std::index_sequence_for< Args... >{} );
	file << "\n";

	for ( size_t i = 0; i < _recordings.size(); ++i )
	{
		const auto& rec = _recordings[ i ];
		auto ns = rec.relativeTime.count();
		auto ms = std::chrono::duration_cast< std::chrono::milliseconds >( rec.relativeTime ).count();

		file << i << "," << ns << "," << ms;
		file << writeTupleCSV( rec.args );
		file << "\n";
	}
}

template< typename... Args >
template< typename OStream, typename Tuple, size_t... IndexSequence >
void EventRecorder< Args... >::printTupleImpl( OStream& out, const Tuple& t, std::index_sequence< IndexSequence... > ) const
{
	out << "(";
	( ( out << ( IndexSequence == 0 ? "" : ", " ) << std::get< IndexSequence >( t ) ), ... );
	out << ")";
}

template< typename... Args >
template< typename OStream, typename Tuple >
void EventRecorder< Args... >::printTuple( OStream& out, const Tuple& t ) const
{
	printTupleImpl( out, t, std::make_index_sequence< std::tuple_size< Tuple >::value >{} );
}

template< typename... Args >
template< size_t... IndexSequence >
std::string EventRecorder< Args... >::writeTupleCSVHeader( std::index_sequence< IndexSequence... > ) const
{
	std::ostringstream oss;
	( ( oss << ",Arg" << IndexSequence ), ... );
	return oss.str();
}

template< typename... Args >
template< typename Tuple, size_t... IndexSequence >
std::string EventRecorder< Args... >::writeTupleCSVImpl( const Tuple& t, std::index_sequence< IndexSequence... > ) const
{
	std::ostringstream oss;
	( ( oss << "," << std::get< IndexSequence >( t ) ), ... );
	return oss.str();
}

template< typename... Args >
template< typename Tuple >
std::string EventRecorder< Args... >::writeTupleCSV( const Tuple& t ) const
{
	return writeTupleCSVImpl( t, std::make_index_sequence< std::tuple_size< Tuple >::value >{} );
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_RECORDER_H
