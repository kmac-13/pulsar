#ifndef KMAC_PULSAR_COMBINING_EVENT_H
#define KMAC_PULSAR_COMBINING_EVENT_H

/**
 * @file combining_event.h
 * @brief Event that collects and aggregates handler return values.
 *
 * CombiningEvent<ReturnType, Combiner, Args...> fires all connected handlers
 * and passes their return values through a Combiner functor to produce a single
 * result.  This is useful for patterns like validation pipelines, consensus
 * queries, and aggregated data collection.
 *
 * @code
 * // validate that all checkers approve an input string
 * CombiningEvent<bool, Combiners::LogicalAnd<>, std::string> validate{this};
 *
 * validate.connect(checker1, &Checker::check);
 * validate.connect(checker2, &Checker::check);
 *
 * bool allOk = validate("input");   // true only if every checker returns true
 * @endcode
 *
 * @section combiners Built-in Combiners
 *
 * See combiners.h for the full list.  All combiners accept a range of return
 * values and produce a single result.
 *
 * @section threading Threading
 *
 * CombiningEvent only supports Direct connections - there is no Deferred variant
 * because the return value must be available synchronously.  All connected
 * handlers are invoked on the triggering thread.
 *
 * @see combiners.h for available Combiner types
 */

#include <kmac/pulsar/pulsar_fwd.h>
#include <kmac/pulsar/config.h>
#include <kmac/pulsar/connection.h>
#include <kmac/pulsar/object.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace kmac {
namespace pulsar {

/**
 * @brief Event that collects handler return values and combines them.
 *
 * @tparam ReturnType the type returned by each handler and by the event itself
 * @tparam Combiner functor that reduces a range of ReturnType values to one
 * @tparam Args argument types forwarded to every handler
 */
template< typename ReturnType, typename Combiner, typename... Args >
class CombiningEvent
{
private:
	// ======================================================================
	// ConnectionImpl<HandlerFunc>
	// Stores one typed handler and manages its lifecycle.
	// ======================================================================

	template< typename HandlerFunc >
	class ConnectionImpl : public ConnectionBase
	{
	private:
		CombiningEvent* _event;
		std::weak_ptr< Object > _sender;
		std::weak_ptr< Object > _receiver;
		HandlerFunc _handler;
		bool _singleShot;

	public:
		ConnectionImpl(
			CombiningEvent* event,
			std::weak_ptr< Object > sender,
			std::weak_ptr< Object > receiver,
			HandlerFunc&& handler,
			bool singleShot = false );

		bool isConnected() const override;

		void disconnect() override;

		bool isBlocked() const override;

		void block() override;
		void unblock() override;

		/**
		 * @brief No-op: CombiningEvent does not support Deferred connections.
		 */
		void beginMigration() override;

		/**
		 * @brief No-op: CombiningEvent does not support Deferred connections.
		 */
		void updateEventLoop( EventLoop* ) override;

		/**
		 * @brief Invoke the handler and return the result.
		 *
		 * Returns std::nullopt if the connection is disconnected, blocked, or
		 * the receiver has been destroyed.  Disconnects and returns nullopt if
		 * single-shot and the handler was called.
		 */
		std::optional< ReturnType > invoke( Args... args );
	};

	// ======================================================================
	// ConnectionWrapperBase / ConnectionWrapper<HandlerFunc>
	// Type-erased wrappers stored in the connections vector.
	// ======================================================================

	class ConnectionWrapperBase
	{
	public:
		virtual ~ConnectionWrapperBase() = default;
		virtual std::shared_ptr< ConnectionBase > getBase() = 0;
		virtual bool isConnected() const = 0;
		virtual std::optional< ReturnType > invoke( Args... args ) = 0;
		virtual void disconnect() = 0;
	};

	template< typename HandlerFunc >
	class ConnectionWrapper : public ConnectionWrapperBase
	{
	private:
		std::shared_ptr< ConnectionImpl< HandlerFunc > > _impl;

	public:
		ConnectionWrapper( std::shared_ptr< ConnectionImpl< HandlerFunc > > impl );

		std::shared_ptr< ConnectionBase > getBase() override;
		bool isConnected() const override;
		std::optional< ReturnType > invoke( Args... args ) override;
		void disconnect() override;
	};

private:
	Object* _senderObj;
	Combiner _combiner;
	mutable platform::Mutex _mutex;
	std::vector< std::unique_ptr< ConnectionWrapperBase > > _connections;

public:
	/**
	 * @brief Construct a CombiningEvent belonging to @p sender.
	 *
	 * @p sender must outlive the event.  Pass @c this for typical member usage.
	 */
	CombiningEvent( Object* sender );

	~CombiningEvent();

	// ======================================================================
	// Event Triggering - three equivalent methods
	// ======================================================================

	/**
	 * @brief Invoke all handlers and return the combined result (idiomatic
	 * C++ operator-call style).
	 *
	 * Handlers are invoked in the order they were connected.  If no handlers
	 * are connected the combiner is called with an empty range and returns
	 * its default result.
	 */
	ReturnType operator()( Args... args );

	/**
	 * @brief Identical to operator().  Explicit trigger alternative.
	 */
	ReturnType trigger( Args... args );

	/**
	 * @brief Identical to operator().  Explicit emit alternative.
	 */
	ReturnType emit( Args... args );

	// ======================================================================
	// Connection Methods
	// ======================================================================

	/**
	 * @brief Connect a lambda or functor.
	 *
	 * @param receiver shared ownership of the receiving Object
	 * @param handler callable returning ReturnType and accepting Args
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connect( std::shared_ptr< Object > receiver, HandlerFunc&& handler );

	/**
	 * @brief Connect a member function returning ReturnType.
	 *
	 * @param receiver shared ownership of the receiver, must derive from Object
	 * @param method pointer-to-member-function returning ReturnType
	 * @param singleShot whether to disconnect after first invokation
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	Connection connect(
		std::shared_ptr< ReceiverType > receiver,
		ReturnType ( ReceiverType::*method )( HandlerArgs... ),
		bool singleShot = false );

	/**
	 * @brief Connect a lambda that auto-disconnects after its first invocation.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param handler handler callable
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connectOnce( std::shared_ptr< Object > receiver, HandlerFunc&& handler );

	/**
	 * @brief Connect a member function that auto-disconnects after first invocation.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param method pointer-to-member-function
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	Connection connectOnce( std::shared_ptr< ReceiverType > receiver, ReturnType ( ReceiverType::*method )( HandlerArgs... ) );

	/**
	 * @brief Disconnect and remove all connections.
	 */
	void disconnectAll();

	/**
	 * @brief Returns the number of entries in the connection list.
	 *
	 * Includes dead connections not yet pruned.
	 */
	std::size_t connectionCount() const;

private:
	/**
	 * @brief Shared implementation for all connect overloads.
	 */
	template< typename HandlerFunc >
	Connection connectInternal( std::shared_ptr< Object > receiver, HandlerFunc&& handler, bool singleShot );

	/**
	 * @brief Remove the wrapper for @p impl.  Called by ConnectionImpl::disconnect().
	 */
	void removeConnection( ConnectionBase* impl );
};

/**
 * @brief Alias for users that prefer signal/emit terminology.
 */
template< typename ReturnType, typename Combiner, typename... Args >
using CombiningSignal = CombiningEvent< ReturnType, Combiner, Args... >;


//
// COMBINING EVENT
//

template< typename ReturnType, typename Combiner, typename... Args >
CombiningEvent< ReturnType, Combiner, Args... >::CombiningEvent( Object* sender )
	: _senderObj( sender )
	, _combiner()
{
}

template< typename ReturnType, typename Combiner, typename... Args >
CombiningEvent< ReturnType, Combiner, Args... >::~CombiningEvent()
{
	disconnectAll();
}

template< typename ReturnType, typename Combiner, typename... Args >
ReturnType CombiningEvent< ReturnType, Combiner, Args... >::operator()( Args... args )
{
	// keep (wrapper*, impl_shared_ptr) pairs so impls stay alive during invocation
	struct ConnectionHolder
	{
		ConnectionWrapperBase* wrapper;
		std::shared_ptr< ConnectionBase > impl;
	};

	std::vector< ConnectionHolder > connections;

	{
		platform::LockGuard< platform::Mutex > lock( _mutex );

		// prune dead connections opportunistically
		_connections.erase(
			std::remove_if( _connections.begin(), _connections.end(),
				[]( const std::unique_ptr< ConnectionWrapperBase >& wrapper ) {
					return ! wrapper || ! wrapper->isConnected();
				} ),
			_connections.end() );

		connections.reserve( _connections.size() );
		for ( auto& conn : _connections )
		{
			auto base = conn->getBase();
			if ( base && base->isConnected() )
			{
				connections.push_back( { conn.get(), base } );
			}
		}
	}
	// mutex released before invoking user code

	std::vector< ReturnType > results;
	results.reserve( connections.size() );

	for ( auto& holder : connections )
	{
		if ( holder.impl && holder.impl->isConnected() )
		{
			// args are lvalues here - don't forward (multiple handlers read the same values)
			auto result = holder.wrapper->invoke( args... );
			if ( result.has_value() )
			{
				results.push_back( std::move( result.value() ) );
			}
		}
	}

	return _combiner( results.begin(), results.end() );
}

template< typename ReturnType, typename Combiner, typename... Args >
ReturnType CombiningEvent< ReturnType, Combiner, Args... >::trigger( Args... args )
{
	return operator()( std::forward< Args >( args )... );
}

template< typename ReturnType, typename Combiner, typename... Args >
ReturnType CombiningEvent< ReturnType, Combiner, Args... >::emit( Args... args )
{
	return operator()( std::forward< Args >( args )... );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
Connection CombiningEvent< ReturnType, Combiner, Args... >::connect( std::shared_ptr< Object > receiver, HandlerFunc&& handler )
{
	return connectInternal( receiver, std::forward< HandlerFunc >( handler ), false );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
Connection CombiningEvent< ReturnType, Combiner, Args... >::connect(
	std::shared_ptr< ReceiverType > receiver,
	ReturnType ( ReceiverType::*method )( HandlerArgs... ),
	bool singleShot )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	// capture raw pointer to avoid circular reference;
	// ConnectionImpl::invoke() checks receiver liveness before calling the lambda
	ReceiverType* rawReceiver = receiver.get();

	auto handler = [ rawReceiver, method ]( Args... args ) -> ReturnType {
		return ( rawReceiver->*method )( args... );  // args are lvalues - don't forward
	};

	return connectInternal( receiver, std::move( handler ), singleShot );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
Connection CombiningEvent< ReturnType, Combiner, Args... >::connectOnce( std::shared_ptr< Object > receiver, HandlerFunc&& handler )
{
	return connectInternal( receiver, std::forward< HandlerFunc >( handler ), true );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
Connection CombiningEvent< ReturnType, Combiner, Args... >::connectOnce(
	std::shared_ptr< ReceiverType > receiver,
	ReturnType ( ReceiverType::*method )( HandlerArgs... ) )
{
	return connect( receiver, method, true );
}

template< typename ReturnType, typename Combiner, typename... Args >
void CombiningEvent< ReturnType, Combiner, Args... >::disconnectAll()
{
	std::vector< std::unique_ptr< ConnectionWrapperBase > > connectionsToDisconnect;

	{
		platform::LockGuard< platform::Mutex > lock( _mutex );
		connectionsToDisconnect = std::move( _connections );
		_connections.clear();
	}

	// disconnect outside the mutex: disconnect() calls removeConnection() which
	// tries to acquire the mutex, so we must not hold it here
	for ( auto& conn : connectionsToDisconnect )
	{
		if ( conn )
		{
			conn->disconnect();
		}
	}
}

template< typename ReturnType, typename Combiner, typename... Args >
size_t CombiningEvent< ReturnType, Combiner, Args... >::connectionCount() const
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	return _connections.size();
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
Connection CombiningEvent< ReturnType, Combiner, Args... >::connectInternal(
	std::shared_ptr< Object > receiver,
	HandlerFunc&& handler,
	bool singleShot )
{
	platform::LockGuard< platform::Mutex > lock( _mutex );

	auto senderPtr = _senderObj ? _senderObj->shared_from_this() : std::shared_ptr< Object >();

	auto connImpl = std::make_shared< ConnectionImpl< HandlerFunc > >(
		this, senderPtr, receiver, std::forward< HandlerFunc >( handler ), singleShot );

	auto wrapper = std::make_unique< ConnectionWrapper< HandlerFunc > >( connImpl );
	_connections.push_back( std::move( wrapper ) );

	if ( _senderObj )
	{
		_senderObj->registerConnection( connImpl );
	}
	receiver->registerConnection( connImpl );

	return Connection( connImpl );
}

template< typename ReturnType, typename Combiner, typename... Args >
void CombiningEvent< ReturnType, Combiner, Args... >::removeConnection( ConnectionBase* impl )
{
	platform::LockGuard< platform::Mutex > lock( _mutex );
	_connections.erase(
		std::remove_if( _connections.begin(), _connections.end(),
			[ impl ]( const std::unique_ptr< ConnectionWrapperBase >& wrapper ) {
				if( ! wrapper ) return true;
				auto base = wrapper->getBase();
				return ! base || base.get() == impl;
			} ),
		_connections.end() );
}


//
// CONNECTION IMPL
//

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::ConnectionImpl(
	CombiningEvent* event,
	std::weak_ptr< Object > sender,
	std::weak_ptr< Object > receiver,
	HandlerFunc&& handler,
	bool singleShot )
	: _event( event )
	, _sender( sender )
	, _receiver( receiver )
	, _handler( std::forward< HandlerFunc >( handler ) )
	, _singleShot( singleShot )
{
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
bool CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::isConnected() const
{
	return _connected;
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::disconnect()
{
	if ( _connected.exchange( false ) )
	{
		if ( _event )
		{
			_event->removeConnection( this );
		}
	}
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
bool CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::isBlocked() const
{
	return _blocked.load( std::memory_order_acquire );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::block()
{
	_blocked.store( true, std::memory_order_release );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::unblock()
{
	_blocked.store( false, std::memory_order_release );
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::beginMigration()
{
	// CombiningEvent does not support Deferred connections, so nothing to migrate
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::updateEventLoop( EventLoop* )
{
	// CombiningEvent does not support Deferred connections, so nothing to migrate
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
std::optional< ReturnType > CombiningEvent< ReturnType, Combiner, Args... >::ConnectionImpl< HandlerFunc >::invoke( Args... args )
{
	if ( ! _connected || _blocked.load( std::memory_order_acquire ) )
	{
		return std::nullopt;
	}

	auto rec = _receiver.lock();
	if ( ! rec )
	{
		// receiver has been destroyed - disconnect and skip
		disconnect();
		return std::nullopt;
	}

	ReturnType result = _handler( std::forward< Args >( args )... );

	if ( _singleShot )
	{
		disconnect();
	}

	return result;
}


//
// CONNECTION WRAPPER
//

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
CombiningEvent< ReturnType, Combiner, Args... >::ConnectionWrapper< HandlerFunc >::ConnectionWrapper(
	std::shared_ptr< ConnectionImpl< HandlerFunc > > impl )
	: _impl( impl )
{
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
std::shared_ptr< ConnectionBase > CombiningEvent< ReturnType, Combiner, Args... >::ConnectionWrapper< HandlerFunc >::getBase()
{
	return _impl;
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
bool CombiningEvent< ReturnType, Combiner, Args... >::ConnectionWrapper< HandlerFunc >::isConnected() const
{
	return _impl && _impl->isConnected();
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
std::optional< ReturnType > CombiningEvent< ReturnType, Combiner, Args... >::ConnectionWrapper< HandlerFunc >::invoke( Args... args )
{
	if ( _impl )
	{
		return _impl->invoke( std::forward< Args >( args )... );
	}
	return std::nullopt;
}

template< typename ReturnType, typename Combiner, typename... Args >
template< typename HandlerFunc >
void CombiningEvent< ReturnType, Combiner, Args... >::ConnectionWrapper< HandlerFunc >::disconnect()
{
	if ( _impl )
	{
		_impl->disconnect();
	}
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_COMBINING_EVENT_H
