#pragma once
#ifndef KMAC_PULSAR_EVENT_DETAIL_H
#define KMAC_PULSAR_EVENT_DETAIL_H

/**
 * @file event_detail.h
 * @brief Detail helpers shared between event.h and event_impl.h.
 */

#include "platform.h"

#include "connection_type.h"
#include "event_loop.h"

#include <tuple>
#include <type_traits>
#include <utility>

namespace kmac {
namespace pulsar {

namespace detail {

/**
 * @brief True if M has a lock_shared() member function - the standard
 * signal that M is a shared/reader-writer mutex (e.g. std::shared_mutex),
 * as opposed to an exclusive-only mutex.  Used by ConnParams::once() to
 * static_assert against single-shot connections on SharedEvent, where a
 * handler disconnecting itself while a SharedLock is held would deadlock.
 */
template< typename M, typename = void >
struct HasLockShared : std::false_type {};

template< typename M >
struct HasLockShared< M, std::void_t< decltype( std::declval< M& >().lock_shared() ) > >
	: std::true_type {};

template< typename M >
inline constexpr bool HasLockShared_v = HasLockShared< M >::value;

// ---------------------------------------------------------------------------

/**
 * @brief Extracts the class type from a member function pointer.
 * Used to infer T for Tracked< T > overloads so the caller can write
 * { obj, anchor } instead of Tracked{ obj, anchor }.
 */
template< typename Method >
struct ExtractReceiver;

template< typename Class, typename Ret, typename... Args >
struct ExtractReceiver< Ret ( Class::* )( Args... ) >
{
	using type = Class;
};

template< typename Class, typename Ret, typename... Args >
struct ExtractReceiver< Ret ( Class::* )( Args... ) const >
{
	using type = Class;
};

template< typename Method >
using ExtractReceiver_t = typename ExtractReceiver< Method >::type;

/**
 * @brief Wraps T in a non-deduced context so the compiler does not try
 * to deduce T from a braced-init-list argument.  When T is already known
 * (via a default template argument or another parameter), the concrete
 * type is fully determined and { &obj, anchor } can aggregate-initialise
 * Tracked< T > cleanly.
 */
template< typename T >
struct NonDeduced
{
	using type = T;
};

template< typename T >
using NonDeduced_t = typename NonDeduced< T >::type;

/**
 * @brief Number of parameters declared on a member function pointer.
 * Used by connect<Method>()/connectFree<Func>() to decide at compile time
 * whether the target takes the full Args... pack (use Callable::create) or
 * only a leading prefix of it (use Callable::createPartial).
 */
template< typename Method >
struct MethodArity;

template< typename Class, typename Ret, typename... HandlerArgs >
struct MethodArity< Ret ( Class::* )( HandlerArgs... ) >
{
	static constexpr std::size_t value = sizeof...( HandlerArgs );
};

template< typename Class, typename Ret, typename... HandlerArgs >
struct MethodArity< Ret ( Class::* )( HandlerArgs... ) const >
{
	static constexpr std::size_t value = sizeof...( HandlerArgs );
};

template< typename Method >
inline constexpr std::size_t MethodArity_v = MethodArity< Method >::value;

/**
 * @brief Number of parameters declared on a free function pointer.
 * Free-function counterpart of MethodArity, used by connectFree<Func>().
 */
template< typename Func >
struct FunctionArity;

template< typename Ret, typename... HandlerArgs >
struct FunctionArity< Ret (*)( HandlerArgs... ) >
{
	static constexpr std::size_t value = sizeof...( HandlerArgs );
};

template< typename Func >
inline constexpr std::size_t FunctionArity_v = FunctionArity< Func >::value;

/**
 * @brief Calls (receiver.*method)(...) with only the leading
 * sizeof...(MethodArgs) elements of a forwarded argument tuple, dropping
 * the rest.  Used by the runtime (non-NTTP) connect(receiver, method)
 * overload to support a method with fewer parameters than the event
 * provides.  The NTTP form (connect<Method>(receiver)) gets this for free
 * via MethodArity_v + Callable::createPartial, since Method's type is
 * known at the call site; a runtime method pointer parameter's type is
 * fixed to void (T::*)(Args...) with no room to detect a shorter parameter
 * list from the outside, so the arity check and partial forwarding both
 * have to happen here instead, on the caller's side of Callable.
 */
template< typename T, typename... MethodArgs, typename... Args, std::size_t... I >
inline void invokeLeadingImpl(
	T& receiver, void ( T::*method )( MethodArgs... ),
	std::tuple< Args&&... > argTuple, std::index_sequence< I... > )
{
	(void) argTuple;  // unused when sizeof...(I) == 0 (method takes no arguments)
	( receiver.*method )( std::get< I >( std::move( argTuple ) )... );
}

template< typename T, typename... MethodArgs, typename... Args >
inline void invokeLeading( T& receiver, void ( T::*method )( MethodArgs... ), Args&&... args )
{
	invokeLeadingImpl( receiver, method,
		std::forward_as_tuple( std::forward< Args >( args )... ),
		std::make_index_sequence< sizeof...( MethodArgs ) >{} );
}

// ---------------------------------------------------------------------------

/**
 * @brief Named functor holding the (receiver, method) pair for a runtime
 * (non-NTTP) connect(receiver, method) connection.
 *
 * It is a named type, not an anonymous lambda, so that the connection has a
 * comparable identity: its Callable::functorStub<PmfInvoker> address is a
 * stable per-type key, and the receiver and method it stores are plain data
 * members.  disconnect(receiver, method) uses that to recover the pair (via
 * Callable::targetAs) and match it with the language's native
 * pointer-to-member ==.
 *
 * The Signature parameter (the event's own void(Args...)) separates the
 * event's argument pack from the method's own, possibly shorter, MethodArgs
 * pack.  When the method takes fewer arguments than the event provides, the
 * trailing arguments are dropped via invokeLeading.  Because MethodArgs is
 * part of the type, a partial-arity and a full-arity connection are distinct
 * PmfInvoker types with distinct stub addresses, so identity matching keeps
 * them separate.
 */
template< typename Signature, typename T, typename... MethodArgs >
struct PmfInvoker;

template< typename... Args, typename T, typename... MethodArgs >
struct PmfInvoker< void ( Args... ), T, MethodArgs... >
{
	T* _receiver;                          ///< bound receiver; non-owning, must outlive the connection
	void ( T::*_method )( MethodArgs... ); ///< the runtime method pointer supplied to connect(receiver, method)

	void operator()( Args... args ) const
	{
		if constexpr ( sizeof...( MethodArgs ) == sizeof...( Args ) )
		{
			( _receiver->*_method )( std::forward< Args >( args )... );
		}
		else
		{
			invokeLeading( *_receiver, _method, std::forward< Args >( args )... );
		}
	}
};

// ---------------------------------------------------------------------------

/**
 * @brief Computes how a connection should actually dispatch, given its
 * declared ConnectionType and the current sender/receiver EventLoop
 * topology.  Called at connect() time and again whenever either side's
 * EventLoop changes via setEventLoop() (see Trackable::setEventLoop()).
 *
 * @param declared the ConnectionType set at connect() time
 * @param senderLoop the sending event's owner's EventLoop, or nullptr
 * @param receiverLoop the tracked receiver's EventLoop, or nullptr
 * @returns Direct, Deferred, or None (topology cannot support either -
 *   the connection remains live but is silently skipped on trigger)
 */
inline ResolvedConnectionType resolveConnectionType(
	ConnectionType declared,
	EventLoop* senderLoop,
	EventLoop* receiverLoop )
{
	// Direct always resolves as Direct
	if ( declared == ConnectionType::Direct )
	{
		return ResolvedConnectionType::Direct;
	}

	// Deferred always resolves as Deferred as long as the receiver's
	// EventLoop is valid; otherwise, it resolves to None (i.e. don't call)
	if ( declared == ConnectionType::Deferred )
	{
		return receiverLoop
			? ResolvedConnectionType::Deferred
			: ResolvedConnectionType::None;
	}

	// Auto resolves to Direct if the sender and receiver EventLoops are the
	// same (including the case where both are nullptr)
	if ( senderLoop == receiverLoop )
	{
		return ResolvedConnectionType::Direct;
	}

	// Auto resolves to Deferred if the receiver EventLoop is valid (but
	// is different from the sender EventLoop)
	if ( receiverLoop )
	{
		return ResolvedConnectionType::Deferred;
	}

	// Auto resolves to None (i.e. don't call) if the sender EventLoop is
	// valid but the receiver EventLoop is not
	return ResolvedConnectionType::None;
}

}  // namespace detail


} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_DETAIL_H
