#pragma once
#ifndef KMAC_PULSAR_TRACKABLE_H
#define KMAC_PULSAR_TRACKABLE_H

/**
 * @file trackable.h
 * @brief Trackable base class and Anchor alias.
 *
 * A Trackable serves two roles:
 *
 *   Sender role (owner) - a BasicEvent constructed with this Trackable as
 *   its owner derives its sender EventLoop from it.  When setEventLoop()
 *   is called, both roles are notified: owned sender events are re-resolved
 *   via updateSenderLoop() and tracked receiver connections are re-resolved
 *   via updateHandlerLoop().
 *
 *   Receiver role - connections to handlers on this object are tracked so
 *   they are automatically disconnected when this object is destroyed.
 *   Every connection where this Trackable is the receiver shares this
 *   object's single migration tag (_migrationTag), so all of a receiver's
 *   pending Deferred tasks move together in one call when it switches
 *   EventLoops, regardless of which event produced each task.
 *
 * Lock ordering: Trackable::_mutex is always released before any call that
 * may acquire an EventImpl mutex.  Both setEventLoop() and trackConnection()
 * snapshot the relevant lists under _mutex, then call back into EventImpl
 * without holding _mutex.
 */

#include "platform.h"
#include "event_impl_base.h"
#include "event_loop.h"
#include "gen_data.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kmac {
namespace pulsar {

class Trackable
{
	// need to support access to the two private registration methods

	template< typename MutexType, typename... Args >
	friend class EventStorage;

	template< typename ReturnType, typename MutexType, typename... Args >
	friend class CombiningEvent;

private:
	/**
	 * @brief One connection this Trackable is the receiver-side tracker for,
	 * kept so it can be auto-disconnected on destruction (or bulk-removed by
	 * extractConnectionsTo()) without the owning event needing to know about
	 * this Trackable at all.
	 */
	struct TrackedConnection
	{
		platform::WeakPtr< EventImplBase > impl;  ///< the owning event's impl; expired() once that event is destroyed
		uint32_t index;          ///< stable handle index into the event's handler array
		uint16_t generation;     ///< generation stamped on the handle when this connection was made
		uint16_t _pad = 0;       // reserved byte padding
	};

	std::vector< TrackedConnection > _trackedConnections;
	std::vector< platform::WeakPtr< EventImplBase > > _ownedEvents;
	platform::Mutex _mutex;

	/**
	 * @brief Source of fresh migration tags, one per Trackable rather than
	 * one per connection.  Starts at 1 so 0 remains reserved as the "no
	 * particular receiver" sentinel used elsewhere (e.g. EventLoop::post()'s
	 * default tag, and untracked connections - see EventStorage::connectImpl).
	 */
	inline static std::atomic< uint64_t > _nextMigrationTag { 1 };

	/**
	 * @brief This receiver's migration tag, shared by every connection where
	 * this Trackable is the receiver, regardless of which event or handler
	 * produced it.  Assigned once at construction; never changes.  Used as
	 * the EventLoop task tag for Deferred connections so that setEventLoop()
	 * can migrate all of this receiver's pending tasks in a single call
	 * rather than needing to collect one tag per connection.
	 */
	const uint64_t _migrationTag = _nextMigrationTag.fetch_add( 1, std::memory_order_relaxed );

	/**
	 * @brief Atomic because eventLoop() is read on the connect path without
	 * holding _mutex.  Written under _mutex; read lock-free everywhere else.
	 * Relaxed ordering is sufficient: any thread reading an EventLoop* it got
	 * from here will have obtained a reference through some prior
	 * synchronization (construction, shared_ptr, etc.) that provides the
	 * necessary happens-before
	 */
	platform::Atomic< EventLoop* > _eventLoop { nullptr };

public:
	Trackable() = default;
	virtual ~Trackable();

	/**
	 * @brief Non-copyable and non-movable: BasicEvent stores this Trackable's
	 * address as EventImpl::owner (sender role) and events elsewhere hold it
	 * indirectly via TrackedConnection (receiver role) - either pointer would
	 * dangle or go stale if the Trackable's address could change after
	 * construction.
	 */
	Trackable( const Trackable& ) = delete;
	Trackable& operator=( const Trackable& ) = delete;
	Trackable( Trackable&& ) = delete;
	Trackable& operator=( Trackable&& ) = delete;

	// -------------------------------------------------------------------------

	/**
	 * @brief Returns the EventLoop currently associated via setEventLoop(),
	 * or nullptr if none.  Lock-free read (see _eventLoop above); this is
	 * what Auto-resolution reads on both the sender and receiver side.
	 */
	EventLoop* eventLoop() const noexcept;

	/**
	 * @brief Associate this Trackable with an EventLoop.
	 *
	 * Notifies all owned BasicEvents to re-resolve as the sender or
	 * re-resolves all Auto connections where this is the receiver.
	 *
	 * Thread-safe - may be called before or after connections are made.
	 */
	void setEventLoop( EventLoop* loop );

	/**
	 * @brief Disconnect all connections tracked by this Trackable.
	 *
	 * Iterates all tracked connections and disconnects each one.  The
	 * tracked connection list is cleared afterward.  Safe to call
	 * explicitly (e.g. at the top of a derived class destructor before
	 * members are freed) as well as from ~Trackable().
	 *
	 * Thread-safe.
	 */
	void disconnectAll();

private:
	/**
	 * @brief Internal: this receiver's migration tag - see _migrationTag.
	 * Used by EventStorage::connectImpl() / CombiningEvent::connectImpl()
	 * to tag a Deferred connection's posted tasks so they migrate as a
	 * group with every other connection on this same receiver.
	 */
	uint64_t migrationTag() const noexcept;

	/**
	 * @brief Internal: called by BasicEvent constructor to register ownership.
	 */
	void registerOwnedEvent( platform::WeakPtr< EventImplBase > impl );

	/**
	 * @brief Internal: register a connection for auto-disconnect on destruction.
	 */
	void trackConnection(
		platform::WeakPtr< EventImplBase > impl,
		uint32_t index,
		uint32_t generation );

	/**
	 * @brief Internal: remove and return all tracked connections to a specific
	 * event.  Used by BasicEvent::disconnect( Trackable& ) to bulk-disconnect
	 * all connections between one event and this tracker.
	 *
	 * @returns pairs of { index, generation } for each removed connection;
	 *   caller is responsible for disconnecting
	 */
	std::vector< GenData > extractConnectionsTo( EventImplBase* target );
};

// ---------------------------------------------------------------------------

inline Trackable::~Trackable()
{
	disconnectAll();
}

inline void Trackable::disconnectAll()
{
	// snapshot under lock, then operate on EventImpls without holding _mutex
	// so we do not invert the EventImpl::mutex -> Trackable::_mutex order
	std::vector< TrackedConnection > snapshot;
	{
		std::lock_guard< platform::Mutex > lock( _mutex );
		snapshot.swap( _trackedConnections );
	}

	// one disconnectHandler call per connection - each acquires the event
	// mutex independently; a grouped approach (one lock per unique event)
	// would reduce lock acquisitions when many connections share the same
	// event, but requires heap-allocating inner vectors and an O(N*E) scan
	// to group them - more expensive than the saved lock acquisitions for
	// the typical case of one or two connections per event per receiver
	for ( auto& entry : snapshot )
	{
		if ( auto impl = entry.impl.lock() )
		{
			impl->disconnectHandler( entry.index, entry.generation );
		}
	}
}

inline EventLoop* Trackable::eventLoop() const noexcept
{
	return _eventLoop.load( std::memory_order_relaxed );
}

inline uint64_t Trackable::migrationTag() const noexcept
{
	return _migrationTag;
}

inline void Trackable::setEventLoop( EventLoop* loop )
{
	// snapshot both lists under a brief lock, then notify without holding
	// _mutex so we do not invert the EventImpl::mutex -> Trackable::_mutex order
	EventLoop* oldLoop = nullptr;
	std::vector< TrackedConnection > receiverSnapshot;
	std::vector< platform::WeakPtr< EventImplBase > > senderSnapshot;
	{
		std::lock_guard< platform::Mutex > lock( _mutex );
		oldLoop = _eventLoop.load( std::memory_order_relaxed );
		_eventLoop.store( loop, std::memory_order_relaxed );
		receiverSnapshot = _trackedConnections;
		senderSnapshot = _ownedEvents;
	}

	// migrate tasks that are pending (not yet draining) on the old loop
	// to the new loop before re-resolving, so they execute in the correct
	// context; tasks already in the old loop's active drain batch are left
	// alone - they were in flight before the migration and complete there
	//
	// every connection where this Trackable is the receiver shares the same
	// _migrationTag, so a single call migrates all of this receiver's
	// pending tasks together, regardless of which event produced each one
	if ( oldLoop && loop && oldLoop != loop && ! receiverSnapshot.empty() )
	{
		oldLoop->migratePendingTo( loop, _migrationTag );
	}

	for ( auto& weak : senderSnapshot )
	{
		if ( auto impl = weak.lock() )
		{
			impl->updateSenderLoop( loop );
		}
	}

	for ( auto& entry : receiverSnapshot )
	{
		if ( auto impl = entry.impl.lock() )
		{
			impl->updateHandlerLoop( entry.index, entry.generation, loop );
		}
	}
}

inline void Trackable::registerOwnedEvent( platform::WeakPtr< EventImplBase > impl )
{
	std::lock_guard< platform::Mutex > lock( _mutex );

	// prune expired events
	_ownedEvents.erase(
		std::remove_if( _ownedEvents.begin(), _ownedEvents.end(),
			[]( const platform::WeakPtr< EventImplBase >& w ) { return w.expired(); } ),
		_ownedEvents.end() );

	// add the new event
	_ownedEvents.push_back( std::move( impl ) );
}

inline void Trackable::trackConnection(
	platform::WeakPtr< EventImplBase > impl,
	uint32_t index,
	uint32_t generation )
{
	std::lock_guard< platform::Mutex > lock( _mutex );

	// prune disconnected connections
	_trackedConnections.erase(
		std::remove_if( _trackedConnections.begin(), _trackedConnections.end(),
			[]( const TrackedConnection& tcon ) {
				auto impl = tcon.impl.lock();
				return ! impl || ! impl->isHandlerConnected( tcon.index, tcon.generation );
			} ),
		_trackedConnections.end() );

	// add the new connection
	_trackedConnections.push_back( { std::move( impl ), index, static_cast< uint16_t >( generation ), 0 } );
}

inline std::vector< GenData > Trackable::extractConnectionsTo( EventImplBase* target )
{
	std::vector< GenData > result;
	std::lock_guard< platform::Mutex > lock( _mutex );

	_trackedConnections.erase(
		std::remove_if( _trackedConnections.begin(), _trackedConnections.end(),
			[ &result, target ]( const TrackedConnection& tc ) {
				if ( tc.impl.lock().get() == target )
				{
					result.emplace_back( tc.index, tc.generation );
					return true;
				}
				return false;
			} ),
		_trackedConnections.end() );

	return result;
}

// ---------------------------------------------------------------------------

/**
 * @brief Type alias for the composition pattern - a standalone Trackable
 * member for classes that cannot (or should not) inherit from Trackable.
 *
 * @section ordering Declaration order
 *
 * The actual guarantee that a disconnected handler is never invoked comes
 * entirely from the sender side: disconnectAll() (run by ~Trackable())
 * marks the connection's slot inactive on the sender's EventImpl, under
 * that impl's own mutex, and dispatch checks that same slot state under the
 * same lock before invoking anything. None of this depends on the
 * receiver's member layout.
 *
 * Declaring the Anchor member last among the owning class's data members
 * (methods don't affect member destruction order, only data member
 * declaration order does) means it is destroyed first, since C++ destroys
 * members in reverse declaration order - so disconnectAll() runs before any
 * of the owner's other members are torn down.  This is not a hard
 * requirement (ordinary Trackable-derived receivers get the *opposite*
 * ordering unconditionally, since base class subobjects always destruct
 * after all derived members, and that is accepted as fine), but it is a
 * zero-cost measure that narrows the window during which a connection could
 * still be nominally live while another one of the owner's members is
 * already gone.  It does not, and cannot, protect against a dispatch that is
 * already mid-execution when the owner's destruction begins - avoiding that
 * is an ordinary lifetime/ownership responsibility of the caller, the same
 * as for any C++ object shared across threads.
 */
using Anchor = Trackable;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_TRACKABLE_H
