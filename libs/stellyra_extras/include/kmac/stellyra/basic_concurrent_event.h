#pragma once
#ifndef KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_H
#define KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_H

/**
 * @file basic_concurrent_event.h
 * @brief BasicConcurrentEvent<MutexType, Args...> - the triggerable, primary
 * event type, backed by BasicConcurrentEventImpl through the generalised
 * EventStorage.
 *
 * Mirrors BasicEvent exactly in shape: EventStorage supplies the full
 * connect/connectFree/connectLambda/disconnect/ConnParams/forwardTo
 * surface generically, this file adds only operator()/trigger()/emit().
 *
 * Direct, Deferred, and Auto-resolved connections are all supported, as is
 * sender-deferred triggering (see operator()'s docs below) - the connect-
 * side API is the full one BasicEvent offers today, and dispatch behavior
 * matches it.
 */

#include "basic_concurrent_event_impl.h"

#include <kmac/stellyra/event_loop.h>
#include <kmac/stellyra/event_storage.h>
#include <kmac/stellyra/platform.h>
#include <kmac/stellyra/trackable.h>

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace kmac {
namespace stellyra {

/**
 * @brief Triggerable concurrent event backed by BasicConcurrentEventImpl.
 *
 * Inherits connection management from EventStorage and adds
 * operator() / trigger() / emit() for dispatching to connected handlers.
 *
 * @tparam MutexType synchronisation strategy
 * @tparam Args argument types forwarded to connected handlers
 */
template< typename MutexType, typename... Args >
class BasicConcurrentEvent : public EventStorage< BasicConcurrentEventImpl, MutexType, Args... >
{
	using Base = EventStorage< BasicConcurrentEventImpl, MutexType, Args... >;

public:
	using Base::Base;  // inherit constructors

	~BasicConcurrentEvent() = default;

	BasicConcurrentEvent( const BasicConcurrentEvent& ) = delete;
	BasicConcurrentEvent& operator=( const BasicConcurrentEvent& ) = delete;
	BasicConcurrentEvent( BasicConcurrentEvent&& ) = default;
	BasicConcurrentEvent& operator=( BasicConcurrentEvent&& ) = default;

	/**
	 * @brief Trigger the event - dispatch to all connected, unblocked,
	 * predicate-passing handlers.
	 *
	 * Sender-deferred: if this event's owner has an EventLoop and the
	 * calling thread isn't that loop's designated context (its registered
	 * drain thread, or the thread currently draining it - see
	 * EventLoop::shouldDispatchDirectlyOnThread()), the trigger itself is
	 * posted to that loop instead of dispatching here and now - the
	 * *whole* dispatch() call happens later, on the owner's home thread,
	 * against whatever the handler list looks like at that later time.
	 * Exactly mirrors BasicEvent::operator() (event.h); dispatch() re-enters
	 * the epoch fresh whenever it actually runs, so nothing about a deferred
	 * trigger needs to survive the interval except the event's own weak
	 * reference and the captured arguments.
	 *
	 * Once the sender-deferral gate above lets a trigger through (or
	 * doesn't apply), dispatch is still always synchronous and lock-free
	 * on whichever thread actually runs it.
	 *
	 * Silently dropped while the event is blocked, checked before either
	 * path above.
	 */
	void operator()( Args... args );

	/**
	 * @brief Trigger the event.
	 *
	 * Identical to operator().
	 */
	void trigger( Args... args );

	/**
	 * @brief Trigger the event.
	 *
	 * Identical to operator().
	 */
	void emit( Args... args );
};

template< typename MutexType, typename... Args >
inline void BasicConcurrentEvent< MutexType, Args... >::operator()( Args... args )
{
	if ( this->_impl->isBlocked() )
	{
		return;
	}

	Trackable* owner = this->_impl->owner;
	EventLoop* senderLoop = owner ? owner->eventLoop() : nullptr;

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
								impl->dispatch( std::forward< decltype( a ) >( a )... );
							},
							*capturedArgs );
					}
				} ),
			this->_impl->id );
		return;
	}

	this->_impl->dispatch( std::forward< Args >( args )... );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEvent< MutexType, Args... >::trigger( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEvent< MutexType, Args... >::emit( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_H
