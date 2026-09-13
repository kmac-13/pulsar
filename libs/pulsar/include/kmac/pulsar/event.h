#pragma once
#ifndef KMAC_PULSAR_EVENT_H
#define KMAC_PULSAR_EVENT_H

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

#include "event_storage.h"

#include <memory>
#include <tuple>
#include <utility>

namespace kmac {
namespace pulsar {

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
class BasicEvent : public EventStorage< kmac::pulsar::EventImpl, MutexType, Args... >
{
	using Base = EventStorage< kmac::pulsar::EventImpl, MutexType, Args... >;

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


// ===========================================================================
// Aliases
// ===========================================================================

/**
 * @brief Thread-safe event using a recursive mutex - the default choice.
 * connect()/disconnect()/trigger() may be called from any thread, and a
 * handler may safely reentrantly connect, disconnect, or trigger on this
 * same event while it fires.  See SharedEvent for concurrent-reader
 * dispatch, or SingleThreadedEvent when the event is confined to one
 * thread and the locking overhead isn't needed at all.
 */
template< typename... Args >
using Event = BasicEvent< platform::RecursiveMutex, Args... >;

/**
 * @brief Thread-safe event using a shared (reader-writer) mutex: multiple
 * threads may trigger() concurrently and dispatch in parallel, at the cost
 * of connect()/disconnect() taking the exclusive lock (blocking until any
 * in-progress triggers finish) and single-shot connections being
 * unsupported (ConnParams::once() static_asserts) - a handler
 * disconnecting itself while a SharedLock is held would deadlock.
 */
template< typename... Args >
using SharedEvent = BasicEvent< platform::SharedMutex, Args... >;

/**
 * @brief Single-threaded event using a no-op mutex: no locking overhead at
 * all.  Only safe when the event and every one of its connections and
 * triggers are confined to a single thread - using it from more than one
 * thread is undefined behaviour, not a runtime-checked error.
 */
template< typename... Args >
using SingleThreadedEvent = BasicEvent< platform::NullMutex, Args... >;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_H
