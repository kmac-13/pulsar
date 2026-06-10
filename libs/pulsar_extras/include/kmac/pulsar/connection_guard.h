#ifndef KMAC_PULSAR_CONNECTION_GUARD_H
#define KMAC_PULSAR_CONNECTION_GUARD_H

/**
 * @file connection_guard.h
 * @brief Thread-safe ConnectionGuard.
 *
 * - **ConnectionGuard**:
 *   Mutex-protected wrapper for use when a Connection must be stored in a container
 *   accessed from multiple threads simultaneously.
 */

#include <kmac/pulsar/pulsar_fwd.h>
#include <kmac/pulsar/connection.h>

namespace kmac {
namespace pulsar {

/**
 * @brief Thread-safe wrapper for Connection storage in shared containers.
 *
 * Use ConnectionGuard when you need to store Connection handles in containers
 * that are accessed from multiple threads simultaneously.  It serializes all
 * Connection operations through an internal mutex.
 *
 * **When to use:**
 * - storing connections in containers accessed by multiple threads
 * - passing connections across thread boundaries
 * - complex multi-threaded connection management
 *
 * **When NOT to use:**
 * - single-threaded scenarios (use Connection directly - no mutex overhead)
 * - when auto-disconnect is sufficient (don't store handles at all)
 * - when Event::disconnect(receiver) works (simpler and faster)
 *
 * @warning This adds mutex overhead on every operation.  Only use when
 * thread safety of the handle itself is genuinely required.
 *
 * @code
 * class MultiThreadedManager
 * {
 * private:
 *     std::mutex _mutex;
 *     std::vector<ConnectionGuard> _connections;
 *
 * public:
 *     void addConnection(Connection conn) {
 *         std::lock_guard<std::mutex> lock(_mutex);
 *         _connections.emplace_back(std::move(conn));
 *     }
 *
 *     void disconnectAll() {
 *         std::lock_guard<std::mutex> lock(_mutex);
 *         for (auto& guard : _connections) {
 *             guard.disconnect();   // thread-safe
 *         }
 *     }
 * };
 * @endcode
 *
 * @see Connection for detailed thread safety notes
 */
class ConnectionGuard
{
private:
	mutable platform::Mutex _mutex;
	Connection _conn;

public:
	/**
	 * @brief Wrap a Connection for thread-safe access.
	 *
	 * @param conn Connection to wrap (moved into the guard)
	 */
	explicit ConnectionGuard( Connection conn );

	/**
	 * @brief Thread-safe check, returns true if the connection is active.
	 */
	bool isConnected() const;

	/**
	 * @brief Thread-safe disconnect.  Safe to call from any thread.
	 */
	void disconnect();

	/**
	 * @brief Thread-safe blocked check.
	 */
	bool isBlocked() const;

	/**
	 * @brief Thread-safe block.
	 */
	void block();

	/**
	 * @brief Thread-safe unblock.
	 */
	void unblock();
};

ConnectionGuard::ConnectionGuard( Connection conn )
	: _conn( std::move( conn ) )
{
}

bool ConnectionGuard::isConnected() const
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	return _conn.isConnected();
}

void ConnectionGuard::disconnect()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.disconnect();
}

bool ConnectionGuard::isBlocked() const
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	return _conn.isBlocked();
}

void ConnectionGuard::block()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.block();
}

void ConnectionGuard::unblock()
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_conn.unblock();
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONNECTION_GUARD_H
