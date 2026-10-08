#pragma once
#ifndef KMAC_STELLYRA_PRIVATE_EVENT_H
#define KMAC_STELLYRA_PRIVATE_EVENT_H

/**
 * @file private_event.h
 * @brief PrivateEvent - Event with compile-time restricted triggering.
 *
 * PrivateEvent<FriendType, Args...> inherits privately from
 * BasicEvent<MutexType, Args...> (defaulting to Event<Args...>) so that
 * only `FriendType` can call the triggering methods (operator(), trigger(),
 * emit()), while all connection and disconnect methods remain publicly
 * accessible.
 *
 * This enforces the principle that only the class that owns an event should
 * be able to fire it, while any code can observe it by connecting handlers.
 *
 * @code
 * class Button : public Trackable
 * {
 * public:
 *     // anyone can connect, but only Button can trigger
 *     PEvent< Button, int, int > clicked { this };
 *
 *     void click( int x, int y )
 *     {
 *         clicked( x, y );   // OK - Button is the friend type
 *     }
 * };
 *
 * Button button;
 * button.clicked.connect< &Handler::onClick >( handler );  // OK
 * button.clicked( 10, 20 );  // compile error - triggering is private
 * @endcode
 *
 * @warning Private inheritance means PrivateEvent cannot be used where a
 *   BasicEvent& is expected.
 *
 * @warning The access restriction applies to the type: any instance of
 *   FriendType may trigger any PrivateEvent<FriendType, ...>, not just the
 *   one declared on the same instance.  This is a standard C++ friend
 *   limitation.
 *
 * @see PEvent for the short aliases.
 */

#include "event.h"

namespace kmac {
namespace stellyra {

/**
 * @brief Event with triggering access restricted to `FriendType`.
 *
 * @tparam FriendType the class (and only that class) allowed to call
 *   operator()(), trigger(), and emit()
 * @tparam MutexType synchronisation strategy
 * @tparam Args argument types forwarded to connected handlers
 */
template< typename FriendType, typename MutexType, typename... Args >
class BasicPrivateEvent : private BasicEvent< MutexType, Args... >
{
	friend FriendType;

	template< typename M, typename... A >
	friend class EventInspector;

public:
	// inherit constructors (no-owner and owner forms)
	using BasicEvent< MutexType, Args... >::BasicEvent;

	~BasicPrivateEvent() = default;

	// =========================================================================
	// expose the full public connect / disconnect API since private
	// inheritance would normally hide everything; each using-declaration
	// explicitly opts a name back into the public interface
	// =========================================================================

	// --- object / method connections ---
	using BasicEvent< MutexType, Args... >::connect;

	// --- free-function connections (separate names; not covered above) ---
	using BasicEvent< MutexType, Args... >::connectFree;

	// --- lambda / functor connections (separate names; not covered above) ---
	using BasicEvent< MutexType, Args... >::connectLambda;

	// --- event forwarding ---
	using BasicEvent< MutexType, Args... >::forwardTo;

	// --- disconnect ---
	using BasicEvent< MutexType, Args... >::disconnect;
	using BasicEvent< MutexType, Args... >::disconnectFree;
	using BasicEvent< MutexType, Args... >::disconnectAll;

	// --- block / unblock ---
	using BasicEvent< MutexType, Args... >::block;
	using BasicEvent< MutexType, Args... >::unblock;
	using BasicEvent< MutexType, Args... >::blockGuard;

private:
	// maintain private visibility for trigger methods so they are
	// inaccessible outside FriendType;
	// private inheritance already hides them, but the other using
	// declarations make the intent explicit and guard against accidental
	// exposure via ADL or template argument deduction
	using BasicEvent< MutexType, Args... >::operator();
	using BasicEvent< MutexType, Args... >::trigger;
	using BasicEvent< MutexType, Args... >::emit;
};

// ============================================================================
// Convenience aliases
// ============================================================================

/**
 * @brief PrivateEvent using the default RecursiveMutex strategy.
 *
 * The most common form: use this unless you need SharedEvent or
 * SingleThreadedEvent semantics with trigger-access restriction.
 *
 * @code
 * PrivateEvent< MyClass, int > valueChanged { this };
 * @endcode
 */
template< typename FriendType, typename... Args >
using PrivateEvent = BasicPrivateEvent< FriendType, platform::RecursiveMutex, Args... >;

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
using PSignal = PrivateSignal< FriendType, Args... >;

/**
 * @brief PrivateEvent using SharedMutex (concurrent reads, exclusive writes).
 *
 * Use when the event will be triggered from many threads simultaneously and
 * all connected handlers are Direct.
 */
template< typename FriendType, typename... Args >
using SharedPrivateEvent = BasicPrivateEvent< FriendType, platform::SharedMutex, Args... >;

/**
 * @brief Shorter alias of SharedPrivateEvent.
 */
template< typename FriendType, typename... Args >
using SharedPEvent = SharedPrivateEvent< FriendType, Args... >;

/**
 * @brief PrivateEvent using NullMutex (zero overhead, single-threaded only).
 */
template< typename FriendType, typename... Args >
using SingleThreadedPrivateEvent = BasicPrivateEvent< FriendType, platform::NullMutex, Args... >;

/**
 * @brief Shorter alias of SingleThreadedPrivateEvent.
 */
template< typename FriendType, typename... Args >
using SingleThreadedPEvent = SingleThreadedPrivateEvent< FriendType, Args... >;

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_PRIVATE_EVENT_H
