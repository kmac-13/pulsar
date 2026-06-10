#ifndef KMAC_PULSAR_FORWARD_H
#define KMAC_PULSAR_FORWARD_H

/**
 * @file pulsar_fwd.h
 * @brief Forward declarations for all Pulsar library classes, enums, and aliases.
 *
 * Include this header when you only need type names (e.g. in other library headers)
 * and want to avoid pulling in full definitions.  Full definitions are provided by
 * the individual component headers and the top-level <kmac/pulsar.h> and
 * <kmac/pulsar_extras.h>.
 */

namespace kmac {
namespace pulsar {

class Connection;
class ConnectionBase;
class ConnectionGroup;
class EventLoop;
class Object;
class ScopedConnection;

/**
 * @brief Specifies how a handler is invoked relative to the triggering thread.
 */
enum class ConnectionType
{
	Auto,               ///< resolved at trigger-time: Direct if sender and receiver share a loop, Deferred otherwise
	Direct,             ///< handler executes immediately on the triggering thread
	Deferred,           ///< invocation is deferred to the receiver's EventLoop
	// Queued = Deferred,  ///< alias for Deferred (familiar to Qt/Boost users)
};

/**
 * @brief Lifecycle state of an EventRecorder.
 */
enum class RecordingMode
{
	Disabled,   ///< No recording in progress; recorder is idle.
	Recording,  ///< Actively capturing emissions.
	Paused      ///< Recording was started but is temporarily suspended.
};

template< typename... Args >
class Event;

template< typename FriendType, typename... Args >
class PrivateEvent;

/// @brief Shorter alias for PrivateEvent.
template< typename FriendType, typename... Args >
using PEvent = PrivateEvent< FriendType, Args... >;

template< typename ReturnType, typename Combiner, typename... Args >
class CombiningEvent;

template< typename... Args >
class RecordableEvent;

template< typename... Args >
class EventRecorder;

template< typename... Args >
class EventInspector;

// ============================================================================
// Signal / emit terminology aliases
// Provided for users migrating from Qt or Boost.Signals2.
// All aliases are fully interchangeable with their Event counterparts.
// ============================================================================

/// @brief Alias for Event - prefer Signal-based naming if your team uses Qt conventions
template< typename... Args >
using Signal = Event< Args... >;

/// @brief Alias for PrivateEvent
template< typename FriendType, typename... Args >
using PSignal = PrivateEvent< FriendType, Args... >;

/// @brief Alias for CombiningEvent
template< typename ReturnType, typename Combiner, typename... Args >
using CombiningSignal = CombiningEvent< ReturnType, Combiner, Args... >;

/// @brief Alias for RecordableEvent
template< typename... Args >
using RecordableSignal = RecordableEvent< Args... >;

/// @brief Alias for EventRecorder
template< typename... Args >
using SignalRecorder = EventRecorder< Args... >;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_FORWARD_H
