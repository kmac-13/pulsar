#pragma once
#ifndef KMAC_PULSAR_CONN_PARAMS_H
#define KMAC_PULSAR_CONN_PARAMS_H

/**
 * @file conn_params.h
 * @brief ConnParams<MutexType, Args...> - optional per-connection settings
 * bundle for connect()-family calls.
 *
 * Bundles priority, a predicate (with its PredicateContext), and
 * single-shot into one value, settable in any combination without a
 * separate overload or function name for each one.  Supplying a predicate
 * is what makes a connection conditional; calling once() is what makes it
 * single-shot.
 *
 * Carries MutexType alongside Args... specifically so once() can
 * static_assert against SharedEvent (SharedMutex) at compile time - a
 * single-shot connection reentrantly disconnecting itself while a
 * SharedLock is held would deadlock.  once() is a lazily-instantiated
 * member function, so this check only fires when once() is actually
 * called for a given MutexType, not merely when a ConnParams of that
 * MutexType is instantiated.
 *
 * Not meant to be stored: connect() reads whatever was set and applies it
 * directly to the new connection; the ConnParams instance itself is
 * discarded once that call returns.
 *
 * @code
 * event.connect( receiver, method );                                  // defaults
 * event.connect( receiver, method, ConnectionType::Direct );          // type only
 * event.connect( receiver, method, ConnectionType::Auto, { 5 } );     // priority only
 * event.connect( receiver, method, ConnectionType::Auto, 5 );         // implicit, priority only
 * event.connect( receiver, method, { 5 } );                           // same - ConnParams-first overload
 * event.connect( receiver, method, { pred } );                        // conditional, Receiver context
 * event.connect( receiver, method, { pred, PredicateContext::Sender } );
 *
 * Predicates always require exact-arity matching against the event's
 * Args... - unlike handlers (connect<&Method>/connectFree<&Func>), there is
 * no partial-argument-matching path for predicates, NTTP or otherwise (see
 * PredicateType below).  Write a full-arity predicate and ignore whichever
 * parameters you don't need.
 *
 * Chaining needs a named type to call .prio()/.once() on - bare braces
 * can't do this (there's nothing to call a method on), and plain
 * "ConnParams()" unqualified won't work outside a member function of the
 * matching EventStorage.  Use ev.params(...) instead - a static method, but
 * callable through the instance, so it never needs ConnParams's type (or
 * MutexType) spelled out at all:
 * event.connect( receiver, method, event.params( pred ).prio( 5 ).once() );
 * @endcode
 *
 * @note Single-shot connections cannot be established through implicit or
 * brace initialization due to ambiguity with the uint32_t constructor
 * overload.  Attempting to mark the uint32_t constructor explicit and/or
 * deleting a bool constructor can result in compilation errors.  For these
 * reasons, single-shot connections require calling `once()` on an instance
 * of ConnParams.
 */

#include "callable.h"
#include "connection_type.h"
#include "event_detail.h"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace kmac {
namespace pulsar {

template< typename MutexType, typename... Args >
class ConnParams
{
	/**
	 * @brief Always Callable<bool(Args...)> - the event's exact argument
	 * list, never a truncated one.  (Predicates currently require full
	 * argument matching.)
	 */
	using PredicateType = Callable< bool( Args... ) >;

	uint32_t _priority = 0;                ///< see prio()
	PredicateType _predicate;              ///< see predicate() - moved out, not copied, when read
	PredicateContext _predicateContext = PredicateContext::Receiver;  ///< see predicateContext()

	/**
	 * @brief Whether a predicate is configured.  A dedicated flag rather
	 * than being derived from _predicate's live truthiness, specifically so
	 * hasPredicate() stays correct no matter when it is called relative to
	 * predicate() (which moves _predicate out on read - see predicate()'s
	 * docs).  Set once, alongside _predicate, in the Pred constructor and in
	 * both when() overloads; never touched by predicate() itself.
	 */
	bool _hasPredicate = false;

	bool _singleShot = false;  ///< see isSingleShot() / once()

public:
	/**
	 * @brief All defaults: priority 0, no predicate, not single-shot.
	 */
	ConnParams() = default;

	/**
	 * @brief Priority-only shorthand.  Intentionally not explicit, so a bare
	 * integer (e.g. `{ 5 }`, or an integer literal at a ConnParams-typed
	 * parameter) converts implicitly - this is what lets connect() call
	 * sites write `{ 5 }` instead of spelling out ConnParams.  Recognize the
	 * consequence: any call passing an integer where a ConnParams argument
	 * is expected converts through here rather than failing to compile.
	 */
	ConnParams( uint32_t priority );

	/**
	 * @brief Conditional connection with a runtime predicate (a capturing
	 * lambda or any other functor) - always owning (heap-allocated) via
	 * Callable::create(F&&), since an arbitrary functor's state has to be
	 * kept alive somewhere.  See the NTTP when() below for a non-owning
	 * alternative when the predicate is a free function.
	 */
	template<
		typename Pred,
		typename = std::enable_if_t< std::is_invocable_r_v< bool, Pred, Args... > > >
	ConnParams( Pred pred, PredicateContext predicateContext = PredicateContext::Receiver );

	/**
	 * @brief Get the configured priority.
	 */
	uint32_t prio() const;

	/**
	 * @brief Set the priority.
	 */
	ConnParams&& prio( uint32_t priority );

	/**
	 * @brief True if a predicate was configured (via the Pred constructor
	 * or either when() overload).  Safe to call in any order relative to
	 * predicate() - see _hasPredicate above and predicate() below.
	 */
	bool hasPredicate() const;

	/**
	 * @brief Moves the configured predicate out.  Destructive: after this
	 * call, this ConnParams's predicate is empty, regardless of what
	 * hasPredicate() reported before the call.  Read hasPredicate() and any
	 * other accessor you need in their own statements if you also need
	 * this value in the same call - do not rely on evaluation order to
	 * sequence a read of hasPredicate() against a read of predicate() in
	 * one function-call argument list; C++ does not guarantee which of two
	 * arguments in the same call is evaluated first.
	 */
	PredicateType&& predicate();

	/**
	 * @brief The context (Sender or Receiver) the configured predicate
	 * should be evaluated in.  Meaningless if hasPredicate() is false.
	 */
	PredicateContext predicateContext() const;

	/**
	 * @brief Specify free-function predicate via NTTP with explicit template
	 * argument, e.g.: `.when< &someFreeFunction >()`.
	 */
	template< bool (*Pred)( Args... ) >
	ConnParams&& when( PredicateContext predicateContext = PredicateContext::Receiver );

	// TODO: consider support for obj/method predicates

	/**
	 * @brief Specify the runtime predicate, using template argument
	 * deduction, to a lambda or functor along with optional context.
	 */
	template< typename Pred >
	ConnParams&& when( Pred&& pred, PredicateContext predicateContext = PredicateContext::Receiver );

	/**
	 * @brief True if once() was called - the connection will auto-disconnect
	 * after its handler fires once.
	 */
	bool isSingleShot() const;

	/**
	 * @brief Marks the connection single-shot: it auto-disconnects after
	 * firing once.
	 *
	 * static_assert fires here for SharedMutex instances rather than in the
	 * constructor as priority and conditional predicates are still supported
	 * features regardless of mutex type - a single-shot connection reentrantly
	 * disconnecting itself while a SharedLock is held would deadlock.
	 */
	ConnParams&& once();
};

// ===========================================================================
// ConnParams implementation
// ===========================================================================

template< typename MutexType, typename... Args >
inline ConnParams< MutexType, Args... >::ConnParams( uint32_t priority )
	: _priority( priority )
{
}

template< typename MutexType, typename... Args >
template< typename Pred, typename >
inline ConnParams< MutexType, Args... >::ConnParams( Pred pred, PredicateContext predicateContext )
	: _predicate( PredicateType::create( std::move( pred ) ) )
	, _predicateContext( predicateContext )
	, _hasPredicate( static_cast< bool >( _predicate ) )
{
}

template< typename MutexType, typename... Args >
inline uint32_t ConnParams< MutexType, Args... >::prio() const
{
	return _priority;
}

template< typename MutexType, typename... Args >
inline ConnParams< MutexType, Args... >&& ConnParams< MutexType, Args... >::prio( uint32_t priority )
{
	_priority = priority;
	return std::move( *this );
}

template< typename MutexType, typename... Args >
inline bool ConnParams< MutexType, Args... >::hasPredicate() const
{
	return _hasPredicate;
}

template< typename MutexType, typename... Args >
inline typename ConnParams< MutexType, Args... >::PredicateType&& ConnParams< MutexType, Args... >::predicate()
{
	return std::move( _predicate );
}

template< typename MutexType, typename... Args >
inline PredicateContext ConnParams< MutexType, Args... >::predicateContext() const
{
	return _predicateContext;
}

template< typename MutexType, typename... Args >
template< bool (*Pred)( Args... ) >
inline ConnParams< MutexType, Args... >&& ConnParams< MutexType, Args... >::when( PredicateContext predicateContext )
{
	_predicate = PredicateType::template create< Pred >();
	_predicateContext = predicateContext;
	_hasPredicate = static_cast< bool >( _predicate );
	return std::move( *this );
}

template< typename MutexType, typename... Args >
template< typename Pred >
inline ConnParams< MutexType, Args... >&& ConnParams< MutexType, Args... >::when(
	Pred&& pred, PredicateContext predicateContext )
{
	_predicate = PredicateType::create( std::forward< Pred >( pred ) );
	_predicateContext = predicateContext;
	_hasPredicate = static_cast< bool >( _predicate );
	return std::move( *this );
}

template< typename MutexType, typename... Args >
inline bool ConnParams< MutexType, Args... >::isSingleShot() const
{
	return _singleShot;
}

template< typename MutexType, typename... Args >
inline ConnParams< MutexType, Args... >&& ConnParams< MutexType, Args... >::once()
{
	static_assert( ! detail::HasLockShared_v< MutexType >,
		"once(): single-shot connections are not supported on SharedEvent (SharedMutex): "
		"disconnecting from within a handler while a SharedLock is held would deadlock. "
		"Use a manual ScopedConnection instead." );
	_singleShot = true;
	return std::move( *this );
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONN_PARAMS_H
