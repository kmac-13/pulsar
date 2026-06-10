#ifndef KMAC_PULSAR_OBJECT_H
#define KMAC_PULSAR_OBJECT_H

/**
 * @file object.h
 * @brief Lifetime anchor and EventLoop association for the Pulsar event system.
 *
 * Object serves two roles:
 *
* - **Sender:** the Event constructor takes a pointer to an Object that identifies
 *   the sender.  The class owning the Event can inherit from Object and pass *this*
 *   in or it can embed an Object member and pass its address:
 * @code
 * class Button : public pulsar::Object
 * {
 * public:
 *     pulsar::Event< int, int > clicked { this };
 * };
 * class Button  // no inheritance required for sender
 * {
 *     pulsar::Object _obj;
 * public:
 *     pulsar::Event< int, int > clicked { &_obj };
 * };
 * @endcode
 *
 * - **Receiver:** Pulsar must be able to verify that a receiver is still alive
 *   before each invocation.  There are two ways to provide this:
 *
 *   Option 1 - inherit from Object (simplest).  The receiver must be
 *   managed via std::shared_ptr:
 * @code
 * class Handler : public pulsar::Object
 * {
 * public:
 *     void onClicked( int x, int y ) { ... }
 * };
 *
 * auto handler = std::make_shared< Handler >();
 * event.connect( handler, &Handler::onClicked );
 * @endcode
 *
 *   Option 2 - embed a std::shared_ptr<Object> member (non-invasive).
 *   The outer class itself does not need to be shared_ptr-managed, but the
 *   embedded Object must be, and is passed to connect() as the lifetime anchor:
 * @code
class Handler
{
    std::shared_ptr< pulsar::Object > _lifetime =
	   std::make_shared< pulsar::Object >();
public:
    void onClicked( int x, int y ) { ... }
    std::shared_ptr< pulsar::Object > pulsarObject() { return _lifetime; }
};

Handler handler;
event.connect(
    handler.pulsarObject(),
    [&handler]( int x, int y ) { handler.onClicked( x, y ); } );
 * @endcode
 *
 * Object also provides:
 * - EventLoop association for Deferred connection dispatch
 * - automatic connection cleanup on destruction
 * - loop migration with pending-event transfer
 */

#include "pulsar_fwd.h"
#include "config.h"
#include "connection.h"
#include "event_loop.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace kmac {
namespace pulsar {

class Object : public std::enable_shared_from_this< Object >
{
private:
	friend class ConnectionBase;

	std::atomic< EventLoop* > _eventLoop;                         ///< loop this object dispatches to (nullptr = direct only)
	mutable platform::Mutex _connectionsMutex;
	std::vector< std::weak_ptr< ConnectionBase > > _connections;  ///< all connections involving this object

public:
	Object();

	virtual ~Object();

	// -------------------------------------------------------------------------
	// Event loop management
	// -------------------------------------------------------------------------

	/**
	 * @brief Returns the EventLoop currently associated with this object,
	 * or nullptr if none has been set.
	 */
	EventLoop* eventLoop() const;

	/**
	 * @brief Associate this object with @p loop and migrate any pending events.
	 *
	 * If the object already has a loop, pending events in the old loop that
	 * belong to this object are extracted and appended to the new loop, so
	 * no deferred invocations are lost.  All existing connections are updated
	 * to target the new loop.
	 *
	 * Call this before making connections if you want Auto resolution to
	 * select Deferred dispatch.
	 *
	 * @param loop new loop (may be nullptr to detach from any loop)
	 */
	void setEventLoop( EventLoop* loop );

	/**
	 * @brief Returns true if this object and @p other share the same
	 * non-null EventLoop instance.
	 *
	 * Used internally by Auto connection resolution to decide between Direct
	 * and Deferred dispatch: if both sender and receiver are on the same loop
	 * the connection is resolved as Direct.
	 */
	bool isOnSameLoop( const Object* other ) const;

	/**
	 * @brief Disconnect all existing connections, then set the event loop.
	 *
	 * Unlike setEventLoop(), this does NOT migrate pending events.  All
	 * connections are severed before the loop pointer is updated.  Use this
	 * when you want a clean break rather than a seamless migration.
	 *
	 * @param loop new loop (may be nullptr)
	 */
	void disconnectAndSetEventLoop( EventLoop* loop );

	// -------------------------------------------------------------------------
	// Connection registry (called internally by Event / ConnectionImpl)
	// -------------------------------------------------------------------------

	/**
	 * @brief Register a connection with this object so it can be cleaned up
	 * on destruction or loop migration.
	 *
	 * Called automatically by Event::connectInternal().  User code should
	 * not call this directly.
	 */
	void registerConnection( std::weak_ptr< ConnectionBase > conn );

	/**
	 * @brief Remove a previously registered connection.
	 *
	 * Called automatically when a connection is disconnected.  Also removes
	 * any expired weak_ptrs encountered during the scan.  User code should
	 * not call this directly.
	 */
	void unregisterConnection( std::weak_ptr< ConnectionBase > conn );

private:
	/**
	 * @brief Disconnect and clear all registered connections.  Called from the destructor.
	 */
	void disconnectAll();
};


//
// IMPLEMENTATION
//

Object::Object()
	: _eventLoop( nullptr )
{
}

Object::~Object()
{
	disconnectAll();
}

EventLoop* Object::eventLoop() const
{
	return _eventLoop.load( std::memory_order_acquire );
}

void Object::setEventLoop( EventLoop* loop )
{
	platform::LockGuard< platform::Mutex > lock( _connectionsMutex );

	if ( _eventLoop.load( std::memory_order_relaxed ) == loop )
	{
		return;  // no change needed
	}

	EventLoop* oldLoop = _eventLoop.load( std::memory_order_relaxed );

	if ( oldLoop )
	{
		// phase 1:
		// suspend all deferred connections so no new events are posted
		// to the old loop while we're transferring
		for( auto& weakConn : _connections )
		{
			if ( auto conn = weakConn.lock() )
			{
				conn->beginMigration();
			}
		}

		// phase 2:
		// extract this object's pending events from the old loop
		auto migratedEvents = oldLoop->extractEventsFor( this );

		// phase 3:
		// point all connections at the new loop
		for( auto& weakConn : _connections )
		{
			if ( auto conn = weakConn.lock() )
			{
				conn->updateEventLoop( loop );
			}
		}

		// phase 4:
		// re-queue the migrated events in the new loop (if any)
		if ( loop && ! migratedEvents.empty() )
		{
			loop->appendEvents( std::move( migratedEvents ) );
		}
	}

	_eventLoop.store( loop, std::memory_order_release );
}

bool Object::isOnSameLoop( const Object* other ) const
{
	EventLoop* thisLoop = _eventLoop.load( std::memory_order_acquire );
	EventLoop* otherLoop = other->_eventLoop.load( std::memory_order_acquire );
	return thisLoop != nullptr && thisLoop == otherLoop;
}

void Object::disconnectAndSetEventLoop( EventLoop* loop )
{
	platform::LockGuard< platform::Mutex > lock( _connectionsMutex );

	for ( auto& weakConn : _connections )
	{
		if ( auto conn = weakConn.lock() )
		{
			conn->disconnect();
		}
	}
	_connections.clear();

	_eventLoop.store( loop, std::memory_order_release );
}

void Object::registerConnection( std::weak_ptr< ConnectionBase > conn )
{
	platform::LockGuard< platform::Mutex > lock( _connectionsMutex );
	_connections.push_back( conn );
}

void Object::unregisterConnection( std::weak_ptr< ConnectionBase > conn )
{
	platform::LockGuard< platform::Mutex > lock( _connectionsMutex );
	_connections.erase(
		std::remove_if( _connections.begin(), _connections.end(),
			[ &conn ]( const std::weak_ptr< ConnectionBase >& weakConn ) {
				auto c1 = weakConn.lock();
				auto c2 = conn.lock();
				// remove if expired, or if it's the same connection impl
				return ! c1 || ( c2 && c1.get() == c2.get() );
			} ),
		_connections.end() );
}

void Object::disconnectAll()
{
	platform::LockGuard< platform::Mutex > lock( _connectionsMutex );
	for( auto& weakConn : _connections )
	{
		if ( auto conn = weakConn.lock() )
		{
			conn->disconnect();
		}
	}
	_connections.clear();
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_OBJECT_H
