#pragma once
#ifndef KMAC_STELLYRA_BASIC_EVENT_H
#define KMAC_STELLYRA_BASIC_EVENT_H

/**
 * @file event.h
 * @brief BasicEvent<MutexType, Args...> - the triggerable event type.
 *
 * BasicEvent inherits all connection management from EventStorage and adds
 * operator() / trigger() / emit() for dispatching to connected handlers.
 *
 * Public API is focused here.  Implementation details live in:
 *   event_storage.h : EventStorage<MutexType, Args...> - connect/disconnect/block
 *   event_flags.h   : EventFlags                       - bit-field flag wrapper
 *   event_impl.h    : EventImpl<MutexType, Args...>    - dispatch, mutex, handlers
 *   handler_entry.h : HandlerEntry<Args...>            - per-connection slot
 */

#include "basic_event_impl.h"
#include "event_storage.h"

#include <memory>
#include <tuple>
#include <utility>

namespace kmac {
namespace stellyra {

// ===========================================================================
// BasicEvent
// ===========================================================================

/**
 * @brief Triggerable event.  Inherits connection management from EventStorage
 * and adds operator() / trigger() / emit().
 *
 * @tparam MutexType synchronisation strategy
 * @tparam Args argument types forwarded to connected handlers
 */
template< typename MutexType, typename... Args >
class BasicEvent : public EventStorage< kmac::stellyra::BasicEventImpl, MutexType, Args... >
{
	using Base = EventStorage< kmac::stellyra::BasicEventImpl, MutexType, Args... >;

public:
	using Base::Base;  // inherit constructors

	~BasicEvent() = default;

	BasicEvent( const BasicEvent& ) = delete;
	BasicEvent& operator=( const BasicEvent& ) = delete;
	BasicEvent( BasicEvent&& ) = default;
	BasicEvent& operator=( BasicEvent&& ) = default;

	/**
	 * @brief Trigger the event - dispatch to all connected handlers.
	 *
	 * If the sender has an EventLoop, dispatch happens synchronously on the
	 * calling thread only when that thread should dispatch directly for
	 * this loop (see EventLoop::shouldDispatchDirectlyOnThread) - either
	 * because it has been registered as the loop's designated thread, or
	 * because it is, right now, the thread executing this loop's drain().
	 * Otherwise, the whole trigger (including Direct connections) is
	 * posted to the sender's EventLoop and runs whenever that loop is next
	 * drained.
	 *
	 * If the sender has no EventLoop, dispatch always happens synchronously.
	 *
	 * Silently dropped while the event is blocked.
	 */
	void operator()( Args... args );

	/// Identical to operator().
	void trigger( Args... args );

	/// Identical to operator().
	void emit( Args... args );
};


// ===========================================================================
// operator() / trigger / emit
// ===========================================================================

template< typename MutexType, typename... Args >
inline void BasicEvent< MutexType, Args... >::operator()( Args... args )
{
	if ( this->_impl->isBlocked() )
	{
		return;
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
		return;
	}

	this->_impl->dispatch( std::forward< Args >( args )... );
}

template< typename MutexType, typename... Args >
inline void BasicEvent< MutexType, Args... >::trigger( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

template< typename MutexType, typename... Args >
inline void BasicEvent< MutexType, Args... >::emit( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_BASIC_EVENT_H
