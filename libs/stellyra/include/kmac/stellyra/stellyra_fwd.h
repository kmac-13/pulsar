#ifndef KMAC_STELLYRA_FORWARD_H
#define KMAC_STELLYRA_FORWARD_H

/**
 * @file stellyra_fwd.h
 * @brief Forward declarations for all Stellyra library classes, enums, and aliases.
 *
 * Include this header when you only need type names (e.g. in other library headers)
 * and want to avoid pulling in full definitions.  Full definitions are provided by
 * the individual component headers and the top-level <kmac/stellyra.h> and
 * <kmac/stellyra_extras.h>.
 */

namespace kmac {
namespace stellyra {

class Connection;        ///< see connection.h
class ConnectionGroup;   ///< see connection_group.h (in extras)
class EventLoop;         ///< see event_loop.h
class ScopedConnection;  ///< see connection.h
class Trackable;         ///< see trackable.h

/**
 * @brief Lifecycle state of an EventRecorder.
 */
enum class RecordingMode
{
	Disabled,   ///< No recording in progress; recorder is idle.
	Recording,  ///< Actively capturing emissions.
	Paused      ///< Recording was started but is temporarily suspended.
};

template< typename MutexType, typename... Args >
class BasicEvent;  ///< see event.h

template< typename FriendType, typename MutexType, typename... Args >
class BasicPrivateEvent;  ///< see private_event.h

// template< typename MutexType, typename... Args >
// class EventImpl;

template< typename MutexType, typename... Args >
class EventInspector;  ///< see event_inspector.h (stellyra_extras)

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_FORWARD_H
