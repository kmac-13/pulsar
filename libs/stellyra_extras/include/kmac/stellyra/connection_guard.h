#pragma once
#ifndef KMAC_STELLYRA_CONNECTION_GUARD_H
#define KMAC_STELLYRA_CONNECTION_GUARD_H

/**
 * @file connection_guard.h
 * @brief Thread-safe wrapper for a Connection handle.
 *
 * ConnectionGuard serialises isConnected(), disconnect(), and block()/
 * unblock()/isBlocked() through an internal mutex.  Use it when a single
 * Connection handle is stored in a container that may be read or written
 * from multiple threads simultaneously.
 *
 * **When to use:**
 *   - a Connection handle is shared across threads and must be
 *     atomically checked-and-disconnected or swapped
 *
 * **When NOT to use:**
 *   - single-threaded scenarios (use Connection directly - no mutex overhead)
 *   - when auto-disconnect via Trackable is sufficient (don't store handles at all)
 *   - when event.disconnect(tracker) works (simpler and faster)
 *
 * @warning Adds mutex overhead on every operation.  Only use when thread
 *   safety of the Connection handle itself is genuinely required.
 *
 * @code
 * class MultiThreadedManager
 * {
 * private:
 *     std::mutex _mutex;
 *     std::vector< ConnectionGuard > _connections;
 *
 * public:
 *     void addConnection( Connection conn )
 *     {
 *         std::lock_guard< std::mutex > lock( _mutex );
 *         _connections.emplace_back( std::move( conn ) );
 *     }
 *
 *     void disconnectAll()
 *     {
 *         std::lock_guard< std::mutex > lock( _mutex );
 *         for ( auto& guard : _connections )
 *         {
 *             guard.disconnect();
 *         }
 *     }
 * };
 * @endcode
 *
 * @see Connection for detailed thread safety notes
 */

#include <kmac/stellyra/connection.h>
#include <kmac/stellyra/platform.h>

namespace kmac {
namespace stellyra {

class ConnectionGuard
{
private:
	mutable platform::Mutex _mutex;
	Connection _conn;  ///<  guarded connection handle

public:
	/**
	 * @brief Wrap a Connection for thread-safe access.
	 *
	 * @param conn Connection to wrap (moved into the guard)
	 */
	explicit ConnectionGuard( Connection conn );

	/**
	 * @brief Move constructor, transfers the wrapped Connection.
	 *
	 * `other` is left wrapping a moved-from (empty) Connection - safe to
	 * call every method on it afterward, all reporting "not connected".
	 */
	ConnectionGuard( ConnectionGuard&& other ) noexcept;

	/**
	 * @brief Move assignment, replaces the wrapped Connection with `other`'s.
	 *
	 * This does NOT disconnect the connection currently wrapped by this
	 * guard - it simply stops being reachable through this guard and keeps
	 * running as an ordinary live connection on its event.  Contrast with
	 * ConnectionGroup::operator=, which explicitly disconnects its entire
	 * current contents before taking ownership of the source's connections:
	 * a ConnectionGuard wraps a single handle whose job is just to track
	 * *which* connection you're pointing at, so silently disconnecting the
	 * previous one on every reassignment would be surprising and
	 * destructive.  If you want the old connection gone, disconnect() it
	 * explicitly before reassigning.
	 */
	ConnectionGuard& operator=( ConnectionGuard&& other ) noexcept;

	ConnectionGuard( const ConnectionGuard& ) = delete;
	ConnectionGuard& operator=( const ConnectionGuard& ) = delete;

	~ConnectionGuard() = default;

	/**
	 * @brief Thread-safe liveness check.
	 *
	 * @return true if the connection is still active
	 */
	bool isConnected() const;

	/**
	 * @brief Thread-safe disconnect.  Safe to call from any thread.
	 */
	void disconnect();

	/**
	 * @brief Thread-safe per-connection blocked check.
	 *
	 * @return true if this specific connection is currently blocked
	 */
	bool isBlocked() const;

	/**
	 * @brief Thread-safe per-connection block - suppresses dispatch
	 * through this connection only, leaving every other connection on
	 * the same event unaffected.
	 */
	void block();

	/**
	 * @brief Thread-safe per-connection unblock.
	 */
	void unblock();
};


//
// IMPLEMENTATION
//

inline ConnectionGuard::ConnectionGuard( Connection conn )
	: _conn( std::move( conn ) )
{
}

inline ConnectionGuard::ConnectionGuard( ConnectionGuard&& other ) noexcept
{
	// lock other before moving so the move is atomic with respect to
	// concurrent isConnected() / disconnect() calls on other
	platform::LockGuard< platform::Mutex > lock( other._mutex );
	_conn = std::move( other._conn );
}

inline ConnectionGuard& ConnectionGuard::operator=( ConnectionGuard&& other ) noexcept
{
	if ( this != &other )
	{
		// acquire both locks without deadlock risk: always lock lower address first
		platform::Mutex* first = &_mutex < &other._mutex ? &_mutex : &other._mutex;
		platform::Mutex* second = &_mutex < &other._mutex ? &other._mutex : &_mutex;
		platform::LockGuard< platform::Mutex > lockA( *first );
		platform::LockGuard< platform::Mutex > lockB( *second );
		_conn = std::move( other._conn );
	}
	return *this;
}

inline bool ConnectionGuard::isConnected() const
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	return _conn.isConnected();
}

inline void ConnectionGuard::disconnect()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.disconnect();
}

inline bool ConnectionGuard::isBlocked() const
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	return _conn.isBlocked();
}

inline void ConnectionGuard::block()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.block();
}

inline void ConnectionGuard::unblock()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.unblock();
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_CONNECTION_GUARD_H
