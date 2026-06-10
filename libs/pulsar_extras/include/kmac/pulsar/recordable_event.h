#ifndef KMAC_PULSAR_RECORDABLE_EVENT_H
#define KMAC_PULSAR_RECORDABLE_EVENT_H

/**
 * @file recordable_event.h
 * @brief Event with optional recording and statistics.
 *
 * RecordableEvent<Args...> extends Event<Args...> with two opt-in capabilities.
 * Both are inactive by default and have minimal overhead when not in use, but
 * RecordableEvent always carries a larger memory footprint than Event regardless
 * of whether recording or statistics are enabled.  For production code where size
 * and the per-emission check overhead matter, use plain Event<Args...>.
 *
 * - **Recording**:
 *   capture every emission (arguments + timestamp) for later replay or export;
 *   accessed through recorder()
 * - **Statistics**:
 *   track emission count and cumulative emit time; accessed through getStatistics()
 *
 * Use RecordableEvent during development, debugging, and testing.  Switch back
 * to plain Event for release builds or performance-critical paths where the
 * virtual dispatch overhead of triggerImpl() matters.
 *
 * @code
 * class Sensor : public pulsar::Object
 * {
 * public:
 *     pulsar::RecordableEvent<float> reading{this};
 *
 *     void measure(float value) {
 *         reading.enableStatistics();
 *         reading.recorder().startRecording();
 *         reading(value);
 *     }
 * };
 *
 * auto stats = sensor->reading.getStatistics();
 * sensor->reading.recorder().exportToCSV("readings.csv");
 * @endcode
 *
 * @see EventRecorder for the full recording/replay API
 */

#include <kmac/pulsar/event.h>

#include "event_inspector.h"
#include "event_recorder.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace kmac {
namespace pulsar {

/**
 * @brief Statistics snapshot collected by RecordableEvent.
 */
struct RecordingStatistics
{
	uint64_t emissionCount;          ///< total number of emissions since statistics were enabled
	uint64_t totalEmitTimeNs;        ///< cumulative time spent in triggerImpl(), in nanoseconds
	double avgEmitTimeNs;            ///< average time per emission, in nanoseconds
	size_t connectionCount;          ///< total entries in the connection list at snapshot time
	size_t directConnectionCount;    ///< number of Direct connections at snapshot time
	size_t deferredConnectionCount;  ///< number of Deferred connections at snapshot time
};

/**
 * @brief Event with optional emission recording and performance statistics.
 *
 * @tparam Args argument types forwarded to every connected handler
 *
 * @see Event for the base event without recording/statistics overhead
 * @see EventRecorder for the recording and replay API
 */
template< typename... Args >
class RecordableEvent : public Event< Args... >
{
private:
	std::atomic< bool > _statisticsEnabled;
	std::atomic< uint64_t > _emissionCount;
	std::atomic< uint64_t > _totalEmitTimeNs;
	EventRecorder< Args... > _recorder;

public:
	/**
	 * @brief Construct a RecordableEvent belonging to @p sender.
	 *
	 * Both statistics and recording are disabled by default.
	 *
	 * @param sender the Object that owns this event (usually @c this)
	 */
	RecordableEvent( Object* sender );

	// ======================================================================
	// Recording / Replay API
	// ======================================================================

	/**
	 * @brief Returns a reference to the event's recorder.
	 *
	 * Use the recorder to start/stop recording, replay emissions, and export
	 * data to CSV.
	 *
	 * @see EventRecorder
	 */
	EventRecorder< Args... >& recorder();

	/**
	 * @brief Returns a const reference to the recorder.
	 */
	const EventRecorder< Args... >& recorder() const;

	// ======================================================================
	// Statistics
	// ======================================================================

	/**
	 * @brief Returns true if statistics collection is currently enabled.
	 */
	bool statisticsEnabled() const;

	/**
	 * @brief Enable or disable statistics collection.
	 *
	 * Enabling resets the emission count and accumulated time to zero.
	 * Disabling leaves the counters at their last values until the next enable.
	 *
	 * @param enable true to enable, false to disable
	 */
	void enableStatistics( bool enable = true );

	/**
	 * @brief Return a snapshot of the current statistics.
	 *
	 * Also captures connection counts from the base event at snapshot time.
	 * Returns zeros if statistics are not enabled.
	 */
	RecordingStatistics getStatistics() const;

	/**
	 * @brief Reset emission count and accumulated time to zero.
	 */
	void resetStatistics();

	// ======================================================================
	// Enhanced summary, including stats and recording state
	// ======================================================================

	/**
	 * @brief Return a multi-section summary string.
	 *
	 * Includes connection info, and conditionally statistics and recording
	 * state if they are active.
	 */
	std::string getSummary() const;

protected:
	/**
	 * @brief Overrides Event::triggerImpl() to add recording and statistics.
	 *
	 * Records the emission if recording is active, times the base class
	 * triggerImpl() if statistics are enabled, then delegates to
	 * Event::triggerImpl() for actual dispatch.
	 */
	void triggerImpl( Args... args ) override;
};

/**
 * @brief Alias for users that prefer signal/emit terminology.
 */
template< typename... Args >
using RecordableSignal = RecordableEvent< Args... >;


//
// IMPLEMENTATION
//

template< typename... Args >
RecordableEvent< Args... >::RecordableEvent( Object* sender )
	: Event< Args... >( sender )
	, _statisticsEnabled( false )
	, _emissionCount( 0 )
	, _totalEmitTimeNs( 0 )
	, _recorder( this )
{
}

template< typename... Args >
EventRecorder< Args... >& RecordableEvent< Args... >::recorder()
{
	return _recorder;
}

template< typename... Args >
const EventRecorder< Args... >& RecordableEvent< Args... >::recorder() const
{
	return _recorder;
}

template< typename... Args >
bool RecordableEvent< Args... >::statisticsEnabled() const
{
	return _statisticsEnabled;
}

template< typename... Args >
void RecordableEvent< Args... >::enableStatistics( bool enable )
{
	_statisticsEnabled = enable;
	if ( enable )
	{
		// reset counters when enabling so stats reflect the period after this call
		_emissionCount.store( 0, std::memory_order_relaxed );
		_totalEmitTimeNs.store( 0, std::memory_order_relaxed );
	}
}

template< typename... Args >
RecordingStatistics RecordableEvent< Args... >::getStatistics() const
{
	RecordingStatistics stats;
	stats.emissionCount = _emissionCount.load( std::memory_order_relaxed );
	stats.totalEmitTimeNs = _totalEmitTimeNs.load( std::memory_order_relaxed );
	stats.avgEmitTimeNs = stats.emissionCount > 0
		? static_cast< double >( stats.totalEmitTimeNs ) / stats.emissionCount
		: 0.0;

	// also snapshot current connection counts from the base event
	auto info = EventInspector< Args... >( const_cast< RecordableEvent& >( *this ) ).getEventInfo();
	stats.connectionCount = info.connectionCount;
	stats.directConnectionCount = info.directConnectionCount;
	stats.deferredConnectionCount = info.deferredConnectionCount;

	return stats;
}

template< typename... Args >
void RecordableEvent< Args... >::resetStatistics()
{
	_emissionCount.store( 0, std::memory_order_relaxed );
	_totalEmitTimeNs.store( 0, std::memory_order_relaxed );
}

template< typename... Args >
std::string RecordableEvent< Args... >::getSummary() const
{
	auto info = EventInspector< Args... >( const_cast< RecordableEvent& >( *this ) ).getEventInfo();

	std::ostringstream oss;
	oss << "RecordableEvent Summary:\n";
	oss << "  Address: " << info.eventAddress << "\n";
	oss << "  Type: " << info.eventTypeName << "\n";
	oss << "  Sender: " << info.senderTypeName << "\n";
	oss << "\nConnections:\n";
	oss << "  Total: " << info.connectionCount << "\n";
	oss << "  Active: " << info.activeConnectionCount << "\n";
	oss << "  Blocked: " << info.blockedConnectionCount << "\n";
	oss << "  Direct: " << info.directConnectionCount << "\n";
	oss << "  Deferred: " << info.deferredConnectionCount << "\n";

	if ( _statisticsEnabled )
	{
		auto stats = getStatistics();
		oss << "\nStatistics:\n";
		oss << "  Emissions: " << stats.emissionCount << "\n";
		oss << "  Total Time: " << stats.totalEmitTimeNs << " ns\n";
		oss << "  Avg Time: " << std::fixed << std::setprecision( 2 ) << stats.avgEmitTimeNs << " ns\n";
	}

	if ( _recorder.mode() != RecordingMode::Disabled )
	{
		oss << "\nRecording:\n";
		oss << "  Status: ";
		switch( _recorder.mode() )
		{
		case RecordingMode::Recording:
			oss << "Recording";
			break;
		case RecordingMode::Paused:
			oss << "Paused";
			break;
		default:
			oss << "Disabled";
			break;
		}
		oss << "\n";
		oss << "  Recordings: " << _recorder.recordingCount() << "\n";

		if ( _recorder.recordingCount() > 0 )
		{
			auto timingStats = _recorder.getTimingStats();
			oss << "  Duration: " << timingStats.totalDuration.count() << " ns\n";
			if ( timingStats.count > 1 )
			{
				oss << "  Avg Interval: " << timingStats.avgInterval.count() << " ns\n";
				oss << "  Min Interval: " << timingStats.minInterval.count() << " ns\n";
				oss << "  Max Interval: " << timingStats.maxInterval.count() << " ns\n";
			}
		}
	}

	return oss.str();
}

template< typename... Args >
void RecordableEvent< Args... >::triggerImpl( Args... args )
{
	// record the emission before dispatching so the recording includes all calls
	if ( _recorder.isRecording() )
	{
		_recorder.recordEmission( args... );
	}

	// only query the clock when statistics are actually enabled
	auto startTime = _statisticsEnabled
		? std::chrono::high_resolution_clock::now()
		: std::chrono::high_resolution_clock::time_point();

	Event< Args... >::triggerImpl( std::forward< Args >( args )... );

	if ( _statisticsEnabled )
	{
		auto endTime = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast< std::chrono::nanoseconds >( endTime - startTime );

		_emissionCount.fetch_add( 1, std::memory_order_relaxed );
		_totalEmitTimeNs.fetch_add( duration.count(), std::memory_order_relaxed );
	}
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_RECORDABLE_EVENT_H
