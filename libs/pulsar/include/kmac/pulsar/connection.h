#pragma once
#ifndef KMAC_PULSAR_CONNECTION_H
#define KMAC_PULSAR_CONNECTION_H

/**
 * @file connection.h
 * @brief User-facing connection handle returned by Event::connect().
 *
 * @section connection Connection
 *
 * Holds a WeakPtr<EventImplBase> + handler index + generation counter (24 bytes).
 * The WeakPtr expires when the owning BasicEvent is destroyed; index + generation
 * identify the specific handler entry and detect stale handles after disconnect
 * or index reuse.
 *
 * All copies of a Connection refer to the same handler; calling disconnect() on
 * any copy disconnects them all.  Safe to call on a null or expired Connection.
 *
 * @section scoped ScopedConnection
 *
 * RAII wrapper that calls disconnect() on destruction.  Move-only.
 */

#include "callable.h"
#include "event_impl_base.h"
#include "platform.h"

#include <cstdint>

namespace kmac {
namespace pulsar {

class Connection
{
private:
	platform::WeakPtr< EventImplBase > _impl;  ///< the owning event's impl; expired() once that event is destroyed
	uint32_t _index = 0;       ///< stable handle index into the event's handler array
	uint16_t _generation = 0;  ///< generation stamped on the handle when this Connection was made

public:
	/**
	 * @brief Constructs a null Connection: isConnected() is false and
	 * disconnect()/block()/unblock() are all no-ops.
	 */
	Connection() = default;

	/**
	 * @brief Constructs a live handle to the handler at `index` (with the
	 * given `generation`) in the event behind `impl`.  Called by
	 * EventStorage::connectImpl() when a handler slot is filled; not meant
	 * to be constructed directly by users.
	 */
	explicit Connection(
		platform::WeakPtr< EventImplBase > impl,
		uint32_t index,
		uint32_t generation );

	/**
	 * @brief True if the underlying event still exists and this handle's
	 * generation still matches the slot's current generation - i.e. the
	 * connection has not been disconnected and its slot has not been reused
	 * by a later connection.  False for a null, disconnected, or stale
	 * Connection.
	 */
	bool isConnected() const;

	/**
	 * @brief Removes this connection from its event, if it is still
	 * connected.  All copies of this Connection observe the disconnect (see
	 * file docs above).  Safe to call on a null, already-disconnected, or
	 * stale Connection - a no-op in every one of those cases.
	 */
	void disconnect();

	/**
	 * @brief Returns true if this specific connection is currently blocked.
	 * Always false for a disconnected or stale (generation mismatch)
	 * Connection.
	 */
	bool isBlocked() const;

	/**
	 * @brief Suppress dispatch through this connection only, leaving every
	 * other connection on the same event unaffected.  Matches
	 * EventImpl::block()'s timing model: only trigger()s that occur while
	 * blocked are suppressed - a Deferred invocation already posted to an
	 * EventLoop before blocking still runs.  No-op if disconnected.
	 */
	void block();

	/**
	 * @brief Resume dispatch through this connection.  No-op if
	 * disconnected or not currently blocked.
	 */
	void unblock();
};

// ---------------------------------------------------------------------------

inline Connection::Connection(
	platform::WeakPtr< EventImplBase > impl,
	uint32_t index,
	uint32_t generation )
	: _impl( std::move( impl ) )
	, _index( index )
	, _generation( generation )
{
}

inline bool Connection::isConnected() const
{
	auto impl = _impl.lock();
	return impl && impl->isHandlerConnected( _index, _generation );
}

inline void Connection::disconnect()
{
	if ( auto impl = _impl.lock() )
	{
		impl->disconnectHandler( _index, _generation );
	}
}

inline bool Connection::isBlocked() const
{
	auto impl = _impl.lock();
	return impl && impl->isHandlerBlocked( _index, _generation );
}

inline void Connection::block()
{
	if ( auto impl = _impl.lock() )
	{
		impl->blockHandler( _index, _generation );
	}
}

inline void Connection::unblock()
{
	if ( auto impl = _impl.lock() )
	{
		impl->unblockHandler( _index, _generation );
	}
}

// ===========================================================================

class ScopedConnection
{
private:
	Connection _connection;

public:
	/**
	 * @brief Constructs an empty guard - owns no connection, disconnect() on
	 * destruction is a no-op.
	 */
	ScopedConnection() = default;

	/**
	 * @brief Takes ownership of `connection`; disconnects it on destruction
	 * unless release() is called first.
	 */
	explicit ScopedConnection( Connection connection );

	/**
	 * @brief Disconnects the owned connection, if any (see
	 * Connection::disconnect() - safe even if already disconnected).
	 */
	~ScopedConnection();

	ScopedConnection( const ScopedConnection& ) = delete;
	ScopedConnection& operator=( const ScopedConnection& ) = delete;
	ScopedConnection( ScopedConnection&& ) = default;
	ScopedConnection& operator=( ScopedConnection&& ) = default;

	/**
	 * @brief Forwards to the owned Connection::isConnected().
	 */
	bool isConnected() const;

	/**
	 * @brief Releases ownership without disconnecting - the connection stays alive.
	 */
	void release();
};

// ---------------------------------------------------------------------------

inline ScopedConnection::ScopedConnection( Connection connection )
	: _connection( std::move( connection ) )
{
}

inline ScopedConnection::~ScopedConnection()
{
	_connection.disconnect();
}

inline bool ScopedConnection::isConnected() const
{
	return _connection.isConnected();
}

inline void ScopedConnection::release()
{
	_connection = Connection{};
}

/**
 * @brief RAII guard that blocks an event for its lifetime.
 *
 * Returned by BasicEvent::blockGuard().  While any BlockGuard for an event
 * exists, calls to operator()() / trigger() / emit() are silently
 * dropped before any dispatch or task-posting takes place.
 *
 * Guards nest correctly: if two guards are held simultaneously the event
 * stays blocked until both are destroyed, or until unblock is manually called
 * twice.
 *
 * Deliberately not templated on MutexType/Args, so one BlockGuard type
 * works for Event, SharedEvent, and SingleThreadedEvent alike - the
 * concrete EventImpl::block() call happens in blockGuard() itself (which
 * still knows the concrete type), before this guard is even constructed;
 * this guard only needs to keep the event alive (_keepAlive) and remember
 * how to unblock it later (_unblock), neither of which requires knowing
 * the concrete type.
 *
 * @code
 * auto guard = ev.blockGuard();  // ev is now blocked
 * ev( 42 );                      // dropped
 * // guard destroyed - ev unblocked
 * @endcode
 */
class BlockGuard
{
	template< typename MutexType, typename... Args >
	friend class BasicEvent;

	template< template< typename, typename... > class ImplT, typename MutexType, typename... Args >
	friend class EventStorage;

private:
	platform::SharedPtr< EventImplBase > _keepAlive;  ///< kept alive for the guard's lifetime; never dereferenced - see _unblock below
	Callable< void() > _unblock;  ///< bound (cheaply, no heap allocation) to the concrete EventImpl::unblock() at construction time, when the concrete type is still known

	/**
	 * @brief Only constructible by BasicEvent::block()/EventStorage::blockGuard(),
	 * which have already called the concrete EventImpl::block() themselves
	 * before constructing this guard - see the class docs above for why.
	 */
	BlockGuard( platform::SharedPtr< EventImplBase > keepAlive, Callable< void() > unblock );

public:
	/**
	 * @brief Unblocks the event, unless this guard was moved-from (in which
	 * case _unblock is empty and there is nothing to do).
	 */
	~BlockGuard();

	BlockGuard( BlockGuard&& ) = default;
	BlockGuard& operator=( BlockGuard&& ) = default;

	BlockGuard( const BlockGuard& ) = delete;
	BlockGuard& operator=( const BlockGuard& ) = delete;
};

inline BlockGuard::BlockGuard( platform::SharedPtr< EventImplBase > keepAlive, Callable< void() > unblock )
	: _keepAlive( std::move( keepAlive ) )
	, _unblock( std::move( unblock ) )
{
}

inline BlockGuard::~BlockGuard()
{
	if ( _unblock )
	{
		_unblock();
	}
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONNECTION_H
