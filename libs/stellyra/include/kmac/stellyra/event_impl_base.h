#pragma once
#ifndef KMAC_STELLYRA_EVENT_IMPL_BASE_H
#define KMAC_STELLYRA_EVENT_IMPL_BASE_H

/**
 * @file event_impl_base.h
 * @brief Type-erased base for BasicEvent's internal implementation object.
 *
 * One EventImplBase is heap-allocated per BasicEvent and shared between the
 * event and all its Connection/Trackable handles via SharedPtr/WeakPtr.
 *
 * @section id Unique ID
 *
 * Each instance receives a globally unique uint64_t _id from a static atomic
 * counter on this base class.  Placing the counter here (not on the template
 * subclass EventImpl<MutexType,Args...>) ensures one monotonic sequence for
 * the entire process - a counter on the subclass would give a separate
 * sequence per template instantiation, allowing ID collisions across event
 * types.  The ID is used by EventLoop::migratePendingTo() to move pending
 * deferred tasks to a new loop without storing raw pointers in queue entries.
 */

#include "platform.h"
#include "gen_data.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace kmac {
namespace stellyra {

class EventLoop;

class EventImplBase
{
private:
	/**
	 * @brief Monotonically increasing process-wide counter for event identity tags.
	 */
	inline static platform::Atomic< uint64_t > _nextId { 0 };

public:
	/**
	 * @brief Globally unique ID assigned at construction.  Used by
	 * EventLoop task migration to identify tasks by owning event.
	 */
	const uint64_t id;

	EventImplBase();

	virtual ~EventImplBase() = default;

	EventImplBase( const EventImplBase& ) = delete;
	EventImplBase& operator=( const EventImplBase& ) = delete;

	/**
	 * @brief Returns true if the handler at index is active with the
	 * expected generation.
	 */
	virtual bool isHandlerConnected( uint32_t index, uint32_t generation ) const = 0;

	/**
	 * @brief Disconnect the handler at index if its generation matches.
	 * Successive disconnects should do nothing.
	 */
	virtual void disconnectHandler( uint32_t index, uint32_t generation ) = 0;

	/**
	 * @brief Disconnect a batch of handlers in a single lock acquisition.
	 *
	 * Equivalent to calling disconnectHandler() for each entry, but acquires
	 * the event mutex only once for the entire batch.  Used by
	 * Trackable::disconnectAll() to avoid N separate lock acquisitions when
	 * disconnecting all of a receiver's connections to one event.
	 *
	 * Entries whose index is out of range, whose active flag is clear, or
	 * whose generation does not match are silently skipped, matching the
	 * idempotent behaviour of the single-entry overload.
	 */
	virtual void disconnectHandlers( const std::vector< GenData >& entries ) = 0;

	/**
	 * @brief Returns true if the handler at index is active, matches the
	 * expected generation, and is currently blocked.
	 */
	virtual bool isHandlerBlocked( uint32_t index, uint32_t generation ) const = 0;

	/**
	 * @brief Block the handler at index if its generation matches; a
	 * blocked handler is skipped at dispatch time (same isActive() check
	 * point), without affecting any other connection.  No-op if the
	 * generation doesn't match (stale Connection).
	 */
	virtual void blockHandler( uint32_t index, uint32_t generation ) = 0;

	/**
	 * @brief Unblock the handler at index if its generation matches.
	 * No-op if the generation doesn't match (stale Connection).
	 */
	virtual void unblockHandler( uint32_t index, uint32_t generation ) = 0;

	/**
	 * @brief Re-resolve all Auto connections using the new sender EventLoop.
	 * Called by Trackable::setEventLoop() when the owning Trackable's loop
	 * changes.
	 */
	virtual void updateSenderLoop( EventLoop* loop ) = 0;

	/**
	 * @brief Update the receiver EventLoop for a handler entry and
	 * re-resolve its connection type if declared as Auto or if a Deferred
	 * connection loses its EventLoop.
	 *
	 * Called by Trackable::setEventLoop().
	 */
	virtual void updateHandlerLoop(
		uint32_t index,
		uint32_t generation,
		EventLoop* loop ) = 0;

	/**
	 * @brief Execute a deferred handler from an EventLoop drain.
	 *
	 * @param index handler index in EventImpl::handlers
	 * @param generation expected generation; mismatch = no-op (stale task)
	 * @param args type-erased shared_ptr<tuple<decay_t<Args>...>>; cast
	 *   back to the concrete type inside EventImpl
	 */
	virtual void invokeDeferred(
		uint32_t index,
		uint32_t generation,
		const std::shared_ptr< void >& args ) = 0;
};

inline EventImplBase::EventImplBase()
	: id( _nextId.fetch_add( 1, std::memory_order_relaxed ) )
{
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_EVENT_IMPL_BASE_H
