#ifndef KMAC_PULSAR_SCOPED_CONNECTION_H
#define KMAC_PULSAR_SCOPED_CONNECTION_H

/**
 * @file scoped_connection.h
 * @brief RAII wrapper that disconnects a connection on scope exit.
 *
 * ScopedConnection is the recommended way to manage the lifetime of a
 * connection that should not outlive a particular scope or object.
 *
 * @code
 * {
 *     auto scoped = event.connect(receiver, handler).scoped();
 *     // ... connection is active here ...
 * }
 * // connection automatically disconnected here
 * @endcode
 *
 * ScopedConnection is move-only (non-copyable).  Use release() to transfer
 * ownership back to a plain Connection if needed.
 *
 * @see Connection::scoped() factory method on Connection
 * @see makeScoped() free-function factory
 * @see ConnectionGroup for managing multiple connections as a unit
 */

#include <kmac/pulsar/connection.h>

namespace kmac {
namespace pulsar {

class ScopedConnection
{
private:
	Connection _connection;

public:
	/**
	 * @brief Default constructor, creates an empty, non-connected instance.
	 */
	ScopedConnection() = default;

	/**
	 * @brief Wrap @p conn in a scoped guard.
	 *
	 * @param conn Connection to take ownership of (moved)
	 */
	ScopedConnection( Connection conn );

	/**
	 * @brief Move constructor, transfers ownership without disconnecting.
	 */
	ScopedConnection( ScopedConnection&& other ) noexcept;

	/**
	 * @brief Move assignment, disconnects the currently held connection,
	 * then takes ownership of @p other's connection.
	 */
	ScopedConnection& operator=( ScopedConnection&& other ) noexcept;

	ScopedConnection( const ScopedConnection& ) = delete;
	ScopedConnection& operator=( const ScopedConnection& ) = delete;

	/**
	 * @brief Destructor, calls disconnect() on the held connection.
	 */
	~ScopedConnection();

	/**
	 * @brief Returns a mutable reference to the underlying Connection.
	 */
	Connection& connection();

	/**
	 * @brief Returns a const reference to the underlying Connection.
	 */
	const Connection& connection() const;

	/**
	 * @brief Returns true if the underlying connection is still active.
	 */
	bool isConnected() const;

	/**
	 * @brief Explicitly disconnect before scope exit.
	 */
	void disconnect();

	/**
	 * @brief Returns true if the connection is currently blocked.
	 */
	bool isBlocked() const;

	/**
	 * @brief Temporarily suppress invocations without disconnecting.
	 */
	void block();

	/**
	 * @brief Resume invocations after block().
	 */
	void unblock();

	/**
	 * @brief Release ownership without disconnecting.
	 *
	 * The returned Connection is now responsible for the connection's
	 * lifetime.  After this call the ScopedConnection is empty.
	 *
	 * @return the previously held Connection
	 */
	Connection release();
};

// ============================================================================
// Factory helpers
// ============================================================================

/**
 * @brief Create a ScopedConnection from a Connection (rvalue overload on Connection).
 *
 * @note This is defined here (after ScopedConnection is complete) because Connection
 * is declared in connection.h before ScopedConnection exists.
 */
inline ScopedConnection Connection::scoped() &&
{
	return ScopedConnection( std::move( *this ) );
}

/**
 * @brief Free-function factory for creating a ScopedConnection.
 *
 * Convenience alternative to Connection::scoped() when the Connection is
 * already in a variable:
 * @code
 * Connection conn = event.connect(receiver, handler);
 * auto scoped = makeScoped(std::move(conn));
 * @endcode
 */
inline ScopedConnection makeScoped( Connection conn )
{
	return ScopedConnection( std::move( conn ) );
}


//
// IMPLEMENTATION
//

ScopedConnection::ScopedConnection( Connection conn )
	: _connection( std::move( conn ) )
{
}

ScopedConnection::ScopedConnection( ScopedConnection&& other ) noexcept
	: _connection( std::move( other._connection ) )
{
}

ScopedConnection& ScopedConnection::operator=( ScopedConnection&& other ) noexcept
{
	if ( this != &other )
	{
		disconnect();
		_connection = std::move( other._connection );
	}
	return *this;
}

ScopedConnection::~ScopedConnection()
{
	disconnect();
}

Connection& ScopedConnection::connection()
{
	return _connection;
}

const Connection& ScopedConnection::connection() const
{
	return _connection;
}

bool ScopedConnection::isConnected() const
{
	return _connection.isConnected();
}

void ScopedConnection::disconnect()
{
	if ( _connection.isConnected() )
	{
		_connection.disconnect();
	}
}

bool ScopedConnection::isBlocked() const
{
	return _connection.isBlocked();
}

void ScopedConnection::block()
{
	_connection.block();
}

void ScopedConnection::unblock()
{
	_connection.unblock();
}

Connection ScopedConnection::release()
{
	return std::move( _connection );
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_SCOPED_CONNECTION_H
