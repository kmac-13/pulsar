#ifndef KMAC_PULSAR_RECEIVER_LIFETIME_ANCHOR_H
#define KMAC_PULSAR_RECEIVER_LIFETIME_ANCHOR_H

/**
 * @file receiver_lifetime_anchor.h
 * @brief ReceiverLifetimeAnchor<Owner> - non-invasive receiver participation.
 *
 * Allows any class to act as a Pulsar receiver without inheriting from
 * pulsar::Object and without being shared_ptr-managed itself.  Embed one
 * as a member and pass @c this to the constructor:
 *
 * @code
 * class Receiver
 * {
 * public:
 *     void onPing( int v ) { ... }
 *
 *     pulsar::ReceiverLifetimeAnchor< Receiver > pulsarAnchor { this };
 * };
 *
 * Receiver receiver;
 * event.connect( receiver.pulsarAnchor, &Receiver::onPing );
 * @endcode
 *
 * @section mechanism Mechanism
 *
 * ReceiverLifetimeAnchor internally owns a @c shared_ptr<Object>.  That
 * Object is passed to Pulsar's connection infrastructure as the lifetime
 * anchor - the same role played by the shared_ptr when the receiver inherits
 * from Object directly.  The raw @c Owner* is captured inside the handler
 * lambda and is dereferenced only while the anchor Object is alive.
 *
 * @section safety Thread Safety and Destruction
 *
 * The internal @c shared_ptr<Object> is fully protected by the weak_ptr
 * locking mechanism: if a concurrent emission locks the weak_ptr before the
 * anchor destructs, the lock succeeds and the Object's destructor has not
 * yet started - the Object itself is safe to access.
 *
 * However, the @c Owner* captured in the handler lambda is a raw pointer,
 * not a shared_ptr.  It is valid only while the owner is alive.  This
 * creates a hazard when the owner can be destroyed on a different thread
 * from where emissions are dispatched: a concurrent emission may lock the
 * anchor's Object successfully (because the anchor has not yet been
 * destroyed) while the owner's destructor is already running on another
 * thread.  The handler would then dereference a raw pointer into a
 * partially-destroyed object.
 *
 * This is the same hazard as the manual @c shared_ptr<Object> pattern that
 * ReceiverLifetimeAnchor replaces - it is not made worse, but it is not
 * made better either.
 *
 * Safe usage:
 * - single-threaded programs (no concurrent emission possible)
 * - all connections explicitly disconnected before the owner is destroyed
 * - owner is always destroyed on its associated event loop thread with
 *   Deferred connections only (the loop serialises destruction and dispatch)
 *
 * If cross-thread destruction safety is required, inherit from
 * @c pulsar::Object and manage the receiver via @c shared_ptr instead.
 * The weak_ptr lock on a @c shared_ptr-managed Object guarantees the
 * destructor has not started if the lock succeeds, covering the cross-thread
 * case fully.
 *
 * @section event_loop EventLoop association
 *
 * The internal Object can be associated with an EventLoop the same way any
 * other Object can, enabling Deferred connections:
 *
 * @code
 * receiver.pulsarAnchor.setEventLoop( loop );
 * @endcode
 */

#include "object.h"

#include <memory>

namespace kmac {
namespace pulsar {

/**
 * @brief Non-invasive receiver anchor.  Embed as a member to enable an
 * otherwise unrelated class to receive Pulsar events.
 *
 * @tparam Owner the enclosing class whose methods will be used as handlers
 *
 * @see The Thread Safety and Destruction section in the file documentation
 *   for constraints on cross-thread destruction.
 */
template< typename Owner >
class ReceiverLifetimeAnchor
{
private:
	Owner* _owner;
	std::shared_ptr< Object > _object;

public:
	/**
	 * @brief Construct with a pointer to the enclosing owner.
	 *
	 * @param owner pointer to the object whose handler methods will be
	 *   connected via this anchor; typically @c this of the enclosing class
	 */
	explicit ReceiverLifetimeAnchor( Owner* owner );

	~ReceiverLifetimeAnchor() = default;

	// non-copyable, non-movable - the owner pointer and object identity
	// must remain stable for the lifetime of all connections
	ReceiverLifetimeAnchor( const ReceiverLifetimeAnchor& ) = delete;
	ReceiverLifetimeAnchor& operator=( const ReceiverLifetimeAnchor& ) = delete;
	ReceiverLifetimeAnchor( ReceiverLifetimeAnchor&& ) = delete;
	ReceiverLifetimeAnchor& operator=( ReceiverLifetimeAnchor&& ) = delete;

	// -------------------------------------------------------------------------
	// Accessors for Event::connect
	// -------------------------------------------------------------------------

	/**
	 * @brief Returns the raw owner pointer.
	 *
	 * Used by Event::connect to construct the handler lambda.
	 */
	Owner* owner() const;

	/**
	 * @brief Returns the internal lifetime anchor object.
	 *
	 * Passed to Pulsar's connection infrastructure as the receiver lifetime
	 * anchor.  Connections are severed when this shared_ptr is reset on
	 * destruction of the anchor.
	 */
	const std::shared_ptr< Object >& object() const;

	// -------------------------------------------------------------------------
	// EventLoop forwarding
	// -------------------------------------------------------------------------

	/**
	 * @brief Associate this receiver with an EventLoop.
	 *
	 * Forwards directly to the internal Object.  Required for Deferred
	 * connections targeting this receiver.
	 *
	 * @param loop the loop to associate with; pass nullptr to detach
	 */
	void setEventLoop( EventLoop* loop );

	/**
	 * @brief Returns the EventLoop currently associated with this receiver,
	 * or nullptr if none has been set.
	 */
	EventLoop* eventLoop() const;
};

/// @brief Shorter alias for ReceiverLifetimeAnchor.
template< typename Owner >
using RLAnchor = ReceiverLifetimeAnchor< Owner >;

template< typename Owner >
ReceiverLifetimeAnchor< Owner >::ReceiverLifetimeAnchor( Owner* owner )
	: _owner( owner )
	, _object( std::make_shared< Object >() )
{
}

template< typename Owner >
Owner* ReceiverLifetimeAnchor< Owner >::owner() const
{
	return _owner;
}

template< typename Owner >
const std::shared_ptr< Object >& ReceiverLifetimeAnchor< Owner >::object() const
{
	return _object;
}

template< typename Owner >
void ReceiverLifetimeAnchor< Owner >::setEventLoop( EventLoop* loop )
{
	_object->setEventLoop( loop );
}

template< typename Owner >
EventLoop* ReceiverLifetimeAnchor< Owner >::eventLoop() const
{
	return _object->eventLoop();
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_RECEIVER_LIFETIME_ANCHOR_H
