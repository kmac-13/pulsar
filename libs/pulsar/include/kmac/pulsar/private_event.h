#ifndef KMAC_PULSAR_PRIVATE_EVENT_H
#define KMAC_PULSAR_PRIVATE_EVENT_H

/**
 * @file private_event.h
 * @brief PrivateEvent - Event with compile-time restricted triggering.
 *
 * PrivateEvent<FriendType, Args...> inherits privately from Event<Args...>
 * so that only @p FriendType can call the triggering methods
 * (operator(), trigger(), emit()), while all connection and inspection
 * methods remain publicly accessible.
 *
 * This enforces the principle that only the class that owns an event should
 * be able to fire it, while any code can observe it.
 *
 * @code
 * class Button : public pulsar::Object
 * {
 * public:
 *     // anyone can connect, but only Button can trigger
 *     pulsar::PrivateEvent<Button, int, int> clicked{this};
 *
 *     void click(int x, int y) {
 *         clicked(x, y);   // OK - Button is the friend type
 *     }
 * };
 *
 * auto button = std::make_shared<Button>();
 * button->clicked.connect(handler, &Handler::onClick);   // OK
 * button->clicked(10, 20);   // compile error - triggering is private
 * @endcode
 *
 * @warning Private inheritance means you cannot use PrivateEvent where an Event&
 *   is expected, use EventInspector to introspect the connection state.
 *
 * @warning The restricted access to FriendType doesn't prevent an instance of
 *   FriendType from triggering the event of a different instance of FriendType.
 *
 * @see PEvent for a shorter alias.
 */

#include "event.h"

namespace kmac {
namespace pulsar {

/**
 * @brief Event with triggering access restricted to @p FriendType.
 *
 * @tparam FriendType the class allowed to call operator(), trigger(), and emit(),
 *   typically the class that declares the event as a member
 * @tparam Args argument types forwarded to connected handlers
 */
template< typename FriendType, typename... Args >
class PrivateEvent : private Event< Args... >  // private inheritance: trigger methods hidden by default
{
	/**
	 * @brief Only FriendType is allowed to trigger event.
	 */
	friend FriendType;

	/**
	 * @brief EventInspector needs access to Event internals for inspection.
	 */
	friend class EventInspector< Args... >;

private:
	// re-hide trigger methods so they are inaccessible outside FriendType
	using Event< Args... >::operator();
	using Event< Args... >::trigger;
	using Event< Args... >::emit;

public:
	/**
	 * @brief Inherit the Event constructor (takes the sender Object*).
	 */
	using Event< Args... >::Event;

	virtual ~PrivateEvent() = default;

	// ========================================================================
	// Connect/Disconnect Methods - public, accessible to all
	// ========================================================================

	// explicitly expose all public connect and disconnect methods
	// required because private inheritance hides the base class public interface

	using Event< Args... >::connect;
	using Event< Args... >::connectFree;
	using Event< Args... >::connectOnce;
	using Event< Args... >::connectOnceFree;
	using Event< Args... >::connectIf;
	using Event< Args... >::connectWithPriority;

	using Event< Args... >::disconnect;
	using Event< Args... >::disconnectAll;
	using Event< Args... >::disconnectFree;

	using Event< Args... >::forwardTo;
};

// ============================================================================
// Aliases
// ============================================================================

/**
 * @brief Shorter alias for PrivateEvent.
 */
template< typename FriendType, typename... Args >
using PEvent = PrivateEvent< FriendType, Args... >;

/**
 * @brief Signal-terminology alias for PrivateEvent.
 */
template< typename FriendType, typename... Args >
using PrivateSignal = PrivateEvent< FriendType, Args... >;

/**
 * @brief Signal-terminology short alias for PrivateEvent.
 */
template< typename FriendType, typename... Args >
using PSignal = PrivateEvent< FriendType, Args... >;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_PRIVATE_EVENT_H
