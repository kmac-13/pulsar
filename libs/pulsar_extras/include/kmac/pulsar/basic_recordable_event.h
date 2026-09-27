#pragma once
#ifndef KMAC_PULSAR_BASIC_RECORDABLE_EVENT_H
#define KMAC_PULSAR_BASIC_RECORDABLE_EVENT_H

/**
 * @file basic_recordable_event.h
 * @brief Event with optional recording and statistics.
 *
 * BasicRecordableEvent<MutexType, Args...> presents the same public API as
 * BasicEvent<MutexType, Args...> but adds two opt-in capabilities:
 *
 *   - **Recording**: capture every emission (arguments + timestamp) for later
 *     replay or export; accessed through recorder()
 *   - **Statistics**: track emission count and cumulative emit time; accessed
 *     through getStatistics()
 *
 * Both are inactive by default.  BasicRecordableEvent always carries a larger
 * memory footprint than BasicEvent regardless of whether either feature is
 * in use.  For production code where size and the per-emission overhead
 * matter, use plain Event<Args...>.
 *
 * Use BasicRecordableEvent during development, debugging, and testing.  The
 * compile-time swap between Event and RecordableEvent is transparent because
 * both expose identical connect / disconnect / operator() / trigger / emit
 * APIs via the shared EventStorage base:
 *
 * @code
 * class Sensor : public Trackable
 * {
 * public:
 *     RecordableEvent< float > reading { this };
 *
 *     void measure( float value )
 *     {
 *         reading.enableStatistics();
 *         reading.recorder().startRecording();
 *         reading( value );
 *     }
 * };
 *
 * auto stats = sensor.reading.getStatistics();
 * sensor.reading.recorder().exportToCSV( "readings.csv" );
 * @endcode
 *
 * @see EventRecorder for the full recording/replay API
 * @see RecordingStatistics for the statistics snapshot type
 */

#include <kmac/pulsar/basic_event_impl.h>
#include <kmac/pulsar/event_storage.h>

#include "event_recorder.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

namespace kmac {
namespace pulsar {

/**
 * @brief Statistics snapshot collected by BasicRecordableEvent.
 */
struct RecordingStatistics
{
	uint64_t emissionCount;               ///< total emissions since statistics were enabled
	uint64_t totalEmitTimeNs;             ///< cumulative dispatch time, in nanoseconds
	double avgEmitTimeNs;                 ///< average dispatch time per emission, in nanoseconds
	std::size_t connectionCount;          ///< total slot count at snapshot time (includes inactive slots)
	std::size_t activeConnectionCount;    ///< live connections at snapshot time
	std::size_t directConnectionCount;    ///< Direct connections at snapshot time
	std::size_t deferredConnectionCount;  ///< Deferred connections at snapshot time
};

/**
 * @brief Event with optional emission recording and performance statistics.
 *
 * Inherits all connection management from EventStorage and adds
 * operator() / trigger() / emit() with recording and statistics hooks.
 *
 * @tparam MutexType synchronisation strategy (same as BasicEvent)
 * @tparam Args argument types forwarded to connected handlers
 */
template< typename MutexType, typename... Args >
class BasicRecordableEvent : public EventStorage< kmac::pulsar::BasicEventImpl, MutexType, Args... >
{
	using Base = EventStorage< kmac::pulsar::BasicEventImpl, MutexType, Args... >;

private:
	std::atomic< bool > _statisticsEnabled { false };
	std::atomic< uint64_t > _emissionCount { 0 };
	std::atomic< uint64_t > _totalEmitTimeNs { 0 };
	EventRecorder< Args... > _recorder;

public:
	/**
	 * @brief Construct with no owner.
	 *
	 * Both statistics and recording are disabled by default.
	 */
	BasicRecordableEvent();

	/**
	 * @brief Construct with an owner Trackable.
	 *
	 * Both statistics and recording are disabled by default.
	 *
	 * @param owner the Trackable that owns this event (usually @c this)
	 */
	explicit BasicRecordableEvent( Trackable* owner );

	~BasicRecordableEvent() = default;

	BasicRecordableEvent( const BasicRecordableEvent& ) = delete;
	BasicRecordableEvent& operator=( const BasicRecordableEvent& ) = delete;
	BasicRecordableEvent( BasicRecordableEvent&& ) = default;
	BasicRecordableEvent& operator=( BasicRecordableEvent&& ) = default;

	// =========================================================================
	// triggering
	// =========================================================================

	/**
	 * @brief Trigger the event - record emission, update statistics, then dispatch.
	 *
	 * Recording and statistics are only active when explicitly enabled;
	 * the overhead when both are off is a single atomic bool load per
	 * emission.  Silently dropped while the event is blocked.
	 */
	void operator()( Args... args );

	/// Identical to operator().
	void trigger( Args... args );

	/// Identical to operator().
	void emit( Args... args );

	// =========================================================================
	// recording API
	// =========================================================================

	/**
	 * @brief Returns a reference to the event's recorder.
	 *
	 * Use the recorder to start/stop recording, replay emissions, and
	 * export data to CSV.
	 *
	 * @see EventRecorder
	 */
	EventRecorder< Args... >& recorder();

	/**
	 * @brief Returns a const reference to the recorder.
	 */
	const EventRecorder< Args... >& recorder() const;

	// =========================================================================
	// statistics API
	// =========================================================================

	/**
	 * @brief Returns true if statistics collection is currently enabled.
	 */
	bool statisticsEnabled() const;

	/**
	 * @brief Enable or disable statistics collection.
	 *
	 * Enabling resets emission count and accumulated time to zero.
	 * Disabling leaves the counters at their last values.
	 *
	 * @param enable true to enable, false to disable
	 */
	void enableStatistics( bool enable = true );

	/**
	 * @brief Return a snapshot of the current statistics.
	 *
	 * Also captures connection counts from the slot table at snapshot time.
	 * Returns zeros if statistics are not enabled.
	 */
	RecordingStatistics getStatistics() const;

	/**
	 * @brief Reset emission count and accumulated time to zero.
	 */
	void resetStatistics();

	// =========================================================================
	// enhanced summary
	// =========================================================================

	/**
	 * @brief Return a multi-section summary string.
	 *
	 * Includes connection state, and conditionally statistics and recording
	 * state if either is active.
	 */
	std::string getSummary() const;
};

//
// IMPLEMENTATION
//

template< typename MutexType, typename... Args >
inline BasicRecordableEvent< MutexType, Args... >::BasicRecordableEvent()
{
	// wire up replay so recorder().replay() fires back through this event
	_recorder.setReplayTarget( Callable< void( Args... ) >::create( [ this ]( Args... args ) {
		operator()( std::forward< Args >( args )... );
	} ) );
}

template< typename MutexType, typename... Args >
inline BasicRecordableEvent< MutexType, Args... >::BasicRecordableEvent( Trackable* owner )
	: Base( owner )
{
	_recorder.setReplayTarget( Callable< void( Args... ) >::create( [ this ]( Args... args ) {
		operator()( std::forward< Args >( args )... );
	} ) );
}


// ===========================================================================
// operator() / trigger / emit
// ===========================================================================

template< typename MutexType, typename... Args >
inline void BasicRecordableEvent< MutexType, Args... >::operator()( Args... args )
{
	if ( this->_impl->isBlocked() )
	{
		return;
	}

	// record before dispatch so the recording captures every call, including
	// ones that produce no handler invocations
	if ( _recorder.isRecording() )
	{
		_recorder.recordEmission( args... );
	}

	// only query the clock when statistics are actually enabled
	const bool statsOn = _statisticsEnabled.load( std::memory_order_relaxed );
	std::chrono::high_resolution_clock::time_point startTime;
	if ( statsOn )
	{
		startTime = std::chrono::high_resolution_clock::now();
	}

	EventLoop* senderLoop = this->_impl->owner ? this->_impl->owner->eventLoop() : nullptr;

	if ( senderLoop
		&& ! senderLoop->shouldDispatchDirectlyOnThread( platform::currentThreadId() ) )
	{
		auto capturedArgs =
			std::make_shared< std::tuple< std::decay_t< Args >... > >(
				std::forward< Args >( args )... );

		platform::WeakPtr< typename Base::EventImpl > w( this->_impl );
		senderLoop->post(
			EventLoop::Task::create(
				[ w, capturedArgs ]() {
					if ( auto impl = w.lock() )
					{
						std::apply(
							[ &impl ]( auto&&... a ) {
								impl->dispatch(
									std::forward< decltype( a ) >( a )... );
							},
							*capturedArgs );
					}
				} ),
			this->_impl->id );
	}
	else
	{
		this->_impl->dispatch( std::forward< Args >( args )... );
	}

	if ( statsOn )
	{
		auto endTime = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast< std::chrono::nanoseconds >( endTime - startTime );
		_emissionCount.fetch_add( 1, std::memory_order_relaxed );
		_totalEmitTimeNs.fetch_add(
			static_cast< uint64_t >( duration.count() ),
			std::memory_order_relaxed );
	}
}

template< typename MutexType, typename... Args >
inline void BasicRecordableEvent< MutexType, Args... >::trigger( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

template< typename MutexType, typename... Args >
inline void BasicRecordableEvent< MutexType, Args... >::emit( Args... args )
{
	operator()( std::forward< Args >( args )... );
}


// ===========================================================================
// recording API
// ===========================================================================

template< typename MutexType, typename... Args >
inline EventRecorder< Args... >&
BasicRecordableEvent< MutexType, Args... >::recorder()
{
	return _recorder;
}

template< typename MutexType, typename... Args >
inline const EventRecorder< Args... >&
BasicRecordableEvent< MutexType, Args... >::recorder() const
{
	return _recorder;
}


// ===========================================================================
// statistics API
// ===========================================================================

template< typename MutexType, typename... Args >
inline bool BasicRecordableEvent< MutexType, Args... >::statisticsEnabled() const
{
	return _statisticsEnabled.load( std::memory_order_relaxed );
}

template< typename MutexType, typename... Args >
inline void BasicRecordableEvent< MutexType, Args... >::enableStatistics( bool enable )
{
	_statisticsEnabled.store( enable, std::memory_order_relaxed );
	if ( enable )
	{
		// reset so stats reflect the period starting from this call
		_emissionCount.store( 0, std::memory_order_relaxed );
		_totalEmitTimeNs.store( 0, std::memory_order_relaxed );
	}
}

template< typename MutexType, typename... Args >
inline RecordingStatistics
BasicRecordableEvent< MutexType, Args... >::getStatistics() const
{
	RecordingStatistics stats;
	stats.emissionCount = _emissionCount.load( std::memory_order_relaxed );
	stats.totalEmitTimeNs = _totalEmitTimeNs.load( std::memory_order_relaxed );
	stats.avgEmitTimeNs = stats.emissionCount > 0
		? static_cast< double >( stats.totalEmitTimeNs ) / stats.emissionCount
		: 0.0;

	// read connection counts directly from _impl - same data EventInspector
	// would read, without needing to construct one
	platform::LockGuard< MutexType > lock( this->_impl->mutex );
	stats.connectionCount = this->_impl->handlers.size();
	stats.activeConnectionCount = 0;
	stats.directConnectionCount = 0;
	stats.deferredConnectionCount = 0;

	for ( const auto& entry : this->_impl->handlers )
	{
		if ( ! entry.flags.isActive() )
		{
			continue;
		}

		++stats.activeConnectionCount;
		auto resolved = entry.connTypes.resolvedConnType();
		if ( resolved == ResolvedConnectionType::Direct )
		{
			++stats.directConnectionCount;
		}
		else
		{
			++stats.deferredConnectionCount;
		}
	}

	return stats;
}

template< typename MutexType, typename... Args >
inline void BasicRecordableEvent< MutexType, Args... >::resetStatistics()
{
	_emissionCount.store( 0, std::memory_order_relaxed );
	_totalEmitTimeNs.store( 0, std::memory_order_relaxed );
}


// ===========================================================================
// getSummary
// ===========================================================================

template< typename MutexType, typename... Args >
inline std::string BasicRecordableEvent< MutexType, Args... >::getSummary() const
{
	auto stats = getStatistics();  // also snapshots connection counts

	std::ostringstream oss;
	oss << "RecordableEvent Summary:\n";
	oss << "  Type: " << demangle( PULSAR_TYPE_NAME( *this ) ) << "\n";
	oss << "\nConnections:\n";
	oss << "  Total slots:  " << stats.connectionCount << "\n";
	oss << "  Active:       " << stats.activeConnectionCount << "\n";
	oss << "  Direct:       " << stats.directConnectionCount << "\n";
	oss << "  Deferred:     " << stats.deferredConnectionCount << "\n";

	if ( _statisticsEnabled.load( std::memory_order_relaxed ) )
	{
		oss << "\nStatistics:\n";
		oss << "  Emissions:  " << stats.emissionCount << "\n";
		oss << "  Total Time: " << stats.totalEmitTimeNs << " ns\n";
		oss << "  Avg Time:   "
			<< std::fixed << std::setprecision( 2 )
			<< stats.avgEmitTimeNs << " ns\n";
	}

	auto recMode = _recorder.mode();
	if ( recMode != RecordingMode::Disabled )
	{
		oss << "\nRecording:\n";
		oss << "  Status: ";
		switch ( recMode )
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
			oss << "  Duration:     " << timingStats.totalDuration.count() << " ns\n";
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

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_BASIC_RECORDABLE_EVENT_H
