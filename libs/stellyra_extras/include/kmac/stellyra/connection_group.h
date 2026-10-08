#pragma once
#ifndef KMAC_STELLYRA_CONNECTION_GROUP_H
#define KMAC_STELLYRA_CONNECTION_GROUP_H

/**
 * @file connection_group.h
 * @brief Manages a collection of connections that can be disconnected as a unit.
 *
 * Useful when an object holds connections that need to be torn down and
 * rebuilt during its lifetime - for example, when swapping a model, changing
 * state, or transitioning between phases.  Connections in the group can be
 * disconnected all at once without destroying the owning object:
 *
 * @code
 * class Widget : public Trackable
 * {
 * private:
 *     ConnectionGroup _connections;
 *
 * public:
 *     void setModel( Model* model )
 *     {
 *         _connections.disconnectAll();  // disconnect from old model
 *
 *         _connections += model->dataChanged.connect< &Widget::onDataChanged >( this );
 *         _connections += model->titleChanged.connect< &Widget::onTitleChanged >( this );
 *     }
 * };
 * @endcode
 *
 * ConnectionGroup is move-only (non-copyable).
 *
 * @note blockAll() / unblockAll() are declared but not yet implemented.
 *   Per-connection blocking will be re-integrated in a future update once
 *   the HandlerEntry flags are extended to carry a per-slot blocked bit.
 *   Event-level blocking is available now via event.block() / event.blockGuard().
 *
 * @see ScopedConnection for managing a single connection with RAII
 */

#include <kmac/stellyra/connection.h>

#include <algorithm>
#include <vector>

namespace kmac {
namespace stellyra {

class ConnectionGroup
{
private:
	std::vector< Connection > _connections;  ///<  all connections, may include inactive ones

public:
	/**
	 * @brief Default constructor, creates an empty group.
	 */
	ConnectionGroup() = default;

	/**
	 * @brief Move constructor, transfers all connections without disconnecting.
	 */
	ConnectionGroup( ConnectionGroup&& other ) noexcept;

	/**
	 * @brief Move assignment, disconnects current connections, then takes
	 * ownership of @p other's connections.
	 */
	ConnectionGroup& operator=( ConnectionGroup&& other ) noexcept;

	ConnectionGroup( const ConnectionGroup& ) = delete;
	ConnectionGroup& operator=( const ConnectionGroup& ) = delete;

	/**
	 * @brief Destructor, disconnects all held connections.
	 */
	~ConnectionGroup();

	// -------------------------------------------------------------------------
	// inspection
	// -------------------------------------------------------------------------

	/**
	 * @brief Returns the total number of connections (including disconnected
	 * ones not yet cleaned up).
	 */
	std::size_t size() const;

	/**
	 * @brief Returns the number of connections that are still active.
	 */
	std::size_t activeCount() const;

	/**
	 * @brief Returns true if the group contains no connections.
	 */
	bool empty() const;

	/**
	 * @brief Returns true if at least one connection in the group is still active.
	 */
	bool hasActiveConnections() const;

	// -------------------------------------------------------------------------
	// adding connections
	// -------------------------------------------------------------------------

	/**
	 * @brief Add a connection to the group.
	 *
	 * @param conn Connection to take ownership of (moved)
	 */
	void add( Connection conn );

	/**
	 * @brief Add a connection using the += operator.
	 *
	 * Enables the idiomatic pattern:
	 * @code
	 * _connections += event.connect< &Handler::onData >( this );
	 * @endcode
	 */
	ConnectionGroup& operator+=( Connection conn );

	// -------------------------------------------------------------------------
	// bulk operations
	// -------------------------------------------------------------------------

	/**
	 * @brief Disconnect and remove all connections in the group.
	 */
	void disconnectAll();

	/**
	 * @brief Block every connection in the group.
	 */
	void blockAll();

	/**
	 * @brief Unblock every connection in the group.
	 */
	void unblockAll();

	/**
	 * @brief Remove dead (disconnected) connections from the internal list.
	 *
	 * Connections are lazily kept in the vector after disconnect.  Call
	 * cleanup() to reclaim memory if you frequently disconnect individual
	 * connections from a long-lived group.
	 */
	void cleanup();

	// -------------------------------------------------------------------------
	// ownership transfer
	// -------------------------------------------------------------------------

	/**
	 * @brief Release all Connections without disconnecting them.
	 *
	 * After this call the group is empty and the Connections continue to
	 * live as long as the event and receiver are alive (auto-disconnect on
	 * Trackable destruction).  Useful when you want to hand off lifetime
	 * management to the objects themselves.
	 */
	void release();

	// -------------------------------------------------------------------------
	// element access
	// -------------------------------------------------------------------------

	/** @brief Access a connection by index (no bounds checking). */
	Connection& operator[]( std::size_t index );

	/** @brief Access a connection by index (no bounds checking). */
	const Connection& operator[]( std::size_t index ) const;

	// -------------------------------------------------------------------------
	// range iteration
	// -------------------------------------------------------------------------

	auto begin();
	auto begin() const;
	auto end();
	auto end() const;
};


//
// IMPLEMENTATION
//

inline ConnectionGroup::ConnectionGroup( ConnectionGroup&& other ) noexcept
	: _connections( std::move( other._connections ) )
{
}

inline ConnectionGroup& ConnectionGroup::operator=( ConnectionGroup&& other ) noexcept
{
	if ( this != &other )
	{
		disconnectAll();
		_connections = std::move( other._connections );
	}
	return *this;
}

inline ConnectionGroup::~ConnectionGroup()
{
	disconnectAll();
}

inline std::size_t ConnectionGroup::size() const
{
	return _connections.size();
}

inline std::size_t ConnectionGroup::activeCount() const
{
	std::size_t count = 0;
	for ( const auto& conn : _connections )
	{
		if ( conn.isConnected() )
		{
			++count;
		}
	}
	return count;
}

inline bool ConnectionGroup::empty() const
{
	return _connections.empty();
}

inline bool ConnectionGroup::hasActiveConnections() const
{
	for ( const auto& conn : _connections )
	{
		if ( conn.isConnected() )
		{
			return true;
		}
	}
	return false;
}

inline void ConnectionGroup::add( Connection conn )
{
	_connections.push_back( std::move( conn ) );
}

inline ConnectionGroup& ConnectionGroup::operator+=( Connection conn )
{
	add( std::move( conn ) );
	return *this;
}

inline void ConnectionGroup::disconnectAll()
{
	for ( auto& conn : _connections )
	{
		if ( conn.isConnected() )
		{
			conn.disconnect();
		}
	}
	_connections.clear();
}

inline void ConnectionGroup::blockAll()
{
	for ( auto& conn : _connections )
	{
		conn.block();
	}
}

inline void ConnectionGroup::unblockAll()
{
	for ( auto& conn : _connections )
	{
		conn.unblock();
	}
}

inline void ConnectionGroup::cleanup()
{
	_connections.erase(
		std::remove_if( _connections.begin(), _connections.end(),
			[]( const Connection& conn ) {
				return ! conn.isConnected();
			} ),
		_connections.end() );
}

inline void ConnectionGroup::release()
{
	_connections.clear();
}

inline Connection& ConnectionGroup::operator[]( std::size_t index )
{
	return _connections[ index ];
}

inline const Connection& ConnectionGroup::operator[]( std::size_t index ) const
{
	return _connections[ index ];
}

inline auto ConnectionGroup::begin()
{
	return _connections.begin();
}

inline auto ConnectionGroup::begin() const
{
	return _connections.begin();
}

inline auto ConnectionGroup::end()
{
	return _connections.end();
}

inline auto ConnectionGroup::end() const
{
	return _connections.end();
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_CONNECTION_GROUP_H
