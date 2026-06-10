#ifndef KMAC_PULSAR_CONNECTION_GROUP_H
#define KMAC_PULSAR_CONNECTION_GROUP_H

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
 * class Widget : public pulsar::Object
 * {
 * private:
 *     pulsar::ConnectionGroup _connections;
 *
 * public:
 *     void setModel(std::shared_ptr<Model> model)
 *     {
 *         _connections.disconnectAll();  // disconnect from old model
 *
 *         _connections += model->dataChanged.connect(shared_from_this(), &Widget::onDataChanged);
 *         _connections += model->titleChanged.connect(shared_from_this(), &Widget::onTitleChanged);
 *     }
 * };
 * @endcode
 *
 * @note Connections are also disconnected automatically when the owning
 * Object is destroyed, so ConnectionGroup is not needed purely for cleanup
 * on destruction.
 *
 * ConnectionGroup is move-only (non-copyable).
 *
 * @see ScopedConnection for managing a single connection with RAII
 */

#include <kmac/pulsar/connection.h>

#include <algorithm>
#include <vector>

namespace kmac {
namespace pulsar {

class ConnectionGroup
{
private:
	std::vector< Connection > _connections;

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
	// Inspection
	// -------------------------------------------------------------------------

	/**
	 * @brief Returns the total number of connections (including disconnected ones not yet cleaned up).
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
	// Adding connections
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
	 * _connections += event.connect(receiver, handler);
	 * @endcode
	 */
	ConnectionGroup& operator+=( Connection conn );

	// -------------------------------------------------------------------------
	// Bulk operations
	// -------------------------------------------------------------------------

	/**
	 * @brief Disconnect and remove all connections in the group.
	 */
	void disconnectAll();

	/**
	 * @brief Block all connections in the group.
	 */
	void blockAll();

	/**
	 * @brief Unblock all connections in the group.
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
	// Ownership transfer
	// -------------------------------------------------------------------------

	/**
	 * @brief Release all Connections without disconnecting them.
	 *
	 * After this call, the group is empty and the Connections continue to
	 * live as long as the event and receiver are alive (auto-disconnect).
	 * Useful when you want to hand off lifetime management to the objects
	 * themselves.
	 */
	void release();

	// -------------------------------------------------------------------------
	// Element access
	// -------------------------------------------------------------------------

	/**
	 * @brief Access a connection by index (no bounds checking).
	 */
	Connection& operator[]( std::size_t index );

	/**
	 * @brief Access a connection by index (no bounds checking).
	 */
	const Connection& operator[]( std::size_t index ) const;

	// -------------------------------------------------------------------------
	// Range iteration
	// -------------------------------------------------------------------------

	auto begin();
	auto begin() const;
	auto end();
	auto end() const;
};


//
// IMPLEMENTATION
//

ConnectionGroup::ConnectionGroup( ConnectionGroup&& other ) noexcept
	: _connections( std::move( other._connections ) )
{
}

ConnectionGroup& ConnectionGroup::operator=( ConnectionGroup&& other ) noexcept
{
	if ( this != &other )
	{
		disconnectAll();
		_connections = std::move( other._connections );
	}
	return *this;
}

ConnectionGroup::~ConnectionGroup()
{
	disconnectAll();
}

std::size_t ConnectionGroup::size() const
{
	return _connections.size();
}

std::size_t ConnectionGroup::activeCount() const
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

bool ConnectionGroup::empty() const
{
	return _connections.empty();
}

bool ConnectionGroup::hasActiveConnections() const
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

void ConnectionGroup::add( Connection conn )
{
	_connections.push_back( std::move( conn ) );
}

ConnectionGroup& ConnectionGroup::operator+=( Connection conn )
{
	add( std::move( conn ) );
	return *this;
}

void ConnectionGroup::disconnectAll()
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

void ConnectionGroup::blockAll()
{
	for ( auto& conn : _connections )
	{
		conn.block();
	}
}

void ConnectionGroup::unblockAll()
{
	for ( auto& conn : _connections )
	{
		conn.unblock();
	}
}

void ConnectionGroup::cleanup()
{
	_connections.erase(
		std::remove_if( _connections.begin(), _connections.end(),
			[]( const Connection& conn ) {
				return ! conn.isConnected();
			} ),
		_connections.end() );
}

void ConnectionGroup::release()
{
	_connections.clear();
}

Connection& ConnectionGroup::operator[]( std::size_t index )
{
	return _connections[ index ];
}

const Connection& ConnectionGroup::operator[]( std::size_t index ) const
{
	return _connections[ index ];
}

auto ConnectionGroup::begin()
{
	return _connections.begin();
}

auto ConnectionGroup::begin() const
{
	return _connections.begin();
}

auto ConnectionGroup::end()
{
	return _connections.end();
}

auto ConnectionGroup::end() const
{
	return _connections.end();
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONNECTION_GROUP_H
