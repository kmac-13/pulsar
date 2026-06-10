#ifndef KMAC_PULSAR_CONNECTION_H
#define KMAC_PULSAR_CONNECTION_H

/**
 * @file connection.h
 * @brief Connection handle and abstract base.
 *
 * This file defines the following related types:
 *
 * - **ConnectionBase**:
 *   Internal abstract interface implemented by each typed ConnectionImpl inside Event.
 *
 * - **Connection**:
 *   A lightweight, copyable handle returned by Event::connect().  Individual methods
 *   are thread-safe, but the handle object itself must not be shared across threads
 *   without external synchronization.
 */

#include "pulsar_fwd.h"
#include "config.h"

#include <atomic>
#include <memory>

namespace kmac {
namespace pulsar {

// ============================================================================
// ConnectionBase
// ============================================================================

/**
 * @brief Abstract base class for all typed connection implementations.
 *
 * Each ConnectionImpl<HandlerFunc> inside Event derives from this class.
 * User code never interacts with ConnectionBase directly; use Connection
 * (or ScopedConnection / ConnectionGroup) instead.
 */
class ConnectionBase
{
	// only Object (via setEventLoop/disconnectAndSetEventLoop) may call the
	// migration protocol methods, user code goes through Connection handles.
	friend class Object;

protected:
	std::atomic< bool > _connected { true };  ///< true while the connection is active
	std::atomic< bool > _blocked { false };   ///< true while the connection is temporarily blocked

public:
	virtual ~ConnectionBase() = default;

	/**
	 * @brief Returns true if the connection has not been disconnected.
	 */
	virtual bool isConnected() const = 0;

	/**
	 * @brief Permanently disconnect.  Safe to call multiple times.
	 */
	virtual void disconnect() = 0;

	/**
	 * @brief Returns true if the connection is currently blocked.
	 */
	virtual bool isBlocked() const = 0;

	/**
	 * @brief Suppress invocations without disconnecting.
	 */
	virtual void block() = 0;

	/**
	 * @brief Resume suppressed invocations.
	 */
	virtual void unblock() = 0;

private:
	/**
	 * @brief Called by Object::setEventLoop() before the object's loop pointer changes.
	 *
	 * Suspends deferred invocations until updateEventLoop() installs the new loop.
	 */
	virtual void beginMigration() = 0;

	/**
	 * @brief Called by Object::setEventLoop() after the new loop is set.
	 *
	 * @param newLoop the new EventLoop (may be nullptr to detach)
	 */
	virtual void updateEventLoop( EventLoop* newLoop ) = 0;
};

// ============================================================================
// Connection
// ============================================================================

/**
 * @brief Handle to an event connection.
 *
 * Connection provides explicit control over event connections, allowing manual
 * disconnection, blocking, and lifetime management.  However, explicit connection
 * management is often unnecessary - Pulsar automatically disconnects when objects
 * are destroyed.
 *
 * @section thread_safety Thread Safety
 *
 * **Methods are individually thread-safe:**
 * - disconnect(), block(), unblock(), isConnected() can be called from any thread
 *
 * **The Connection object itself is NOT thread-safe:**
 * - do NOT access the same Connection object from multiple threads simultaneously
 * - do NOT store Connection objects in shared containers without synchronization
 * - do NOT pass Connection objects by reference across thread boundaries
 *
 * @section lifetime Lifetime Management
 *
 * Connections automatically disconnect when:
 * - the sender (event owner) is destroyed
 * - the receiver (handler owner) is destroyed
 * - disconnect() is explicitly called
 *
 * You only need Connection handles when you want to:
 * - manually disconnect before object destruction
 * - temporarily block/unblock a connection
 * - check connection status
 *
 * @section usage Recommended Usage Patterns
 *
 * **Best: Let auto-disconnect work**
 * @code
 * event.connect(receiver, &Receiver::handler);
 * // no need to store Connection - auto-disconnects when receiver destroyed
 * @endcode
 *
 * **Good: Use ScopedConnection for RAII**
 * @code
 * auto scoped = event.connect(receiver, handler).scoped();
 * // auto-disconnects when scoped instance goes out of scope
 * @endcode
 *
 * **Good: Disconnect by receiver**
 * @code
 * event.disconnect(receiver);  // thread-safe, no handle needed
 * @endcode
 *
 * **Acceptable: Store in single-threaded context**
 * @code
 * Connection conn = event.connect(receiver, handler);
 * // ... later in same thread ...
 * conn.disconnect();
 * @endcode
 *
 * @section anti_patterns Anti-Patterns to Avoid
 *
 * **Wrong: Concurrent access to same Connection object**
 * @code
 * // thread A
 * connections[i].disconnect();
 *
 * // thread B (concurrent - RACE CONDITION on the Connection object!)
 * connections[i] = event.connect(...);
 * @endcode
 *
 * **Wrong: Passing by reference across threads**
 * @code
 * void worker(Connection& conn) {   // BAD - caller may also use conn
 *     conn.disconnect();
 * }
 * @endcode
 *
 * **Inefficient: Rapid connect/disconnect churn**
 * @code
 * while (running) {
 *     auto conn = event.connect(...);   // allocates each iteration
 *     ...
 *     conn.disconnect();
 * }
 * // better: use event.disconnect(receiver) to avoid handle churn
 * @endcode
 *
 * @section multi_threaded Multi-Threaded Scenarios
 *
 * If you must store connections in containers shared between threads,
 * use ConnectionGuard instead:
 * @code
 * ConnectionGuard guard(event.connect(receiver, handler));
 * // thread-safe from any thread:
 * guard.disconnect();
 * @endcode
 *
 * @see ScopedConnection for automatic RAII disconnection
 * @see ConnectionGuard for thread-safe storage in shared containers
 * @see Event::disconnect(receiver) for thread-safe disconnection by receiver
 */
class Connection
{
private:
	std::shared_ptr< ConnectionBase > _impl;

public:
	/**
	 * @brief Default constructor, creates an empty, disconnected handle.
	 */
	Connection() = default;

	/**
	 * @brief Construct from a ConnectionBase implementation.
	 *
	 * @param impl Shared ownership of the underlying connection.
	 */
	Connection( std::shared_ptr< ConnectionBase > impl );

	/**
	 * @brief Returns true if the connection is still active.
	 *
	 * Each call is atomic with respect to the internal connection state, but
	 * the Connection object must not be accessed concurrently from multiple
	 * threads.  Use ConnectionGuard for shared-ownership scenarios.
	 */
	bool isConnected() const;

	/**
	 * @brief Disconnect this connection.
	 *
	 * Safe to call from any thread, but the Connection object itself must not
	 * be accessed concurrently from multiple threads.  Use ConnectionGuard
	 * for shared-ownership scenarios.
	 *
	 * Has no effect if already disconnected.
	 */
	void disconnect();

	/**
	 * @brief Returns true if the connection is currently blocked.
	 *
	 * Each call is atomic with respect to the internal connection state, but
	 * the Connection object must not be accessed concurrently from multiple
	 * threads.  Use ConnectionGuard for shared-ownership scenarios.
	 */
	bool isBlocked() const;

	/**
	 * @brief Temporarily suppress handler invocations without disconnecting.
	 *
	 * Blocked connections remain in the event's connection list but their
	 * handlers are not called.  Each call is atomic with respect to the
	 * internal connection state, but the Connection object must not be
	 * accessed concurrently from multiple threads.  Use ConnectionGuard
	 * for shared-ownership scenarios.
	 *
	 * @see unblock()
	 */
	void block();

	/**
	 * @brief Resume handler invocations after block().
	 *
	 * Each call is atomic with respect to the internal connection state, but
	 * the Connection object must not be accessed concurrently from multiple
	 * threads.  Use ConnectionGuard for shared-ownership scenarios.
	 *
	 * @see block()
	 */
	void unblock();

	/**
	 * @brief Convert to a ScopedConnection for RAII lifetime management.
	 *
	 * This is an rvalue-ref qualified method: the Connection is moved into
	 * the returned ScopedConnection and is no longer valid after the call.
	 *
	 * @code
	 * auto scoped = event.connect(receiver, handler).scoped();
	 * // Automatically disconnects when scoped goes out of scope.
	 * @endcode
	 *
	 * @return ScopedConnection that owns and disconnects on destruction.
	 *
	 * @note the definition is found with ScopedConnection
	 */
	ScopedConnection scoped() &&;
};

//
// CONNECTION
//

Connection::Connection( std::shared_ptr< ConnectionBase > impl )
	: _impl( impl )
{
}

bool Connection::isConnected() const
{
	return _impl && _impl->isConnected();
}

void Connection::disconnect()
{
	if ( _impl )
	{
		_impl->disconnect();
	}
}

bool Connection::isBlocked() const
{
	return _impl && _impl->isBlocked();
}

void Connection::block()
{
	if ( _impl )
	{
		_impl->block();
	}
}

void Connection::unblock()
{
	if ( _impl )
	{
		_impl->unblock();
	}
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONNECTION_H
