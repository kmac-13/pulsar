#ifndef KMAC_PULSAR_EVENT_H
#define KMAC_PULSAR_EVENT_H

/**
 * @file event.h
 * @brief Core Event class template: type-safe event dispatching with automatic
 * connection lifecycle management.
 *
 * Event<Args...> is the primary building block of Pulsar.  Place Event members
 * inside any Object-derived class to expose observable state changes or actions:
 *
 * @code
 * class Button : public pulsar::Object
 * {
 * public:
 *     pulsar::Event<int, int> clicked{this};
 *
 *     void click(int x, int y) { clicked(x, y); }
 * };
 * @endcode
 *
 * @section triggering Triggering
 *
 * Three equivalent methods are provided:
 * - operator() - idiomatic C++, recommended for new code
 * - trigger()  - explicit alternative
 * - emit()     - familiar to Qt users
 *
 * All three forward to the same internal triggerImpl() which can be overridden
 * by subclasses (see RecordableEvent).
 *
 * @section connections Connections
 *
 * Handlers can be member functions, lambdas, or free functions.  The
 * connection type (Direct / Deferred / Auto) controls which EventLoop executes
 * the handler.  Auto (the default) picks Direct when both objects share a
 * loop at trigger-time, Deferred otherwise.
 *
 * @section lifetime Lifetime Management
 *
 * Connections are automatically severed when either the sender or receiver is
 * destroyed.  You only need to store Connection handles when you want to
 * disconnect, block, or inspect them manually.
 *
 * @section design Performance Design
 *
 * To avoid the overhead of std::function (type erasure at the API boundary,
 * potential heap allocation, no move-only capture support), Event uses a
 * two-layer template design:
 *
 * - ConnectionImpl<HandlerFunc>:
 *   Owns the exact callable type; can be inlined.
 *
 * - ConnectionWrapper<HandlerFunc>:
 *   Type-erased wrapper stored in the connections vector.  One virtual call
 *   per emission at this boundary, then a direct typed call.
 *
 * This gives the compiler maximum opportunity to inline handler callback
 * invocations while keeping the connection list type-erased for storage.
 */

// protect against Windows macro pollution
#ifdef _WIN32
#ifdef Args
#undef Args
#endif
#endif

// Qt defines: #define emit
// save and suppress it so our emit() method compiles; the macro is restored
// at the bottom of this file, leaving the translation unit unaffected
#pragma push_macro("emit")
#undef emit

#include "config.h"
#include "pulsar_fwd.h"
#include "connection.h"
#include "inspection_info.h"
#include "event_loop.h"
#include "object.h"
#include "receiver_lifetime_anchor.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

namespace kmac {
namespace pulsar {

namespace detail {

template< typename Receiver, typename... HandlerArgs, size_t... IndexSequence, typename... EventArgs >
auto makePartialHandlerImpl(
	Receiver* raw,
	void ( Receiver::*method )( HandlerArgs... ),
	std::index_sequence< IndexSequence... >,
	std::tuple< EventArgs... >* )
{
	return [ raw, method ]( EventArgs... args )
	{
		auto argTuple = std::forward_as_tuple( std::forward< EventArgs >( args )... );
		( raw->*method )(
			std::forward< std::tuple_element_t< IndexSequence, std::tuple< EventArgs... > > >(
				std::get< IndexSequence >( argTuple ) )... );
	};
}

/**
 * @brief Supports partial-argument connections.
 *
 * Wraps a member function pointer in a lambda that accepts the event's full
 * argument list (EventArgs...) but forwards only the first sizeof...(HandlerArgs)
 * arguments to the method.  This enables partial argument matching: a slot with
 * fewer parameters than the event compiles and silently drops trailing args.
 *
 * The EventArgs... pack is provided by the connect overload (which knows Args...)
 * and passed as a null type-tag pointer so it participates in deduction.
 * HandlerArgs... is deduced from the method pointer.
 *
 * A static_assert fires at connect time if the handler requests more arguments
 * than the event provides, giving a clear error rather than a substitution maze.
 * @param raw
 */
template< typename... EventArgs, typename Receiver, typename... HandlerArgs >
auto makePartialHandler(
	Receiver* raw,
	void ( Receiver::*method )( HandlerArgs... ) )
{
	static_assert( sizeof...( HandlerArgs ) <= sizeof...( EventArgs ),
		"Handler has more arguments than the event provides" );

	return makePartialHandlerImpl(
		raw, method,
		std::make_index_sequence< sizeof...( HandlerArgs ) >{},
		static_cast< std::tuple< EventArgs... >* >( nullptr ) );
}

} // namespace detail


/**
 * @brief Type-safe event with automatic connection lifecycle management.
 *
 * @tparam Args argument types forwarded to every connected handler on emission
 *
 * @see PrivateEvent for events where only the owning class can trigger
 * @see CombiningEvent for events that collect and combine handler return values
 * @see RecordableEvent for a version with statistics and recording support
 */
template< typename... Args >
class Event
{
	friend class EventInspector< Args... >;

private:
	// ======================================================================
	// ConnectionImpl<HandlerFunc>
	// Owns one typed handler.  Stored via shared_ptr so the Connection handle
	// and the wrapper can both keep it alive independently.
	// ======================================================================

	template< typename HandlerFunc >
	class ConnectionImpl
		: public ConnectionBase
		, public std::enable_shared_from_this< ConnectionImpl< HandlerFunc > >
	{
	private:
		Event* _event;                      ///< back-pointer to the owning event
		std::weak_ptr< Object > _sender;    ///< sender - used for inspection only
		std::weak_ptr< Object > _receiver;  ///< receiver - checked for liveness before every invocation
		std::shared_ptr< std::decay_t< HandlerFunc > > _handler;  ///< the actual handler; decay_t normalises reference/cv types; shared_ptr allows sharing into deferred lambdas without copying (supports move-only captures)
		ConnectionType _type;               ///< Direct, Deferred, or Auto
		EventLoop* _loop;                   ///< target loop for explicit Deferred or Auto resolved to Deferred connections; nullptr for Direct and Auto
		bool _singleShot;                   ///< disconnect automatically after first invocation
		std::atomic< bool > _migrating;     ///< true while setEventLoop() migration is in progress

	public:
		ConnectionImpl(
			Event* event,
			std::weak_ptr< Object > sender,
			std::weak_ptr< Object > receiver,
			HandlerFunc&& handler,
			ConnectionType type,
			EventLoop* loop,
			bool singleShot = false );

		bool isConnected() const override;
		void disconnect() override;

		bool isBlocked() const override;
		void block() override;
		void unblock() override;

		ConnectionType type() const;
		std::weak_ptr< Object > sender() const;
		std::weak_ptr< Object > receiver() const;
		bool isSingleShot() const;

		/**
		 * @brief Suspend deferred dispatch during loop migration.
		 */
		void beginMigration() override;

		/**
		 * @brief Resume deferred dispatch with the new loop after migration.
		 *
		 * For explicit deferred connections, also logs a warning if the new loop
		 * creates or resolves a mismatch with the sender's loop.
		 */
		void updateEventLoop( EventLoop* newLoop ) override;

		/**
		 * @brief Invoke the handler, choosing Direct or Deferred dispatch.
		 *
		 * For Auto connections, the choice is made dynamically at trigger-time
		 * by comparing the sender's and receiver's current event loops.
		 */
		void invoke( Args... args );

		/**
		 * @brief Queue invocation using a pre-built shared argument tuple.
		 *
		 * Called by triggerImpl() for Deferred connections when args have
		 * already been packed into a shared_ptr<tuple> to avoid per-connection
		 * copying.
		 */
		void invokeDeferred( std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs );

	private:
		/**
		 * @brief Check all preconditions before invoking the handler.
		 */
		bool canInvoke() const;

		/**
		 * @brief Execute the handler directly and disconnect if single-shot.
		 */
		void invokeHandler( Args... args );

		/**
		 * @brief Post handler invocation to the receiver's event loop (copies args).
		 */
		void queueInvocation( Args... args );

		/**
		 * @brief Post handler invocation to an explicitly specified loop.
		 *
		 * Used by Auto connections so they can queue to the receiver's current
		 * loop without storing it in _loop between triggers.
		 */
		void queueInvocationToLoop( EventLoop* loop, Args... args );

		/**
		 * @brief Post handler invocation using an existing shared argument tuple.
		 *
		 * Avoids re-copying args when multiple Deferred connections share the
		 * same emission.
		 */
		void queueInvocationWithTuple( EventLoop* loop, std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs );
	};

	// ======================================================================
	// ConnectionWrapperBase / ConnectionWrapper<HandlerFunc>
	// Type-erased wrappers stored in the connections vector.
	// One virtual call per emission boundary; the actual handler call is typed.
	// ======================================================================

	class ConnectionWrapperBase
	{
	public:
		virtual ~ConnectionWrapperBase() = default;
		virtual void invokeDirect( Args... args ) = 0;
		virtual void invokeDeferred( std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs ) = 0;

		virtual bool isConnected() const = 0;
		virtual void disconnect() = 0;

		virtual ConnectionType type() const = 0;
		virtual std::shared_ptr< ConnectionBase > getBase() = 0;
		virtual int priority() const = 0;
		virtual bool matchesReceiver( const Object* receiver ) const = 0;
		virtual bool matchesHandler( const void* funcPtr ) const = 0;
		virtual ConnectionInfo getInfo() const = 0;
	};

	template< typename HandlerFunc >
	class ConnectionWrapper : public ConnectionWrapperBase
	{
	private:
		std::shared_ptr< ConnectionImpl< HandlerFunc > > _impl;
		int _priority;         ///< higher values execute first
		const void* _funcPtr;  ///< used for disconnectFree() matching; nullptr for lambdas

	public:
		ConnectionWrapper(
			std::shared_ptr< ConnectionImpl< HandlerFunc > > impl,
			int priority,
			const void* funcPtr = nullptr );

		void invokeDirect( Args... args ) override;
		void invokeDeferred( std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs ) override;

		bool isConnected() const override;
		void disconnect() override;

		ConnectionType type() const override;
		std::shared_ptr< ConnectionBase > getBase() override;
		int  priority() const override;
		bool matchesReceiver( const Object* receiver ) const override;
		bool matchesHandler( const void* funcPtr ) const override;
		ConnectionInfo getInfo() const override;
	};

private:
	Object* _senderObj;                         ///< the Object that owns this event
	std::atomic< bool > _destroying { false };  ///< set in destructor to prevent re-entrant removeConnection
	mutable platform::SharedMutex _mutex;
	std::vector< std::unique_ptr< ConnectionWrapperBase > > _connections;  ///< sorted by descending priority

public:
	/**
	 * @brief Construct an event belonging to @p sender.
	 *
	 * @p sender must outlive the Event.  Typically @p sender is @c this of
	 * the enclosing Object:
	 * @code
	 * pulsar::Event<int> valueChanged{this};
	 * @endcode
	 */
	Event( Object* sender );

	virtual ~Event();

	// ======================================================================
	// Event Triggering - three equivalent methods
	// ======================================================================
	// Auto-deferred to the sender's event loop thread if called from a
	// different thread (when the sender has a loop assigned).
	// ======================================================================

	/**
	 * @brief Trigger the event.  Idiomatic C++ style; recommended for new code.
	 */
	void operator()( Args... args );

	/**
	 * @brief Trigger the event.  Alternative explicit style.
	 */
	void trigger( Args... args );

	/**
	 * @brief Trigger the event.  Qt-compatible style.
	 */
	void emit( Args... args );

	// ======================================================================
	// Connection Methods
	// ======================================================================

	/**
	 * @brief Connect a lambda or functor to this event.
	 *
	 * @param receiver shared ownership of the receiving Object, the connection
	 *   is automatically severed when receiver is destroyed
	 * @param handler any callable with a signature compatible with void(Args...),
	 *   move-only captures (e.g. std::unique_ptr) are supported
	 * @param type connection type, Auto (default) resolves at trigger-time
	 * @return Connection handle for manual lifecycle control
	 */
	template< typename HandlerFunc >
	Connection connect(
		std::shared_ptr< Object > receiver,
		HandlerFunc&& handler,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function pointer to this event.
	 *
	 * @param receiver shared ownership of the receiver, ReceiverType must derive
	 *   from Object
	 * @param method pointer-to-member-function, the receiver is captured as a
	 *   raw pointer inside the lambda (no circular reference)
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	Connection connect(
		std::shared_ptr< ReceiverType > receiver,
		void ( ReceiverType::*method )( HandlerArgs... ),
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function via a ReceiverLifetimeAnchor.
	 *
	 * Allows any class to receive events without inheriting from Object or
	 * being shared_ptr-managed.  The anchor provides both the lifetime token
	 * (its internal Object) and the raw owner pointer used to call the method.
	 *
	 * @code
	 * class Controller
	 * {
	 * public:
	 *     void onValue( const int& v ) { ... }
	 *     pulsar::ReceiverLifetimeAnchor< Controller > pulsarAnchor { this };
	 * };
	 *
	 * Controller ctrl;
	 * event.connect( ctrl.pulsarAnchor, &Controller::onValue );
	 * @endcode
	 *
	 * @param anchor the ReceiverLifetimeAnchor embedded in the receiver
	 * @param method pointer-to-member-function on Owner
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename Owner, typename... HandlerArgs >
	Connection connect(
		ReceiverLifetimeAnchor< Owner >& anchor,
		void ( Owner::*method )( HandlerArgs... ),
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a free function or non-capturing lambda (no receiver).
	 *
	 * Free-function connections are always Direct and do not participate in
	 * receiver lifetime tracking.  They remain connected until explicitly
	 * disconnected via disconnectFree() or disconnectAll().
	 *
	 * @param handler free function or non-capturing lambda
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connectFree( HandlerFunc&& handler, bool singleShot = false );

	/**
	 * @brief Connect a handler that auto-disconnects after its first invocation.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param handler handler callable
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connectOnce(
		std::shared_ptr< Object > receiver,
		HandlerFunc&& handler,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function that auto-disconnects after first invocation.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param method pointer-to-member-function
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	Connection connectOnce(
		std::shared_ptr< ReceiverType > receiver,
		void ( ReceiverType::*method )( HandlerArgs... ),
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a free function that auto-disconnects after first invocation.
	 *
	 * @param handler free function or non-capturing lambda
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connectOnceFree( HandlerFunc&& handler );

	/**
	 * @brief Connect a handler that is only invoked when @p condition returns true.
	 *
	 * The condition predicate is evaluated on the same thread as the handler,
	 * immediately before the handler runs.  For Direct connections this is the
	 * triggering thread; for Deferred connections this is the receiver's EventLoop
	 * drain thread at dequeue time.
	 *
	 * Predicates that inspect only the event arguments or atomic state are safe
	 * for any connection type.  Predicates that inspect non-atomic receiver state
	 * should not be used with Deferred connections, as that state may have changed
	 * between emission and dequeue.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param handler callable invoked when @p condition returns true
	 * @param condition predicate receiving the same arguments as the event
	 * @param type connection type (default: Auto)
	 * @return Connection handle
	 */
	template< typename HandlerFunc, typename ConditionFunc >
	Connection connectIf(
		std::shared_ptr< Object > receiver,
		HandlerFunc&& handler,
		ConditionFunc&& condition,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function with a condition predicate.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param method pointer-to-member-function
	 * @param condition predicate called with the event args
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs, typename ConditionFunc >
	Connection connectIf(
		std::shared_ptr< ReceiverType > receiver,
		void ( ReceiverType::*method )( HandlerArgs... ),
		ConditionFunc&& condition,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a handler with an explicit execution priority.
	 *
	 * Higher priority values execute before lower ones.  Default priority (used
	 * by plain connect()) is 0.  Connections with equal priority execute in
	 * insertion order.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param handler handler callable
	 * @param priority execution priority, positive = higher, negative = lower
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename HandlerFunc >
	Connection connectWithPriority(
		std::shared_ptr< Object > receiver,
		HandlerFunc&& handler,
		int priority,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function with an explicit execution priority.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param method pointer-to-member-function
	 * @param priority execution priority
	 * @param type connection type
	 * @return Connection handle
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	Connection connectWithPriority(
		std::shared_ptr< ReceiverType > receiver,
		void ( ReceiverType::*method )( HandlerArgs... ),
		int priority,
		ConnectionType type = ConnectionType::Auto );

	// ======================================================================
	// Disconnection Methods
	// ======================================================================

	/**
	 * @brief Disconnect and remove all connections.
	 */
	void disconnectAll();

	/**
	 * @brief Disconnect all connections whose receiver is @p receiver.
	 *
	 * Thread-safe.  Does not require a stored Connection handle.
	 *
	 * @param receiver raw pointer to the receiver Object
	 */
	void disconnect( const Object* receiver );

	/**
	 * @brief Disconnect all connections whose receiver is @p receiver.
	 *
	 * Convenience overload accepting a shared_ptr.
	 */
	void disconnect( std::shared_ptr< Object > receiver );

	/**
	 * @brief Disconnect all connections from @p receiver to a specific member function.
	 *
	 * @param receiver shared ownership of the receiver
	 * @param method specific method to disconnect, other methods on the same receiver
	 *   remain connected
	 */
	template< typename ReceiverType, typename... HandlerArgs >
	void disconnect( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ) );

	/**
	 * @brief Disconnect a free function previously connected via connectFree().
	 *
	 * Matching is performed by function pointer comparison, so non-capturing
	 * lambdas that convert to the same function pointer will also match.
	 *
	 * @param func the exact function pointer used when connecting
	 */
	template< typename... HandlerArgs >
	void disconnectFree( void (*func)( HandlerArgs... ) );

	/**
	 * @brief Forward all emissions of this event to @p other.
	 *
	 * Each time this event fires, @p other is triggered with the same
	 * arguments via a Direct connection.  The forwarding connection is
	 * returned and can be disconnected to stop forwarding.
	 *
	 * @param other target event to forward to (must have identical Args)
	 * @return Connection handle for the forwarding connection
	 */
	Connection forwardTo( Event< Args... >& other );

protected:
	/**
	 * @brief Core emission implementation, overridable by subclasses.
	 *
	 * operator()/trigger()/emit() all delegate here.  RecordableEvent
	 * overrides this to add recording and statistics before calling the
	 * base implementation.
	 *
	 * Acquires the mutex to snapshot the connection list, releases it,
	 * then invokes Direct connections synchronously and posts Deferred
	 * connections to their respective loops.
	 */
	virtual void triggerImpl( Args... args );

private:
	/**
	 * @brief Remove the wrapper for @p impl from the connections vector.
	 *
	 * Called by ConnectionImpl::disconnect().  If the event is being
	 * destroyed (_destroying == true) this is a no-op to avoid re-entering
	 * the destructor's cleanup path.
	 */
	void removeConnection( ConnectionBase* impl );

	/**
	 * @brief Internal connect implementation shared by all public overloads.
	 *
	 * Stores the connection type as-is (Auto defers resolution to trigger-time),
	 * validates Deferred prerequisites, creates ConnectionImpl and ConnectionWrapper,
	 * inserts into the sorted connection list, and registers the connection with
	 * both sender and receiver.
	 */
	template< typename HandlerFunc >
	Connection connectInternal(
		std::shared_ptr< Object > receiver,
		HandlerFunc&& handler,
		ConnectionType type,
		bool singleShot,
		int priority );
};

/**
 * @brief Alias for users that prefer signal/emit terminology.
 */
template< typename... Args >
using Signal = Event< Args... >;


//
// EVENT
//

template< typename... Args >
Event< Args...>::Event( Object* sender )
	: _senderObj( sender )
{
}

template< typename... Args >
Event< Args...>::~Event()
{
	_destroying.store( true, std::memory_order_release );
	disconnectAll();
}

template< typename... Args >
void Event< Args...>::operator()( Args... args )
{
	// if the sender has an associated EventLoop and the current call stack
	// is not already draining that loop, defer the entire emission to it,
	// which ensures that all handlers see a consistent sender-thread context
	// regardless of which thread called operator()
	//
	// if the sender has no loop, or if we are already inside that loop's
	// drain (t_drainingLoop == senderLoop), emit directly
	EventLoop* senderLoop = _senderObj ? _senderObj->eventLoop() : nullptr;
	if ( senderLoop && tls_drainingLoop != senderLoop )
	{
		// capture args by value into the deferred lambda - same as the
		// existing deferred-connection mechanism
		auto tupleArgs = std::make_shared< std::tuple< std::decay_t< Args >... > >( args... );
		senderLoop->postEvent(
			[ this, tupleArgs ]() mutable {
				std::apply(
					[ this ]( Args... a ) {
						triggerImpl( std::forward< Args >( a )... );
					},
					*tupleArgs );
			},
			_senderObj );
		return;
	}
	triggerImpl( std::forward< Args >( args )... );
}

template< typename... Args >
void Event< Args...>::trigger( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

template< typename... Args >
void Event< Args...>::emit( Args... args )
{
	operator()( std::forward< Args >( args )... );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connect( std::shared_ptr< Object > receiver, HandlerFunc&& handler, ConnectionType type )
{
	// connect with default priority (0)
	return connectInternal( receiver, std::forward< HandlerFunc >( handler ), type, false, 0 );
}

template< typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
Connection Event< Args...>::connect( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ), ConnectionType type )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	// capture raw pointer to avoid circular reference,
	// safe because ConnectionImpl::canInvoke() checks _receiver.expired() before calling the lambda
	ReceiverType* rawReceiver = receiver.get();
	auto handler = detail::makePartialHandler< Args... >( rawReceiver, method );

	return connect( receiver, std::move( handler ), type );
}

template< typename... Args >
template< typename Owner, typename... HandlerArgs >
Connection Event< Args... >::connect( ReceiverLifetimeAnchor< Owner >& anchor, void ( Owner::*method )( HandlerArgs... ), ConnectionType type )
{
	// anchor.owner() is a raw pointer into the enclosing owner object,
	// anchor.object() is the shared_ptr<Object> that serves as the lifetime
	// token - connections are severed when the anchor destructs and resets it
	Owner* rawOwner = anchor.owner();
	auto handler = detail::makePartialHandler< Args... >( rawOwner, method );

	return connect( anchor.object(), std::move( handler ), type );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connectFree( HandlerFunc&& handler, bool singleShot )
{
	platform::UniqueLock< platform::SharedMutex > lock( _mutex );

	auto senderPtr = _senderObj ? _senderObj->shared_from_this() : std::shared_ptr< Object >();

	auto connImpl = std::make_shared< ConnectionImpl< HandlerFunc > >(
		this,
		senderPtr,
		std::weak_ptr< Object >(),         // empty weak_ptr - no receiver lifetime tracking
		std::forward< HandlerFunc >( handler ),
		ConnectionType::Direct,
		nullptr,
		singleShot );

	// try to get function pointer (for free functions and non-capturing lambdas) for disconnectFree() matching
	void* funcPtr = nullptr;
	if constexpr( std::is_convertible_v< std::decay_t< HandlerFunc >, void(*)(Args...) > )
	{
		auto fp = static_cast< void(*)(Args...) >( handler );
		funcPtr = reinterpret_cast< void* >( fp );
	}

	auto wrapper = std::make_unique< ConnectionWrapper< HandlerFunc > >( connImpl, 0, funcPtr );
	_connections.push_back( std::move( wrapper ) );

	if ( _senderObj )
	{
		_senderObj->registerConnection( connImpl );
	}

	return Connection( connImpl );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connectOnce( std::shared_ptr< Object > receiver, HandlerFunc&& handler, ConnectionType type )
{
	// connect with default priority (0)
	return connectInternal( receiver, std::forward< HandlerFunc >( handler ), type, true, 0 );
}

template< typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
Connection Event< Args...>::connectOnce( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ), ConnectionType type )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	ReceiverType* rawReceiver = receiver.get();
	auto handler = detail::makePartialHandler< Args... >( rawReceiver, method );

	// connect with default priority (0)
	return connectInternal( receiver, std::move( handler ), type, true, 0 );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connectOnceFree( HandlerFunc&& handler )
{
	return connectFree( std::forward< HandlerFunc >( handler ), true );
}

template< typename... Args >
template< typename HandlerFunc, typename ConditionFunc >
Connection Event< Args...>::connectIf( std::shared_ptr< Object > receiver, HandlerFunc&& handler, ConditionFunc&& condition, ConnectionType type )
{
	// wrap the handler and condition together - condition is evaluated at invocation time
	auto conditionalHandler = [ handler = std::forward< HandlerFunc >( handler ), condition = std::forward< ConditionFunc >( condition ) ] ( Args... args ) mutable {
		if ( condition( args... ) )
		{
			handler( std::forward< Args >( args )... );
		}
	};

	return connectInternal( receiver, std::move( conditionalHandler ), type, false, 0 );
}

template< typename... Args >
template< typename ReceiverType, typename... HandlerArgs, typename ConditionFunc >
Connection Event< Args...>::connectIf( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ), ConditionFunc&& condition, ConnectionType type )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	ReceiverType* rawReceiver = receiver.get();
	auto partialMethod = detail::makePartialHandler< Args... >( rawReceiver, method );
	auto conditionalHandler = [ partialMethod, condition = std::forward< ConditionFunc >( condition ) ] ( Args... args ) mutable {
		if ( condition( args... ) )
		{
			partialMethod( std::forward< Args >( args )... );
		}
	};

	return connectInternal( receiver, std::move( conditionalHandler ), type, false, 0 );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connectWithPriority( std::shared_ptr< Object > receiver, HandlerFunc&& handler, int priority, ConnectionType type )
{
	return connectInternal( receiver, std::forward< HandlerFunc >( handler ), type, false, priority );
}

template< typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
Connection Event< Args...>::connectWithPriority( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ), int priority, ConnectionType type )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	ReceiverType* rawReceiver = receiver.get();
	auto handler = detail::makePartialHandler< Args... >( rawReceiver, method );

	return connectInternal( receiver, std::move( handler ), type, false, priority );
}

template< typename... Args >
void Event< Args...>::disconnectAll()
{
	std::vector< std::shared_ptr< ConnectionBase > > toDisconnect;

	// hold the lock while collecting, disconnect outside to avoid deadlock
	{
		platform::UniqueLock< platform::SharedMutex > lock( _mutex );

		for ( auto& conn : _connections )
		{
			if ( conn )
			{
				auto base = conn->getBase();
				if ( base )
				{
					toDisconnect.push_back( base );
				}
			}
		}

		_connections.clear();
	}

	for ( auto& conn : toDisconnect )
	{
		conn->disconnect();
	}
}

template< typename... Args >
void Event< Args...>::disconnect( const Object* receiver )
{
	std::vector< std::shared_ptr< ConnectionBase > > toDisconnect;

	// hold the lock while collecting, disconnect outside to avoid deadlock
	{
		platform::UniqueLock< platform::SharedMutex > lock( _mutex );

		for ( auto& conn : _connections )
		{
			if ( conn && conn->matchesReceiver( receiver ) )
			{
				auto base = conn->getBase();
				if ( base )
				{
					toDisconnect.push_back( base );
				}
			}
		}
	}

	for( auto& conn : toDisconnect )
	{
		conn->disconnect();
	}
}

template< typename... Args >
void Event< Args...>::disconnect( std::shared_ptr< Object > receiver )
{
	disconnect( receiver.get() );
}

template< typename... Args >
template< typename ReceiverType, typename... HandlerArgs >
void Event< Args...>::disconnect( std::shared_ptr< ReceiverType > receiver, void ( ReceiverType::*method )( HandlerArgs... ) )
{
	static_assert( std::is_base_of< Object, ReceiverType >::value, "Receiver must derive from Object" );

	std::vector< std::shared_ptr< ConnectionBase > > toDisconnect;

	// hold the lock while collecting, disconnect outside to avoid deadlock
	{
		platform::UniqueLock< platform::SharedMutex > lock( _mutex );

		// extract function pointer for comparison - type-punned through void**
		const void* funcPtr = reinterpret_cast< const void* >( *reinterpret_cast< const void** >( &method ) );

		for ( auto& conn : _connections )
		{
			if ( conn && conn->matchesReceiver( receiver.get() ) && conn->matchesHandler( funcPtr ) )
			{
				auto base = conn->getBase();
				if ( base )
				{
					toDisconnect.push_back( base );
				}
			}
		}
	}

	for( auto& conn : toDisconnect )
	{
		conn->disconnect();
	}
}

template< typename... Args >
template< typename... HandlerArgs >
void Event< Args...>::disconnectFree( void (*func)( HandlerArgs... ) )
{
	static_assert( std::is_same_v< std::tuple< Args... >, std::tuple< HandlerArgs... > >, "Function signature must match event signature" );

	std::vector< std::shared_ptr< ConnectionBase > > toDisconnect;

	// hold the lock while collecting, disconnect outside to avoid deadlock
	{
		platform::UniqueLock< platform::SharedMutex > lock( _mutex );

		const void* funcPtr = reinterpret_cast< const void* >( func );

		for ( auto& conn : _connections )
		{
			if ( conn && conn->matchesHandler( funcPtr ) )
			{
				auto base = conn->getBase();
				if ( base )
				{
					toDisconnect.push_back( base );
				}
			}
		}
	}

	for( auto& conn : toDisconnect )
	{
		conn->disconnect();
	}
}

template< typename... Args >
Connection Event< Args...>::forwardTo( Event< Args... >& other )
{
	// use the sender as the dummy receiver so the connection participates in
	// its lifetime tracking; fall back to a temporary Object if there's no sender
	auto dummyReceiver = _senderObj ? _senderObj->shared_from_this() : std::make_shared< Object >();

	return connectInternal(
		dummyReceiver,
		[ &other ]( Args... args ) {
			other.triggerImpl( std::forward< Args >( args )... );
		},
		ConnectionType::Direct,
		false,      // singleShot
		0 );        // priority
}

template< typename... Args >
void Event< Args...>::triggerImpl( Args... args )
{
	// keep a (wrapper*, impl_shared_ptr) pair for each connection so the impl
	// stays alive for the duration of the emission even if another thread
	// disconnects it mid-flight
	struct ConnectionHolder {
		ConnectionWrapperBase* wrapper;
		std::shared_ptr< ConnectionBase > impl;
	};

	std::vector< ConnectionHolder > directConnections;
	std::vector< ConnectionHolder > deferredConnections;

	{
		platform::SharedLock< platform::SharedMutex > lock( _mutex );

		for ( auto& conn : _connections )
		{
			auto base = conn->getBase();
			if ( ! base || ! base->isConnected() )
			{
				continue;
			}

			ConnectionHolder holder{ conn.get(), base };
			ConnectionType connType = conn->type();

			if ( connType == ConnectionType::Direct )
			{
				directConnections.push_back( holder );
			}
			else if ( connType == ConnectionType::Deferred )
			{
				deferredConnections.push_back( holder );
			}
			else // Auto - resolved dynamically inside invoke()
			{
				directConnections.push_back( holder );
			}
		}
	}  // mutex released - impls kept alive by shared_ptrs in holders

	// invoke direct connections synchronously (Auto connections also enter here
	// and perform their own Direct/Deferred decision inside invoke())
	for ( auto& holder : directConnections )
	{
		// re-check after lock release: another thread may have disconnected
		if ( holder.impl && holder.impl->isConnected() )
		{
			holder.wrapper->invokeDirect( std::forward< Args >( args )... );
		}
	}

	// pack args once and share across all explicit Deferred connections
	if ( ! deferredConnections.empty() )
	{
		auto sharedArgs = std::make_shared< std::tuple< std::decay_t< Args >... > >( args... );
		for ( auto& holder : deferredConnections )
		{
			if ( holder.impl && holder.impl->isConnected() )
			{
				holder.wrapper->invokeDeferred( sharedArgs );
			}
		}
	}
}

template< typename... Args >
void Event< Args...>::removeConnection( ConnectionBase* impl )
{
	// skip during destruction - disconnectAll() handles cleanup
	if ( _destroying.load( std::memory_order_acquire ) )
	{
		return;
	}

	platform::UniqueLock< platform::SharedMutex > lock( _mutex );
	_connections.erase(
		std::remove_if( _connections.begin(), _connections.end(),
			[ impl ]( const std::unique_ptr< ConnectionWrapperBase >& wrapper ) {
				if ( ! wrapper )
				{
					return true;
				}
				auto base = wrapper->getBase();
				return ! base || base.get() == impl;
			} ),
		_connections.end() );
}

template< typename... Args >
template< typename HandlerFunc >
Connection Event< Args...>::connectInternal( std::shared_ptr< Object > receiver, HandlerFunc&& handler, ConnectionType type, bool singleShot, int priority )
{
	platform::UniqueLock< platform::SharedMutex > lock( _mutex );

	// Auto connections store _type = Auto and resolve to Direct or Deferred
	// dynamically at each trigger, based on the sender's and receiver's
	// current event loops at that moment, which means they remain correct
	// after loop migration without any special handling
	//
	// explicit Direct and explicit Deferred connections are stored as-is:
	// - for explicit Direct, we log a warning if the loops are mismatched at
	// connect-time (the user may be intentionally crossing threads, but it
	// is a common source of bugs)
	// - for explicit Deferred, we log a warning if the receiver has no loop yet
	// (invocations will be silently dropped until a loop is assigned)

	EventLoop* receiverLoop = receiver ? receiver->eventLoop() : nullptr;

	EventLoop* loop = nullptr;  // target loop for explicit Deferred; nullptr for Direct and Auto

	if ( type == ConnectionType::Direct )
	{
#if KMAC_PULSAR_LOG_CONNECTION_ISSUES
		EventLoop* senderLoop = _senderObj ? _senderObj->eventLoop() : nullptr;
		if ( senderLoop != receiverLoop )
		{
			PULSAR_CONNECTION_LOG
				<< "Direct connection with mismatched event loops"
				<< " (sender loop: " << senderLoop
				<< ", receiver loop: " << receiverLoop << ")"
				<< " - handler will execute on the emitting thread regardless of receiver affinity\n";
		}
#endif
	}
	else if ( type == ConnectionType::Deferred )
	{
		loop = receiverLoop;
#if KMAC_PULSAR_LOG_CONNECTION_ISSUES
		if ( ! loop )
		{
			PULSAR_CONNECTION_LOG
				<< "Deferred connection made but receiver has no event loop"
				<< " - invocations will be silently dropped until a loop is assigned via setEventLoop()\n";
		}
#endif
	}
	// Auto: no validation or logging at connect-time; resolution is deferred to trigger-time

	auto senderPtr = _senderObj ? _senderObj->shared_from_this() : std::shared_ptr< Object >();

	auto connImpl = std::make_shared< ConnectionImpl< HandlerFunc > >(
		this, senderPtr, receiver, std::forward< HandlerFunc >( handler ), type, loop, singleShot );

	auto wrapper = std::make_unique< ConnectionWrapper< HandlerFunc > >( connImpl, priority );

	// insert in descending priority order using binary search
	auto insertPos = std::upper_bound(
		_connections.begin(),
		_connections.end(),
		priority,
		[]( int prio, const std::unique_ptr< ConnectionWrapperBase >& conn ) {
			return prio > conn->priority();
		} );

	_connections.insert( insertPos, std::move( wrapper ) );

	if ( _senderObj )
	{
		_senderObj->registerConnection( connImpl );
	}
	receiver->registerConnection( connImpl );

	return Connection( connImpl );
}


//
// CONNECTION IMPL
//

template< typename... Args >
template< typename HandlerFunc >
Event< Args... >::ConnectionImpl< HandlerFunc >::ConnectionImpl(
	Event* event,
	std::weak_ptr< Object > sender,
	std::weak_ptr< Object > receiver,
	HandlerFunc&& handler,
	ConnectionType type,
	EventLoop* loop,
	bool singleShot )
	: _event( event )
	, _sender( sender )
	, _receiver( receiver )
	, _handler( std::make_shared< std::decay_t< HandlerFunc > >( std::forward< HandlerFunc >( handler ) ) )
	, _type( type )
	, _loop( loop )
	, _singleShot( singleShot )
	, _migrating( false )
{
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionImpl< HandlerFunc >::isConnected() const
{
	return _connected;
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::disconnect()
{
	// exchange prevents double-disconnect from calling removeConnection twice
	if ( _connected.exchange( false ) )
	{
		if ( _event )
		{
			_event->removeConnection( this );
		}
	}
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionImpl< HandlerFunc >::isBlocked() const
{
	return _blocked.load( std::memory_order_acquire );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::block()
{
	_blocked.store( true, std::memory_order_release );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::unblock()
{
	_blocked.store( false, std::memory_order_release );
}

template< typename... Args >
template< typename HandlerFunc >
ConnectionType Event< Args... >::ConnectionImpl< HandlerFunc >::type() const
{
	return _type;
}

template< typename... Args >
template< typename HandlerFunc >
std::weak_ptr< Object > Event< Args... >::ConnectionImpl< HandlerFunc >::sender() const
{
	return _sender;
}

template< typename... Args >
template< typename HandlerFunc >
std::weak_ptr< Object > Event< Args... >::ConnectionImpl< HandlerFunc >::receiver() const
{
	return _receiver;
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionImpl< HandlerFunc >::isSingleShot() const
{
	return _singleShot;
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::beginMigration()
{
	// suspend Deferred dispatch until the new loop pointer is installed
	_migrating.store( true, std::memory_order_release );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::updateEventLoop( EventLoop* newLoop )
{
#if KMAC_PULSAR_LOG_CONNECTION_ISSUES
	if ( _type == ConnectionType::Direct )
	{
		// check whether the migration creates or resolves a loop mismatch
		EventLoop* senderLoop = nullptr;
		if ( auto sender = _sender.lock() )
		{
			senderLoop = sender->eventLoop();
		}

		bool wasMismatched = ( senderLoop != _loop );         // _loop still holds old receiver loop
		bool willBeMismatched = ( senderLoop != newLoop );

		if ( ! wasMismatched && willBeMismatched )
		{
			PULSAR_CONNECTION_LOG
				<< "Direct connection loop mismatch introduced by migration"
				<< " (sender loop: " << senderLoop
				<< ", new receiver loop: " << newLoop << ")"
				<< " - handler will execute on the emitting thread\n";
		}
		else if ( wasMismatched && ! willBeMismatched )
		{
			PULSAR_CONNECTION_LOG
				<< "Direct connection loop mismatch resolved by migration"
				<< " (sender and receiver now share loop: " << newLoop << ")\n";
		}
	}
	else if ( _type == ConnectionType::Deferred )
	{
		if ( _loop && ! newLoop )
		{
			PULSAR_CONNECTION_LOG
				<< "Deferred connection lost its event loop via migration"
				<< " - invocations will be silently dropped until a loop is restored\n";
		}
		else if ( ! _loop && newLoop )
		{
			PULSAR_CONNECTION_LOG
				<< "Deferred connection gained an event loop via migration"
				<< " (loop: " << newLoop << ") - invocations will resume\n";
		}
	}
	// Auto connections: no logging needed - they always re-evaluate at trigger-time
#endif

	_loop = newLoop;
	_migrating.store( false, std::memory_order_release );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::invoke( Args... args )
{
	if ( ! canInvoke() )
	{
		return;
	}

	if ( _type == ConnectionType::Direct )
	{
		invokeHandler( std::forward< Args >( args )... );
	}
	else if ( _type == ConnectionType::Deferred )
	{
		if ( _loop )
		{
			queueInvocation( std::forward< Args >( args )... );
		}
		else
		{
			// receiver has no loop - drop the invocation and warn
#if KMAC_PULSAR_LOG_CONNECTION_ISSUES
			PULSAR_CONNECTION_LOG
				<< "Deferred connection fired but receiver has no event loop"
				<< " - invocation dropped; assign a loop via setEventLoop()\n";
#endif
		}
	}
	else // Auto - resolve dynamically using current loop state
	{
		EventLoop* senderLoop = nullptr;
		if ( auto sender = _sender.lock() )
		{
			senderLoop = sender->eventLoop();
		}

		EventLoop* receiverLoop = nullptr;
		if ( auto receiver = _receiver.lock() )
		{
			receiverLoop = receiver->eventLoop();
		}

		if ( receiverLoop != nullptr && receiverLoop != senderLoop )
		{
			// receiver is on a different loop - queue to it
			queueInvocationToLoop( receiverLoop, std::forward< Args >( args )... );
		}
		else
		{
			// same loop, both null, or receiver has no loop - invoke directly
			invokeHandler( std::forward< Args >( args )... );
		}
	}
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::invokeDeferred( std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs )
{
	if ( ! canInvoke() )
	{
		return;
	}

	// invokeDeferred is only called for explicit Deferred connections from triggerImpl's
	// shared-tuple optimisation path; Auto connections go through invoke() instead
	if ( _type == ConnectionType::Deferred )
	{
		if ( _loop )
		{
			queueInvocationWithTuple( _loop, tupleArgs );
		}
		else
		{
			// receiver has no loop - drop the invocation and warn
#if KMAC_PULSAR_LOG_CONNECTION_ISSUES
			PULSAR_CONNECTION_LOG
				<< "Deferred connection fired but receiver has no event loop"
				<< " - invocation dropped; assign a loop via setEventLoop()\n";
#endif
		}
	}
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionImpl< HandlerFunc >::canInvoke() const
{
	if ( ! _connected || _blocked.load( std::memory_order_acquire ) )
	{
		return false;
	}

	if ( _migrating.load( std::memory_order_acquire ) )
	{
		return false;
	}

	// check receiver liveness only if _receiver was set
	// a default-constructed weak_ptr (used for free functions) compares equal to itself
	// in both owner_before directions, so the XOR of the two owner_before calls is false
	if ( _receiver.owner_before( std::weak_ptr< Object >{} ) || std::weak_ptr< Object >{}.owner_before( _receiver ) )
	{
		if ( _receiver.expired() )
		{
			const_cast< ConnectionImpl* >( this )->disconnect();
			return false;
		}
	}

	return true;
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::invokeHandler( Args... args )
{
	( *_handler )( std::forward< Args >( args )... );
	if ( _singleShot )
	{
		disconnect();
	}
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::queueInvocation( Args... args )
{
	queueInvocationToLoop( _loop, args... );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::queueInvocationToLoop( EventLoop* loop, Args... args )
{
	queueInvocationWithTuple( loop, std::make_shared< std::tuple< std::decay_t< Args >... > >( std::make_tuple( args... ) ) );
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionImpl< HandlerFunc >::queueInvocationWithTuple( EventLoop* loop, std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs )
{
	// share the handler and args tuple - no copy needed, supports move-only captures
	auto sharedHandler = _handler;
	bool singleShot = _singleShot;
	auto weakThis = std::weak_ptr< ConnectionImpl >( this->shared_from_this() );

	auto receiver = _receiver.lock();
	Object* receiverPtr = receiver ? receiver.get() : nullptr;

	loop->postEvent(
		[ sharedHandler, tupleArgs, singleShot, weakThis ]() mutable {
			std::apply( *sharedHandler, *tupleArgs );
			if ( singleShot )
			{
				if ( auto conn = weakThis.lock() )
				{
					conn->disconnect();
				}
			}
		},
		receiverPtr );
}


//
// CONNECTION WRAPPER
//

template< typename... Args >
template< typename HandlerFunc >
Event< Args... >::ConnectionWrapper< HandlerFunc >::ConnectionWrapper(
	std::shared_ptr< ConnectionImpl< HandlerFunc > > impl,
	int priority,
	const void* funcPtr )
	: _impl( impl )
	, _priority( priority )
	, _funcPtr( funcPtr )
{
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionWrapper< HandlerFunc >::invokeDirect( Args... args )
{
	if ( _impl )
	{
		_impl->invoke( std::forward< Args >( args )... );
	}
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionWrapper< HandlerFunc >::invokeDeferred( std::shared_ptr< std::tuple< std::decay_t< Args >... > > tupleArgs )
{
	if ( _impl )
	{
		_impl->invokeDeferred( tupleArgs );
	}
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionWrapper< HandlerFunc >::isConnected() const
{
	return _impl && _impl->isConnected();
}

template< typename... Args >
template< typename HandlerFunc >
void Event< Args... >::ConnectionWrapper< HandlerFunc >::disconnect()
{
	if ( _impl )
	{
		_impl->disconnect();
	}
}

template< typename... Args >
template< typename HandlerFunc >
ConnectionType Event< Args... >::ConnectionWrapper< HandlerFunc >::type() const
{
	return _impl ? _impl->type() : ConnectionType::Direct;
}

template< typename... Args >
template< typename HandlerFunc >
std::shared_ptr< ConnectionBase > Event< Args... >::ConnectionWrapper< HandlerFunc >::getBase()
{
	return _impl;
}

template< typename... Args >
template< typename HandlerFunc >
int Event< Args... >::ConnectionWrapper< HandlerFunc >::priority() const
{
	return _priority;
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionWrapper< HandlerFunc >::matchesReceiver( const Object* receiver ) const
{
	if ( _impl )
	{
		auto receiverPtr = _impl->receiver().lock();
		return receiverPtr && receiverPtr.get() == receiver;
	}
	return false;
}

template< typename... Args >
template< typename HandlerFunc >
bool Event< Args... >::ConnectionWrapper< HandlerFunc >::matchesHandler( const void* funcPtr ) const
{
	return _funcPtr && _funcPtr == funcPtr;
}

template< typename... Args >
template< typename HandlerFunc >
ConnectionInfo Event< Args... >::ConnectionWrapper< HandlerFunc >::getInfo() const
{
	ConnectionInfo info;
	info.connectionAddress = nullptr;
	info.priority = _priority;
	info.type = ConnectionType::Direct;
	info.isConnected = false;
	info.isBlocked = false;
	info.isSingleShot = false;
	info.senderAddress = nullptr;
	info.receiverAddress = nullptr;
	info.senderEventLoop = nullptr;
	info.receiverEventLoop = nullptr;
	info.senderTypeName = "Unknown";
	info.receiverTypeName = "Unknown";

	if ( _impl )
	{
		info.connectionAddress = _impl.get();
		info.type = _impl->type();
		info.isConnected = _impl->isConnected();
		info.isBlocked = _impl->isBlocked();
		info.isSingleShot = _impl->isSingleShot();

		if ( auto sender = _impl->sender().lock() )
		{
			info.senderAddress = sender.get();
			info.senderEventLoop = sender->eventLoop();
			info.senderTypeName = demangle( PULSAR_TYPE_NAME( *sender ) );
		}

		if ( auto receiver = _impl->receiver().lock() )
		{
			info.receiverAddress = receiver.get();
			info.receiverEventLoop = receiver->eventLoop();
			info.receiverTypeName = demangle( PULSAR_TYPE_NAME( *receiver ) );
		}
	}

	return info;
}

} // namespace pulsar
} // namespace kmac

#pragma pop_macro("emit")

#endif // KMAC_PULSAR_EVENT_H
