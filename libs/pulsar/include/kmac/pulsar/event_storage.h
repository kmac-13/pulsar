#pragma once
#ifndef KMAC_PULSAR_EVENT_STORAGE_H
#define KMAC_PULSAR_EVENT_STORAGE_H

/**
 * @file event_storage.h
 * @brief EventStorage<MutexType, Args...> - handler storage and connection
 * management for event types, without triggering.
 *
 * EventStorage owns the heap-allocated EventImpl, exposes the full
 * connect / disconnect / block API, and is the base class for both
 * BasicEvent (which adds triggering) and RecordableBasicEvent (which adds
 * recording + triggering).
 *
 * Users never name EventStorage directly; they work with Event<Args...>,
 * SharedEvent<Args...>, SingleThreadedEvent<Args...>, or their recordable
 * equivalents.
 *
 * The connection API is split into three named buckets so that each name
 * has a small, easily-navigated overload set:
 *
 *   connect
 *       - object/method connections: NTTP method pointer, runtime
 *         pointer-to-member-function, with or without an explicit
 *         Tracked<T> tracker; the NTTP forms also accept methods
 *         that take only a leading prefix of Args... (partial argument
 *         matching), dropping the trailing arguments automatically
 *
 *   connectFree
 *       - free function connections: NTTP function pointer or runtime
 *         function pointer, with or without an explicit Trackable tracker
 *         that bounds the connection's lifetime (a free function has no
 *         receiver of its own to track); the NTTP forms support partial
 *         argument matching the same way as the object/method bucket
 *
 *   connectLambda
 *       - capturing lambda / arbitrary functor connections, either as a
 *         pre-wrapped Callable or as a raw functor that gets wrapped
 *         internally; always takes an explicit Trackable& tracker
 *
 * disconnect() is split the same way: disconnect/disconnectFree by target,
 * plus the tracker- and disconnectAll()-based forms that apply across all
 * three buckets.  disconnect<Method>/disconnectFree<Func> reconstruct the
 * same partial-or-exact Callable that connect<Method>/connectFree<Func>
 * builds for a given Method/Func, so disconnecting a partial-arity
 * connection by name works.  The runtime forms connect(receiver, method) and
 * connectFree(func) are likewise disconnectable by target:
 * disconnect(receiver, method) recovers the stored PmfInvoker's identity via
 * Callable::targetAs, and disconnectFree(func) compares the reconstructed
 * function-pointer Callable.
 *
 * Every by-target disconnect (NTTP and runtime, method and free) removes a
 * single matching connection: if the same target was connected more than
 * once, one connection is removed and the rest remain.  The bulk forms,
 * disconnect(Trackable&) and disconnectAll(), remove every matching
 * connection.
 */

#include "platform.h"
#include "pulsar_fwd.h"

#include "callable.h"
#include "conn_params.h"
#include "connection.h"
#include "connection_type.h"
#include "event_detail.h"
#include "event_impl.h"
#include "event_impl_base.h"
#include "event_loop.h"
#include "gen_data.h"
#include "handler_entry.h"
#include "trackable.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace kmac {
namespace pulsar {

// ===========================================================================
// Tracked<T>  (defined here because BasicEvent connect() uses it)
// ===========================================================================

/**
 * @brief Bundles a receiver reference with an explicit Trackable for connect()
 * overloads where the receiver does not inherit from Trackable, or where a
 * different Trackable (e.g. an Anchor member) should own the connection
 * lifetime.
 *
 * @code
 * Anchor anchor;
 * event.connect< &MyClass::onData >( Tracked{ obj, anchor } );
 * @endcode
 */
template< typename T >
struct Tracked
{
	T& receiver;
	Trackable& tracker;
};

// C++17 deduction guide so Tracked{ obj, tracker } deduces T automatically.
template< typename T >
Tracked( T&, Trackable& ) -> Tracked< T >;


// ===========================================================================
// EventStorage
// ===========================================================================

template< typename MutexType, typename... Args >
class EventStorage
{
	// EventInspector reads _impl directly
	friend class EventInspector< MutexType, Args... >;

public:
	using HandlerType = Callable< void( Args... ) >;
	using PredicateType = Callable< bool( Args... ) >;
	using ConnParams = kmac::pulsar::ConnParams< MutexType, Args... >;

	/**
	 * @brief Builds a default-constructed ConnParams for this event's actual
	 * mutex and argument list, useful for method chaining.
	 *
	 * @note This is a convenience method designed to simplify connect call
	 * sites by eliminating the need to spell out ConnParams with  all
	 * argument types, using Event<...>::ConnParams (all argument types), or
	 * decltype(ev)::ConnParams to get an instance.  Callers, instead, can
	 * use  ev.params(...) at call sites, constructing an instance and
	 * chaining  methods from that, e.g. `ev.params().prio(5).once()`.
	 */
	static ConnParams params();

	/**
	 * @brief Builds a ConnParams with specified priority for this event's
	 * actual mutex and argument list, useful for method chaining.
	 *
	 * @note This is a convenience method designed to simplify connect call
	 * sites by eliminating the need to spell out ConnParams with  all
	 * argument types, using Event<...>::ConnParams (all argument types), or
	 * decltype(ev)::ConnParams to get an instance.  Callers, instead, can
	 * use  ev.params(...) at call sites, constructing an instance and
	 * chaining  methods from that, e.g. `ev.params(5).once()`.
	 */
	static ConnParams params( uint32_t priority );

	/**
	 * @brief Builds a ConnParams with conditional predicate/context for this
	 * event's actual mutex and argument list, useful for method chaining.
	 *
	 * @note This is a convenience method designed to simplify connect call
	 * sites by eliminating the need to spell out ConnParams with  all
	 * argument types, using Event<...>::ConnParams (all argument types), or
	 * decltype(ev)::ConnParams to get an instance.  Callers, instead, can
	 * use  ev.params(...) at call sites, constructing an instance and
	 * chaining  methods from that, e.g. `ev.params(ifPred).prio(5).once()`.
	 */
	template<
		typename Pred,
		typename = std::enable_if_t< std::is_invocable_r_v< bool, Pred, Args... > > >
	static ConnParams params( Pred pred, PredicateContext predicateContext = PredicateContext::Receiver );

protected:
	using HandlerEntry = kmac::pulsar::HandlerEntry< Args... >;
	using EventImpl = kmac::pulsar::EventImpl< MutexType, Args... >;

	/// heap-allocated implementation; shared with all Connection handles,
	/// protected so BasicEvent and RecordableBasicEvent can call
	/// _impl->dispatch() in their trigger implementations
	platform::SharedPtr< EventImpl > _impl { std::make_shared< EventImpl >() };

public:
	/**
	 * @brief Construct with no owner.
	 */
	EventStorage();

	/**
	 * @brief Construct with an owner Trackable.
	 *
	 * The owner's EventLoop is used as the sender loop when resolving Auto
	 * connections.  The event registers itself with the owner so that
	 * owner->setEventLoop() automatically re-resolves all Auto connections.
	 *
	 * The owner must outlive this event.  Typically the event is declared
	 * as a member of the owner class:
	 * @code
	 * class MyClass : public Trackable {
	 *     Event< int > onData { this };
	 * };
	 * @endcode
	 */
	explicit EventStorage( Trackable* owner );

	~EventStorage() = default;

	EventStorage( const EventStorage& ) = delete;
	EventStorage& operator=( const EventStorage& ) = delete;
	EventStorage( EventStorage&& ) = default;
	EventStorage& operator=( EventStorage&& ) = default;

	/**
	 * @brief Returns the Trackable this event was constructed with as its
	 * owner (nullptr if none).  Used by forwardTo() to auto-source the
	 * forwarding connection's tracker from a target event without the
	 * caller having to pass it explicitly.
	 */
	Trackable* owner() const;

	// =========================================================================
	// object / method connections, including NTTP (non-type template parameter)
	// =========================================================================

	/**
	 * @brief Connect a member function via NTTP, with a receiver that inherits
	 * from Trackable and optional connection type and connection parameters:
	 * connect<&ReceiverType::method>(receiver, connType, connParams);
	 */
	template< auto Method, typename T >
	Connection connect(
		T& receiver,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a member function via NTTP, with a receiver that inherits
	 * from Trackable, connection parameters, and optional connection type:
	 * connect<&ReceiverType::method>(receiver, connParams, connType);
	 */
	template< auto Method, typename T >
	Connection connect(
		T& receiver,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function via NTTP, with a non-Trackable
	 * receiver-Trackable pair and optional connection type and connection
	 * parameters:
	 * connect<&ReceiverType::method>({receiver, tracker}, connType, connParams);
	 */
	template< auto Method, typename T = detail::ExtractReceiver_t< decltype( Method ) > >
	Connection connect(
		detail::NonDeduced_t< Tracked< T > > tracked,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a member function via NTTP, with a non-Trackable
	 * receiver-Trackable pair, connection parameters, and optional connection
	 * type:
	 * connect<&ReceiverType::method>({receiver, tracker}, connParams, connType);
	 */
	template< auto Method, typename T = detail::ExtractReceiver_t< decltype( Method ) > >
	Connection connect(
		detail::NonDeduced_t< Tracked< T > > tracked,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function via runtime argument, with a receiver
	 * that inherits from Trackable and optional connection type and connection
	 * parameters:
	 * connect(receiver, &ReceiverType::method, connType, connParams);
	 *
	 * @note This form is disconnectable by target via
	 * disconnect(receiver, method); retaining the returned Connection is
	 * optional.
	 */
	template< typename T, typename... MethodArgs >
	Connection connect(
		T& receiver,
		void ( T::*method )( MethodArgs... ),
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a member function via runtime argument, with a receiver
	 * that inherits from Trackable, connection parameters, and optional
	 * connection type:
	 * connect(receiver, &ReceiverType::method, connParams, connType);
	 *
	 * @note This form is disconnectable by target via
	 * disconnect(receiver, method); retaining the returned Connection is
	 * optional.
	 */
	template< typename T, typename... MethodArgs >
	Connection connect(
		T& receiver,
		void ( T::*method )( MethodArgs... ),
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a member function via runtime argument, with a non-Trackable
	 * receiver-Trackable pair and optional connection type and connection
	 * parameters:
	 * connect({receiver, tracker}, &ReceiverType::method, connType, connParams);
	 *
	 * @note This form is disconnectable by target via
	 * disconnect(receiver, method); retaining the returned Connection is
	 * optional.
	 */
	template< typename T, typename... MethodArgs >
	Connection connect(
		detail::NonDeduced_t< Tracked< T > > tracked,
		void ( T::*method )( MethodArgs... ),
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a member function via runtime argument, with a non-Trackable
	 * receiver-Trackable pair, connection parameters, and optional connection
	 * type:
	 * connect({receiver, tracker}, &ReceiverType::method, connParams, connType);
	 *
	 * @note This form is disconnectable by target via
	 * disconnect(receiver, method); retaining the returned Connection is
	 * optional.
	 */
	template< typename T, typename... MethodArgs >
	Connection connect(
		detail::NonDeduced_t< Tracked< T > > tracked,
		void ( T::*method )( MethodArgs... ),
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	// =========================================================================
	// free-function connections, including NTTP (non-type template parameter)
	// =========================================================================

	/**
	 * @brief Connect a free function via NTTP, with optional connection type
	 * and connection parameters:
	 * connect<&func>(connType, connParams);
	 */
	template< auto Func >
	Connection connectFree(
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a free function via NTTP, with connection parameters
	 * and optional connection type:
	 * connect<&func>(connParams, connType);
	 */
	template< auto Func >
	Connection connectFree(
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a free function via runtime argument, with optional
	 * connection type and connection parameters:
	 * connect(&func, connType, connParams);
	 */
	Connection connectFree(
		void (*func)( Args... ),
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a free function via runtime argument, with connection
	 * parameters and optional connection type:
	 * connect(&func, connParams, connType);
	 */
	Connection connectFree(
		void (*func)( Args... ),
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a free function via NTTP, tracked by an explicit
	 * Trackable whose lifetime bounds the connection, with optional connection
	 * type and connection parameters:
	 * connect<&func>(tracker, connType, connParams);
	 *
	 * A free function has no receiver of its own, so the tracker is a separate
	 * object; when it is destroyed the connection is automatically removed.
	 * disconnectFree<Func>() disconnects this form by target as well.
	 */
	template< auto Func >
	Connection connectFree(
		Trackable& tracker,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a free function via NTTP, tracked by an explicit
	 * Trackable, with connection parameters and optional connection type:
	 * connect<&func>(tracker, connParams, connType);
	 */
	template< auto Func >
	Connection connectFree(
		Trackable& tracker,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a free function via runtime argument, tracked by an
	 * explicit Trackable whose lifetime bounds the connection, with optional
	 * connection type and connection parameters:
	 * connect(tracker, &func, connType, connParams);
	 *
	 * A free function has no receiver of its own, so the tracker is a separate
	 * object; when it is destroyed the connection is automatically removed.
	 * disconnectFree(&func) disconnects this form by target as well.
	 */
	Connection connectFree(
		Trackable& tracker,
		void (*func)( Args... ),
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a free function via runtime argument, tracked by an
	 * explicit Trackable, with connection parameters and optional connection
	 * type:
	 * connect(tracker, &func, connParams, connType);
	 */
	Connection connectFree(
		Trackable& tracker,
		void (*func)( Args... ),
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	// =========================================================================
	// lambda / functor connections
	// =========================================================================

	/**
	 * @brief Connect a pre-wrapped Callable, with an explicit Trackable tracker
	 * and optional connection type and connection parameters:
	 * connect(tracker, ...
	 */
	Connection connectLambda(
		Trackable& tracker,
		HandlerType&& handler,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a pre-wrapped Callable, with an explicit Trackable tracker,
	 * connection type, and optional connection parameters:
	 * connect(tracker, ...
	 */
	Connection connectLambda(
		Trackable& tracker,
		HandlerType&& handler,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a functor (typically a capturing lambda) directly,
	 * without the caller needing to wrap it in Callable::create() first.
	 *
	 * @code
	 * event.connectLambda( tracker, [ this ]( int x ) { onValue( x ); } );
	 * @endcode
	 */
	template< typename F >
	Connection connectLambda(
		Trackable& tracker,
		F&& functor,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a pre-wrapped Callable, with an explicit Trackable
	 * tracker, connection parameters, and optional connection type - same as
	 * the type-first overload above with the last two arguments swapped:
	 * connect(tracker, handler, connParams, connType);
	 */
	template< typename F >
	Connection connectLambda(
		Trackable& tracker,
		F&& functor,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a pre-wrapped Callable with no tracker.
	 *
	 * No automatic lifetime tracking: nothing disconnects this on the
	 * caller's behalf, and a Deferred or resolved-Deferred connection can
	 * invoke the handler on another thread at an arbitrary later time.  The
	 * caller is fully responsible for managing the returned Connection's
	 * lifetime (manual disconnect, ScopedConnection, ConnectionGroup, or
	 * ensuring any captured references outlive the connection).  Prefer the
	 * tracker overload above when the lambda captures by reference and a
	 * suitable Trackable is available; use this overload when you have
	 * deliberately chosen to manage lifetime yourself, the same trust model
	 * already used by the free-function connectFree() overloads.
	 */
	Connection connectLambda(
		HandlerType&& handler,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a pre-wrapped Callable with no tracker, connection
	 * parameters, and optional connection type - same lifetime tradeoff as
	 * the type-first overload above, with the last two arguments swapped:
	 * connect(handler, connParams, connType);
	 */
	Connection connectLambda(
		HandlerType&& handler,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	/**
	 * @brief Connect a functor with no tracker.  See the Callable overload
	 * above for the lifetime tradeoff this implies.
	 */
	template< typename F >
	Connection connectLambda(
		F&& functor,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Connect a functor with no tracker, connection parameters, and
	 * optional connection type - same lifetime tradeoff as the Callable
	 * overload above, with the last two arguments swapped:
	 * connect(functor, connParams, connType);
	 */
	template< typename F >
	Connection connectLambda(
		F&& functor,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	// =========================================================================
	// event chaining
	// =========================================================================

	/**
	 * @brief Connects this event so that triggering it also triggers
	 * `target`, forwarding the leading arguments `target` accepts and
	 * dropping the rest.
	 *
	 * The forwarding connection's tracker is auto-sourced from `target`'s
	 * own owner Trackable (target.owner()), so its lifetime is bound to
	 * `target` without the caller needing to pass a tracker explicitly - if
	 * `target` has no owner, the forwarding connection is untracked, the
	 * same trust model as connectFree() with no tracker.  `type` applies to
	 * this forwarding connection on the source event; `target`'s own
	 * trigger then follows `target`'s own thread affinity.
	 *
	 * `target`'s parameter list must be the first-N prefix of this event's
	 * Args... - createPartial (routed through via target's own operator())
	 * enforces this at compile time the same way it does for the regular
	 * connect<Method> path.  Use a lambda connection if a different subset
	 * or different order of arguments is needed.
	 *
	 * @code
	 * xChanged.forwardTo( moved );  // xChanged(int) -> moved()
	 * @endcode
	 */
	template< typename TargetEvent >
	Connection forwardTo(
		TargetEvent& targetEvent,
		ConnectionType type = ConnectionType::Auto,
		ConnParams params = {} );

	/**
	 * @brief Same as the type-first forwardTo() above, with the last two
	 * arguments swapped: xChanged.forwardTo( moved, connParams, connType );
	 */
	template< typename TargetEvent >
	Connection forwardTo(
		TargetEvent& targetEvent,
		ConnParams params,
		ConnectionType type = ConnectionType::Auto );

	// =========================================================================
	// blocking
	// =========================================================================

	/**
	 * @brief Suppresses triggers on this event: while blocked, trigger()
	 * silently returns without dispatching to any handler.  Reentrant -
	 * tracks a depth counter, so nested block()/unblock() pairs compose
	 * safely.  Affects every handler equally; see Connection::block() to
	 * suppress just one.
	 */
	void block();

	/**
	 * @brief Reverses one block() call.  The event resumes dispatching once
	 * every outstanding block() has been matched by an unblock().  A
	 * surplus unblock() (no corresponding block() outstanding) is a no-op.
	 */
	void unblock();

	/**
	 * @brief RAII alternative to block()/unblock() - blocks immediately and
	 * unblocks when the returned guard goes out of scope, including via an
	 * exception.  [[nodiscard]] because a discarded guard unblocks on the
	 * same line, defeating the point of calling this instead of block().
	 */
	[[ nodiscard ]] BlockGuard blockGuard();

	// =========================================================================
	// disconnection
	// =========================================================================

	/**
	 * @brief Disconnects every connection anchored to `tracker`.  This is a
	 * bulk, lifetime-scoped operation: all of the tracker's connections to
	 * this event are removed.
	 */
	void disconnect( Trackable& tracker );

	/**
	 * @brief Removes a single connection matching NTTP method `Method` on
	 * `receiver`.  If the same method is connected on the same receiver more
	 * than once, one connection is removed and the rest remain, so an
	 * accidental duplicate is not silently masked and an intentional one is
	 * not fully torn down.
	 */
	template< auto Method, typename T >
	void disconnect( T& receiver );

	/**
	 * @brief Removes a single connection matching the runtime method pointer
	 * `method` on `receiver`, e.g. disconnect(receiver, &ReceiverType::method).
	 *
	 * This is the runtime (non-NTTP) counterpart to disconnect<Method>(),
	 * covering connections made via connect(receiver, method).  It matches by
	 * receiver pointer and native pointer-to-member equality on `method`, and
	 * removes a single connection as the NTTP form does.
	 */
	template< typename T, typename... MethodArgs >
	void disconnect( T& receiver, void ( T::*method )( MethodArgs... ) );

	/**
	 * @brief Removes a single connection matching NTTP free function `Func`,
	 * removing one connection if several match.
	 */
	template< auto Func >
	void disconnectFree();

	/**
	 * @brief Removes a single connection matching the runtime free-function
	 * pointer `func`, removing one connection if several match.
	 */
	void disconnectFree( void (*func)( Args... ) );

	/**
	 * @brief Disconnects every connection on this event, regardless of
	 * tracker or target.  Equivalent to calling disconnect() for every live
	 * Connection, but done in a single lock acquisition (see
	 * EventImpl::preLockDisconnectAll()) rather than one per connection.
	 */
	void disconnectAll();

private:
	/**
	 * @brief Shared endpoint every connect()/connectFree()/connectLambda()/
	 * forwardTo() overload funnels through: resolves the connection type
	 * against current loop topology, places the handler via
	 * EventImpl::addConnection(), registers with `tracker` if non-null, and
	 * returns the resulting Connection.
	 *
	 * @param handler the callable to invoke on dispatch
	 * @param tracker receiver-side Trackable for auto-disconnect and Auto
	 *   resolution's receiver loop, or nullptr for an untracked connection
	 *   (see the connectFree()/ connectLambda() no-tracker overloads)
	 * @param type the declared ConnectionType (Direct/Deferred/Auto)
	 * @param hasOwner true if `handler` is bound to a receiver/owner object
	 *   rather than a free function or lambda (see EventFlags::hasOwner())
	 * @param priority user-defined dispatch ordering; 0 is default/unordered
	 * @param hasPredicate true if `predicate` is non-empty and should be attached
	 * @param predicate the conditional predicate, moved in; ignored if hasPredicate is false
	 * @param predicateContext which context the predicate evaluates in (Sender vs Receiver)
	 * @param isSingleShot true if the connection should auto-disconnect after firing once
	 */
	Connection connectImpl(
		HandlerType&& handler,
		Trackable* tracker,
		ConnectionType type,
		bool hasOwner,
		uint32_t priority,
		bool hasPredicate = false,
		Callable< bool( Args... )> predicate = {},
		PredicateContext predicateContext = PredicateContext::Receiver,
		bool isSingleShot = false );
};


// ===========================================================================
// params()
// ===========================================================================

template< typename MutexType, typename... Args >
inline typename EventStorage< MutexType, Args... >::ConnParams
EventStorage< MutexType, Args... >::params()
{
	return ConnParams();
}

template< typename MutexType, typename... Args >
inline typename EventStorage< MutexType, Args... >::ConnParams
EventStorage< MutexType, Args... >::params( uint32_t priority )
{
	return ConnParams( priority );
}

template< typename MutexType, typename... Args >
template< typename Pred, typename >
inline typename EventStorage< MutexType, Args... >::ConnParams
EventStorage< MutexType, Args... >::params( Pred pred, PredicateContext predicateContext )
{
	return ConnParams( std::move( pred ), predicateContext );
}


// ===========================================================================
// Constructors
// ===========================================================================

template< typename MutexType, typename... Args >
inline EventStorage< MutexType, Args... >::EventStorage()
{
	_impl->weakSelf = platform::WeakPtr< EventImplBase >( _impl );
}

template< typename MutexType, typename... Args >
inline EventStorage< MutexType, Args... >::EventStorage( Trackable* owner )
{
	_impl->weakSelf = platform::WeakPtr< EventImplBase >( _impl );
	_impl->owner = owner;
	if ( owner )
	{
		owner->registerOwnedEvent( platform::WeakPtr< EventImplBase >( _impl ) );
	}
}

template< typename MutexType, typename... Args >
inline Trackable* EventStorage< MutexType, Args... >::owner() const
{
	return _impl->owner;
}


// ===========================================================================
// object / method connections
// ===========================================================================

template< typename MutexType, typename... Args >
template< auto Method, typename T >
inline Connection EventStorage< MutexType, Args... >::connect(
	T& receiver, ConnectionType type, ConnParams params )
{
	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	HandlerType handler;
	if constexpr ( detail::MethodArity_v< decltype( Method ) > == sizeof...( Args ) )
	{
		handler = HandlerType::template create< Method >( &receiver );
	}
	else
	{
		handler = HandlerType::template createPartial< Method >( &receiver );
	}
	return connectImpl(
		std::move( handler ), tracker, type, true, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< auto Method, typename T >
inline Connection EventStorage< MutexType, Args... >::connect(
	T& receiver, ConnParams params, ConnectionType type )
{
	return connect< Method >( receiver, type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< auto Method, typename T >
inline Connection EventStorage< MutexType, Args... >::connect(
	detail::NonDeduced_t< Tracked< T > > tracked,
	ConnectionType type, ConnParams params )
{
	HandlerType handler;
	if constexpr ( detail::MethodArity_v< decltype( Method ) > == sizeof...( Args ) )
	{
		handler = HandlerType::template create< Method >( &tracked.receiver );
	}
	else
	{
		handler = HandlerType::template createPartial< Method >( &tracked.receiver );
	}
	return connectImpl(
		std::move( handler ), &tracked.tracker, type, true, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< auto Method, typename T >
inline Connection EventStorage< MutexType, Args... >::connect(
	detail::NonDeduced_t< Tracked< T > > tracked,
	ConnParams params, ConnectionType type )
{
	return connect< Method, T >( tracked, type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< typename T, typename... MethodArgs >
inline Connection EventStorage< MutexType, Args... >::connect(
	T& receiver, void ( T::*method )( MethodArgs... ),
	ConnectionType type, ConnParams params )
{
	static_assert( sizeof...( MethodArgs ) <= sizeof...( Args ),
		"connect: method requests more arguments than this event provides" );

	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	return connectImpl(
		HandlerType::create(
			detail::PmfInvoker< void ( Args... ), T, MethodArgs... >{ &receiver, method } ),
		tracker, type, true, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< typename T, typename... MethodArgs >
inline Connection EventStorage< MutexType, Args... >::connect(
	T& receiver, void ( T::*method )( MethodArgs... ),
	ConnParams params, ConnectionType type )
{
	return connect( receiver, method, type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< typename T, typename... MethodArgs >
inline Connection EventStorage< MutexType, Args... >::connect(
	detail::NonDeduced_t< Tracked< T > > tracked,
	void ( T::*method )( MethodArgs... ),
	ConnectionType type, ConnParams params )
{
	static_assert( sizeof...( MethodArgs ) <= sizeof...( Args ),
		"connect: method requests more arguments than this event provides" );

	return connectImpl(
		HandlerType::create(
			detail::PmfInvoker< void ( Args... ), T, MethodArgs... >{ &tracked.receiver, method } ),
		&tracked.tracker, type, true, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< typename T, typename... MethodArgs >
inline Connection EventStorage< MutexType, Args... >::connect(
	detail::NonDeduced_t< Tracked< T > > tracked,
	void ( T::*method )( MethodArgs... ),
	ConnParams params, ConnectionType type )
{
	return connect( tracked, method, type, std::move( params ) );
}

// ===========================================================================
// free-function connections
// ===========================================================================

template< typename MutexType, typename... Args >
template< auto Func >
inline Connection EventStorage< MutexType, Args... >::connectFree( ConnectionType type, ConnParams params )
{
	HandlerType handler;
	if constexpr ( detail::FunctionArity_v< decltype( Func ) > == sizeof...( Args ) )
	{
		handler = HandlerType::template create< Func >();
	}
	else
	{
		handler = HandlerType::template createPartial< Func >();
	}
	return connectImpl(
		std::move( handler ), nullptr, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< auto Func >
inline Connection EventStorage< MutexType, Args... >::connectFree( ConnParams params, ConnectionType type )
{
	return connectFree< Func >( type, std::move( params ) );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	void (*func)( Args... ), ConnectionType type, ConnParams params )
{
	return connectImpl(
		HandlerType::create( func ), nullptr, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	void (*func)( Args... ), ConnParams params, ConnectionType type )
{
	return connectFree( func, type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< auto Func >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	Trackable& tracker, ConnectionType type, ConnParams params )
{
	HandlerType handler;
	if constexpr ( detail::FunctionArity_v< decltype( Func ) > == sizeof...( Args ) )
	{
		handler = HandlerType::template create< Func >();
	}
	else
	{
		handler = HandlerType::template createPartial< Func >();
	}
	return connectImpl(
		std::move( handler ), &tracker, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< auto Func >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	Trackable& tracker, ConnParams params, ConnectionType type )
{
	return connectFree< Func >( tracker, type, std::move( params ) );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	Trackable& tracker, void (*func)( Args... ), ConnectionType type, ConnParams params )
{
	return connectImpl(
		HandlerType::create( func ), &tracker, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectFree(
	Trackable& tracker, void (*func)( Args... ), ConnParams params, ConnectionType type )
{
	return connectFree( tracker, func, type, std::move( params ) );
}

// ===========================================================================
// lambda / functor connections
// ===========================================================================

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	Trackable& tracker, HandlerType&& handler, ConnectionType type, ConnParams params )
{
	return connectImpl(
		std::move( handler ), &tracker, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	Trackable& tracker, HandlerType&& handler, ConnParams params, ConnectionType type )
{
	return connectLambda( tracker, std::move( handler ), type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< typename F >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	Trackable& tracker, F&& functor, ConnectionType type, ConnParams params )
{
	return connectImpl(
		HandlerType::create( std::forward< F >( functor ) ), &tracker, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< typename F >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	Trackable& tracker, F&& functor, ConnParams params, ConnectionType type )
{
	return connectLambda( tracker, std::forward< F >( functor ), type, std::move( params ) );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	HandlerType&& handler, ConnectionType type, ConnParams params )
{
	return connectImpl(
		std::move( handler ), nullptr, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	HandlerType&& handler, ConnParams params, ConnectionType type )
{
	return connectLambda( std::move( handler ), type, std::move( params ) );
}

template< typename MutexType, typename... Args >
template< typename F >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	F&& functor, ConnectionType type, ConnParams params )
{
	return connectImpl(
		HandlerType::create( std::forward< F >( functor ) ), nullptr, type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< typename F >
inline Connection EventStorage< MutexType, Args... >::connectLambda(
	F&& functor, ConnParams params, ConnectionType type )
{
	return connectLambda( std::forward< F >( functor ), type, std::move( params ) );
}


// ===========================================================================
// event chaining
// ===========================================================================

template< typename MutexType, typename... Args >
template< typename TargetEvent >
inline Connection EventStorage< MutexType, Args... >::forwardTo(
	TargetEvent& targetEvent,
	ConnectionType type,
	ConnParams params )
{
	// route through the same partial-arg machinery the regular connect<Method>
	// path uses, targeting TargetEvent's own operator - its parameter list
	// is required (by createPartial, positionally) to be the first-N prefix
	// of this event's Args..., which is exactly the arg-adaptation rule
	// forwardTo is documented to enforce
	return connectImpl(
		HandlerType::template createPartial< &TargetEvent::operator() >( &targetEvent ),
		targetEvent.owner(), type, false, params.prio(),
		params.hasPredicate(), params.predicate(), params.predicateContext(), params.isSingleShot() );
}

template< typename MutexType, typename... Args >
template< typename TargetEvent >
inline Connection EventStorage< MutexType, Args... >::forwardTo(
	TargetEvent& targetEvent,
	ConnParams params,
	ConnectionType type )
{
	return forwardTo( targetEvent, type, params );
}

// ===========================================================================
// blocking
// ===========================================================================

template< typename MutexType, typename... Args >
inline void EventStorage< MutexType, Args... >::block()
{
	_impl->block();
}

template< typename MutexType, typename... Args >
inline void EventStorage< MutexType, Args... >::unblock()
{
	_impl->unblock();
}

template< typename MutexType, typename... Args >
inline BlockGuard EventStorage< MutexType, Args... >::blockGuard()
{
	_impl->block();
	return BlockGuard(
		_impl,  // implicit upcast to SharedPtr<EventImplBase>, keeps the event alive
		Callable< void() >::create< &EventImpl::unblock >( _impl.get() ) );
}


// ===========================================================================
// disconnection
// ===========================================================================

template< typename MutexType, typename... Args >
inline void EventStorage< MutexType, Args... >::disconnect( Trackable& tracker )
{
	auto connections = tracker.extractConnectionsTo( _impl.get() );
	_impl->disconnectHandlers( connections );
}

template< typename MutexType, typename... Args >
template< auto Method, typename T >
inline void EventStorage< MutexType, Args... >::disconnect( T& receiver )
{
	HandlerType target;
	if constexpr ( detail::MethodArity_v< decltype( Method ) > == sizeof...( Args ) )
	{
		target = HandlerType::template create< Method >( &receiver );
	}
	else
	{
		target = HandlerType::template createPartial< Method >( &receiver );
	}

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< MutexType > lock( _impl->mutex );
		for ( uint32_t i = 0;
			i < static_cast< uint32_t >( _impl->handlers.size() );
			++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, _impl->handlers[ i ].generation );
				break;  // remove one connection; leave any duplicates in place
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename MutexType, typename... Args >
template< auto Func >
inline void EventStorage< MutexType, Args... >::disconnectFree()
{
	HandlerType target;
	if constexpr ( detail::FunctionArity_v< decltype( Func ) > == sizeof...( Args ) )
	{
		target = HandlerType::template create< Func >();
	}
	else
	{
		target = HandlerType::template createPartial< Func >();
	}

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< MutexType > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, _impl->handlers[ i ].generation );
				break;  // remove one connection; leave any duplicates in place
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename MutexType, typename... Args >
inline void EventStorage< MutexType, Args... >::disconnectFree(
	void (*func)( Args... ) )
{
	const HandlerType target = HandlerType::create( func );

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< MutexType > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, _impl->handlers[ i ].generation );
				break;  // remove one connection; leave any duplicates in place
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename MutexType, typename... Args >
template< typename T, typename... MethodArgs >
inline void EventStorage< MutexType, Args... >::disconnect( T& receiver, void ( T::*method )( MethodArgs... ) )
{
	static_assert( sizeof...( MethodArgs ) <= sizeof...( Args ),
		"disconnect: method requests more arguments than this event provides" );

	// connect(receiver, method) stores a PmfInvoker; recover it by exact type
	// and compare the receiver pointer and method directly - matching by the
	// functor's fields means no comparison Callable is constructed, so this
	// path allocates nothing
	using Invoker = detail::PmfInvoker< void ( Args... ), T, MethodArgs... >;

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< MutexType > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( ! entry.flags.isActive() )
			{
				continue;
			}

			const Invoker* candidate = entry.handler.template targetAs< Invoker >();
			if ( candidate
				&& candidate->_receiver == &receiver
				&& candidate->_method == method )
			{
				toDisconnect.emplace_back( i, _impl->handlers[ i ].generation );
				break;  // remove one connection; leave any duplicates in place
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename MutexType, typename... Args >
inline void EventStorage< MutexType, Args... >::disconnectAll()
{
	platform::LockGuard< MutexType > lock( _impl->mutex );
	_impl->preLockDisconnectAll();
}


// ===========================================================================
// connectImpl
// ===========================================================================

template< typename MutexType, typename... Args >
inline Connection EventStorage< MutexType, Args... >::connectImpl(
	HandlerType&& handler,
	Trackable* tracker,
	ConnectionType type,
	bool hasOwner,
	uint32_t priority,
	bool hasPredicate,
	Callable< bool( Args... )> predicate,
	PredicateContext predicateContext,
	bool isSingleShot )
{
	uint32_t index;
	uint16_t gen;
	uint64_t tag;
	{
		platform::LockGuard< MutexType > lock( _impl->mutex );

		EventLoop* receiverLoop = tracker ? tracker->eventLoop() : nullptr;
		EventLoop* senderLoop = _impl->owner ? _impl->owner->eventLoop() : nullptr;
		ResolvedConnectionType resolved = detail::resolveConnectionType( type, senderLoop, receiverLoop );

		tag = tracker ? tracker->migrationTag() : 0;

		index = _impl->addConnection(
			std::move( handler ), tag, static_cast< uint16_t >( priority ),
			type, resolved, receiverLoop, hasOwner, isSingleShot,
			hasPredicate, std::move( predicate ),
			predicateContext == PredicateContext::Receiver, gen );
	}

	platform::WeakPtr< EventImplBase > weak( _impl );
	if ( tracker )
	{
		tracker->trackConnection( weak, index, gen );
	}

	return Connection( std::move( weak ), index, gen );
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_STORAGE_H
