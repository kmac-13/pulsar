#pragma once
#ifndef KMAC_STELLYRA_COMBINING_EVENT_H
#define KMAC_STELLYRA_COMBINING_EVENT_H

/**
 * @file combining_event.h
 * @brief Event that collects and aggregates handler return values.
 *
 * CombiningEvent<Combiner, ReturnType, Args...> fires all connected handlers
 * and passes their return values through a Combiner functor to produce a
 * single result.  Useful for validation pipelines, consensus queries, and
 * aggregated data collection.
 *
 * @code
 * CombiningEvent< Combiners::LogicalAnd<>, bool, std::string > validate { this };
 * validate.connect< &Checker::check >( checker1 );
 * validate.connect< &Checker::check >( checker2 );
 * bool allOk = validate( "input" );
 * @endcode
 *
 * @section template_order Template argument order
 *
 * Combiner comes first, with ReturnType and Args... adjacent to each other
 * since together they describe the handler call signature ReturnType(Args...).
 *
 * @section connect_buckets Connection API
 *
 * The connection API is split into three named buckets, matching EventStorage:
 *
 *   connect / connectOnce
 *       - object/method connections: NTTP method pointer or runtime
 *         pointer-to-member-function, with or without an explicit
 *         Tracked<T> tracker
 *
 *   connectFree / connectOnceFree
 *       - free function connections: NTTP function pointer or runtime
 *         function pointer
 *
 *   connectLambda / connectOnceLambda
 *       - capturing lambda / arbitrary functor connections, either as a
 *         pre-wrapped Callable or as a raw functor that gets wrapped
 *         internally, with or without an explicit Trackable& tracker
 *
 * disconnect() / disconnectFree() are split the same way.
 *
 * @section threading Threading
 *
 * CombiningEvent protects its slot table with a plain (non-recursive) mutex.
 * Reentrancy from within a handler is not supported - do not connect,
 * disconnect, or trigger this event from inside a connected handler.
 *
 * All handlers are invoked Direct (synchronously on the triggering thread).
 * There is no Deferred variant because the return value must be available
 * synchronously.
 *
 * @section lifetime Receiver lifetime
 *
 * When a receiver inherits from Trackable (or a Tracked<T> bundle is passed),
 * connections are automatically disconnected when the receiver is destroyed.
 * Untracked callable connections must be disconnected manually before any
 * captured state is invalidated.
 *
 * @see combiners.h for available Combiner types
 */

#include <kmac/stellyra/callable.h>
#include <kmac/stellyra/connection.h>
#include <kmac/stellyra/event_flags.h>
#include <kmac/stellyra/event_impl_base.h>
#include <kmac/stellyra/gen_data.h>
#include <kmac/stellyra/platform.h>
#include <kmac/stellyra/trackable.h>

#include <memory>
#include <type_traits>
#include <vector>

#include <algorithm>  // for std::remove / std::remove_if / std::find (order maintenance)

namespace kmac {
namespace stellyra {

// Tracked<T> is defined in event_storage.h; forward-declare so
// combining_event.h does not have to include the full event machinery.
template< typename T >
struct Tracked;


// ===========================================================================
// CombiningHandlerEntry
// ===========================================================================

/**
 * @brief One handler slot inside CombiningEventImpl.
 *
 * Parallels HandlerEntry<Args...> but carries Callable<ReturnType(Args...)>
 * instead of Callable<void(Args...)>.  No EventLoop fields are needed -
 * CombiningEvent is always Direct.
 */
template< typename ReturnType, typename... Args >
struct CombiningHandlerEntry
{
	using HandlerType = Callable< ReturnType( Args... ) >;

	HandlerType handler;

	/// per-connection tag required by Trackable::trackConnection(); unused
	/// for task migration since there is no deferred dispatch.  Sourced from
	/// the receiver's Trackable::migrationTag() the same way BasicEvent's
	/// connections are, purely for consistency - it plays no functional role
	/// here.
	uint64_t connectionTag = 0;

	EventFlags flags;         ///< active + singleShot bits
	uint16_t generation = 0;  ///< generation counter for stale-handle detection

	void init( HandlerType&& h, uint64_t tag, bool singleShot );
};

template< typename ReturnType, typename... Args >
inline void CombiningHandlerEntry< ReturnType, Args... >::init(
	HandlerType&& h,
	uint64_t tag,
	bool singleShot )
{
	handler = std::move( h );
	connectionTag = tag;
	flags.setActive( true );
	flags.setIsSingleShot( singleShot );
}


// ===========================================================================
// CombiningEventImpl
// ===========================================================================

/**
 * @brief Heap-allocated implementation object for CombiningEvent.
 *
 * Inherits EventImplBase so that the standard Connection handle and Trackable
 * tracking machinery work without modification.  EventLoop-related overrides
 * are no-ops because CombiningEvent is always Direct.
 */
template< typename ReturnType, typename... Args >
struct CombiningEventImpl : EventImplBase
{
	using HandlerType = Callable< ReturnType( Args... ) >;
	using EntryType = CombiningHandlerEntry< ReturnType, Args... >;

	mutable platform::Mutex mutex;
	std::vector< EntryType > handlers;

	/**
	 * @brief Slot indices in true chronological connection order, used to
	 * drive emit() iteration so results are combined in connection order
	 * even after a disconnect frees a slot that a later connection reuses
	 * (handlers itself is stable-slot storage and is never reordered, since
	 * Connection addresses its target by fixed index - see EntryType).
	 */
	std::vector< uint32_t > order;

	/// earliest known free slot; handlers.size() when none is known
	uint32_t nextFree = 0;

	/**
	 * @brief Whole-event block depth (see EventImpl's identical member for
	 * the full rationale) - always atomic here, unlike EventImpl, since
	 * CombiningEventImpl always uses platform::Mutex and has no
	 * SingleThreadedEvent-equivalent variant to special-case.
	 */
	platform::Atomic< unsigned int > blockDepth { 0 };

	bool isBlocked() const noexcept
	{
		return blockDepth.load( std::memory_order_relaxed ) > 0;
	}

	void block() noexcept
	{
		blockDepth.fetch_add( 1, std::memory_order_relaxed );
	}

	void unblock() noexcept
	{
		unsigned int current = blockDepth.load( std::memory_order_relaxed );
		while ( current > 0
			&& ! blockDepth.compare_exchange_weak(
				current, current - 1, std::memory_order_relaxed ) )
		{
		}
	}

	// ---- EventImplBase overrides -------------------------------------------

	bool isHandlerConnected( uint32_t index, uint32_t generation ) const override;
	void disconnectHandler( uint32_t index, uint32_t generation ) override;
	void disconnectHandlers( const std::vector< GenData >& entries ) override;

	bool isHandlerBlocked( uint32_t index, uint32_t generation ) const override;
	void blockHandler( uint32_t index, uint32_t generation ) override;
	void unblockHandler( uint32_t index, uint32_t generation ) override;

	// CombiningEvent is Direct-only - EventLoop operations are no-ops
	void updateSenderLoop( EventLoop* ) override {}
	void updateHandlerLoop( uint32_t, uint32_t, EventLoop* ) override {}
	void invokeDeferred( uint32_t, uint32_t, const std::shared_ptr< void >& ) override {}

	// ---- API called directly by CombiningEvent -----------------------------

	/// disconnect all active slots, caller must hold mutex
	void preLockDisconnectAll();

private:
	// ---- internal plumbing - nothing outside CombiningEventImpl calls these -

	/**
	 * @brief True if `index` currently holds a live connection matching
	 * `generation` (in range, active, generation matches).
	 */
	bool isSlotLive( uint32_t index, uint32_t generation ) const;

	/**
	 * @brief Disconnects the connection at `index`.
	 *
	 * Precondition: caller holds `mutex` and has already confirmed the slot
	 * is live (via slotLive()).
	 *
	 * Releases the handler, marks the slot inactive, bumps its generation so
	 * a stale Connection can no longer reach it, and returns the slot to
	 * `nextFree` if it is now the earliest known free slot.
	 *
	 * Shared by disconnectHandler() and disconnectHandlers() so this logic
	 * is written once.
	 */
	void disconnectSlotLocked( uint32_t index );
};

// ---------------------------------------------------------------------------

template< typename ReturnType, typename... Args >
inline bool CombiningEventImpl< ReturnType, Args... >::isHandlerConnected( uint32_t index, uint32_t generation ) const
{
	platform::LockGuard< platform::Mutex > lock( mutex );
	return isSlotLive( index, generation );
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::disconnectHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< platform::Mutex > lock( mutex );

	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}

	disconnectSlotLocked( index );
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::disconnectHandlers( const std::vector< GenData >& entries )
{
	platform::LockGuard< platform::Mutex > lock( mutex );

	for ( auto [ index, generation ] : entries )
	{
		if ( ! isSlotLive( index, generation ) )
		{
			continue;
		}

		disconnectSlotLocked( index );
	}
}

template< typename ReturnType, typename... Args >
inline bool CombiningEventImpl< ReturnType, Args... >::isHandlerBlocked( uint32_t index, uint32_t generation ) const
{
	platform::LockGuard< platform::Mutex > lock( mutex );
	if ( ! isSlotLive( index, generation ) )
	{
		return false;
	}
	return handlers[ index ].flags.isBlocked();
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::blockHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< platform::Mutex > lock( mutex );
	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}
	handlers[ index ].flags.setBlocked( true );
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::unblockHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< platform::Mutex > lock( mutex );
	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}
	handlers[ index ].flags.setBlocked( false );
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::preLockDisconnectAll()
{
	for ( uint32_t i = 0; i < static_cast< uint32_t >( handlers.size() ); ++i )
	{
		if ( ! handlers[ i ].flags.isActive() )
		{
			continue;
		}
		handlers[ i ].flags.setActive( false );
		handlers[ i ].generation++;
		handlers[ i ].handler = HandlerType{};
	}
	order.clear();
	nextFree = 0;
}

template< typename ReturnType, typename... Args >
inline bool CombiningEventImpl< ReturnType, Args... >::isSlotLive( uint32_t index, uint32_t generation ) const
{
	return index < handlers.size()
		&& handlers[ index ].generation == generation
		&& handlers[ index ].flags.isActive();
}

template< typename ReturnType, typename... Args >
inline void CombiningEventImpl< ReturnType, Args... >::disconnectSlotLocked( uint32_t index )
{
	handlers[ index ].flags.setActive( false );
	handlers[ index ].generation++;
	{
		HandlerType released = std::move( handlers[ index ].handler );
	}

	order.erase( std::remove( order.begin(), order.end(), index ), order.end() );

	if ( index < nextFree )
	{
		nextFree = index;
	}
}


// ===========================================================================
// CombiningEvent
// ===========================================================================

/**
 * @brief Event that collects handler return values and combines them.
 *
 * @tparam Combiner functor: (Iter first, Iter last) -> ReturnType
 * @tparam ReturnType type returned by each handler and by the event itself
 * @tparam Args argument types forwarded to every handler
 */
template< typename Combiner, typename ReturnType, typename... Args >
class CombiningEvent
{
public:
	using HandlerType = Callable< ReturnType( Args... ) >;

private:
	using Impl = CombiningEventImpl< ReturnType, Args... >;

	platform::SharedPtr< Impl > _impl { std::make_shared< Impl >() };
	Combiner _combiner;

public:
	/// Construct with no owner.
	CombiningEvent() = default;

	/**
	 * @brief Construct with an owner Trackable.
	 *
	 * The owner is accepted for API consistency with BasicEvent.
	 * CombiningEvent is Direct-only so the sender EventLoop is not used.
	 */
	explicit CombiningEvent( Trackable* owner );

	~CombiningEvent() = default;

	CombiningEvent( const CombiningEvent& ) = delete;
	CombiningEvent& operator=( const CombiningEvent& ) = delete;
	CombiningEvent( CombiningEvent&& ) = default;
	CombiningEvent& operator=( CombiningEvent&& ) = default;

	// =========================================================================
	// triggering
	// =========================================================================

	/**
	 * @brief Invoke all handlers and return the combined result.
	 *
	 * Handlers are invoked while holding the internal mutex; reentrancy
	 * (connecting, disconnecting, or re-triggering from inside a handler)
	 * is not supported and will deadlock.  Arguments are passed as lvalues
	 * so every handler receives the same values.
	 *
	 * Returns the combiner's empty-range result if the event is blocked or
	 * no handlers are connected.
	 */
	ReturnType operator()( Args... args );

	/// Identical to operator().
	ReturnType trigger( Args... args );

	/// Identical to operator().
	ReturnType emit( Args... args );

	// =========================================================================
	// blocking
	// =========================================================================

	/// Increment the block count; triggers are dropped while count > 0.
	void block();

	/// Decrement the block count.
	void unblock();

	/**
	 * @brief RAII block guard - blocks on construction, unblocks on destruction.
	 * Guards nest correctly.
	 */
	[[ nodiscard ]] BlockGuard blockGuard();

	// =========================================================================
	// object / method connections
	// =========================================================================

	/**
	 * @brief Connect a member function via NTTP.
	 *
	 * Auto-disconnect is registered when @p receiver derives from Trackable.
	 *
	 * @code
	 * event.connect< &MyClass::onQuery >( myObj );
	 * @endcode
	 */
	template< auto Method, typename T >
	Connection connect( T& receiver );

	/**
	 * @brief NTTP connect with an explicit tracker.
	 *
	 * @code
	 * Anchor anchor;
	 * event.connect< &MyClass::onQuery >( Tracked{ obj, anchor } );
	 * @endcode
	 */
	template< auto Method, typename T >
	Connection connect( Tracked< T > tracked );

	/// Connect a runtime pointer-to-member-function.
	template< typename T >
	Connection connect( T& receiver, ReturnType ( T::*method )( Args... ) );

	/// Runtime PMF connect with explicit tracker.
	template< typename T >
	Connection connect( Tracked< T > tracked, ReturnType ( T::*method )( Args... ) );

	/// NTTP single-shot connect.
	template< auto Method, typename T >
	Connection connectOnce( T& receiver );

	/// NTTP single-shot connect with tracker.
	template< auto Method, typename T >
	Connection connectOnce( Tracked< T > tracked );

	/// Runtime PMF single-shot connect.
	template< typename T >
	Connection connectOnce( T& receiver, ReturnType ( T::*method )( Args... ) );

	/// Runtime PMF single-shot connect with tracker.
	template< typename T >
	Connection connectOnce( Tracked< T > tracked, ReturnType ( T::*method )( Args... ) );

	// =========================================================================
	// free-function connections
	// =========================================================================

	/**
	 * @brief Connect a free function via NTTP.
	 *
	 * @code
	 * event.connectFree< &myFreeFunction >();
	 * @endcode
	 */
	template< ReturnType (*Func)( Args... ) >
	Connection connectFree();

	/// Connect a runtime free-function pointer.
	Connection connectFree( ReturnType (*func)( Args... ) );

	/// NTTP single-shot free-function connect.
	template< ReturnType (*Func)( Args... ) >
	Connection connectOnceFree();

	/// Runtime free-function single-shot connect.
	Connection connectOnceFree( ReturnType (*func)( Args... ) );

	// =========================================================================
	// lambda / Callable connections
	// =========================================================================

	/**
	 * @brief Connect a pre-wrapped Callable.
	 *
	 * No automatic lifetime tracking - disconnect before captures are
	 * invalidated.
	 */
	Connection connectLambda( HandlerType&& handler );

	/// Pre-wrapped Callable connect with explicit tracker.
	Connection connectLambda( Trackable& tracker, HandlerType&& handler );

	/**
	 * @brief Connect a functor (typically a capturing lambda) directly,
	 * without the caller needing to wrap it in Callable::create() first.
	 *
	 * @code
	 * event.connectLambda( [ this ]( int x ) { return onQuery( x ); } );
	 * @endcode
	 */
	template< typename F,
		typename = std::enable_if_t< ! std::is_base_of_v< Trackable, std::remove_reference_t< F > > > >
	Connection connectLambda( F&& functor );

	/// Raw functor connect with explicit tracker.
	template< typename F >
	Connection connectLambda( Trackable& tracker, F&& functor );

	/// Pre-wrapped Callable single-shot connect.
	Connection connectOnceLambda( HandlerType&& handler );

	/// Pre-wrapped Callable single-shot connect with tracker.
	Connection connectOnceLambda( Trackable& tracker, HandlerType&& handler );

	/// Raw functor single-shot connect, wrapped internally.
	template< typename F,
		typename = std::enable_if_t< ! std::is_base_of_v< Trackable, std::remove_reference_t< F > > > >
	Connection connectOnceLambda( F&& functor );

	/// Raw functor single-shot connect with tracker, wrapped internally.
	template< typename F >
	Connection connectOnceLambda( Trackable& tracker, F&& functor );

	// =========================================================================
	// disconnection
	// =========================================================================

	/// Disconnect all connections tracked by @p tracker.
	void disconnect( Trackable& tracker );

	/// Disconnect all connections to a specific NTTP method on a receiver.
	template< auto Method, typename T >
	void disconnect( T& receiver );

	/// Disconnect all connections to a specific NTTP free function.
	template< ReturnType (*Func)( Args... ) >
	void disconnectFree();

	/// Disconnect all connections to a specific runtime free-function pointer.
	void disconnectFree( ReturnType (*func)( Args... ) );

	/// Disconnect every active connection on this event.
	void disconnectAll();

	// =========================================================================
	// inspection
	// =========================================================================

	/**
	 * @brief Total number of slots (includes inactive slots awaiting reuse).
	 * Useful for debugging; not a count of live connections.
	 */
	std::size_t slotCount() const;

private:
	Connection connectImpl( HandlerType&& handler, Trackable* tracker, bool singleShot );
};


// ===========================================================================
// constructor
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline CombiningEvent< Combiner, ReturnType, Args... >::CombiningEvent( Trackable* /*owner*/ )
{
}


// ===========================================================================
// operator() / trigger / emit
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline ReturnType CombiningEvent< Combiner, ReturnType, Args... >::operator()( Args... args )
{
	// empty results vector serves as the "nothing to combine" base case for
	// both the blocked path and the no-active-handlers path
	std::vector< ReturnType > results;

	if ( ! _impl->isBlocked() )
	{
		platform::LockGuard< platform::Mutex > lock( _impl->mutex );

		results.reserve( _impl->order.size() );

		// single-shot handlers that fire this round are removed from
		// _impl->order after the loop, not during it - order is what we are
		// iterating right now, and mutating it mid-iteration would either
		// invalidate the iteration or require fiddly index bookkeeping for
		// no benefit, since nothing else touches order while mutex is held
		std::vector< uint32_t > firedSingleShots;

		for ( uint32_t index : _impl->order )
		{
			auto& entry = _impl->handlers[ index ];
			if ( ! entry.flags.isActive() || entry.flags.isBlocked() )
			{
				continue;
			}

			const bool once = entry.flags.isSingleShot();

			// args are intentionally not forwarded - multiple handlers
			// read the same values
			results.push_back( entry.handler( args... ) );

			if ( once )
			{
				entry.flags.setActive( false );
				entry.generation++;
				{ HandlerType released = std::move( entry.handler ); }
				if ( index < _impl->nextFree )
				{
					_impl->nextFree = index;
				}
				firedSingleShots.push_back( index );
			}
		}

		if ( ! firedSingleShots.empty() )
		{
			_impl->order.erase(
				std::remove_if( _impl->order.begin(), _impl->order.end(),
					[ &firedSingleShots ]( uint32_t idx ) {
						return std::find( firedSingleShots.begin(), firedSingleShots.end(), idx )
							!= firedSingleShots.end();
					} ),
				_impl->order.end() );
		}
	}

	return _combiner( results.begin(), results.end() );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline ReturnType CombiningEvent< Combiner, ReturnType, Args... >::trigger( Args... args )
{
	return operator()( std::forward< Args >( args )... );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline ReturnType CombiningEvent< Combiner, ReturnType, Args... >::emit( Args... args )
{
	return operator()( std::forward< Args >( args )... );
}


// ===========================================================================
// blocking
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline void CombiningEvent< Combiner, ReturnType, Args... >::block()
{
	_impl->block();
}

template< typename Combiner, typename ReturnType, typename... Args >
inline void CombiningEvent< Combiner, ReturnType, Args... >::unblock()
{
	_impl->unblock();
}

template< typename Combiner, typename ReturnType, typename... Args >
inline BlockGuard CombiningEvent< Combiner, ReturnType, Args... >::blockGuard()
{
	_impl->block();
	return BlockGuard(
		_impl,
		Callable< void() >::create< &CombiningEventImpl< ReturnType, Args... >::unblock >( _impl.get() ) );
}


// ===========================================================================
// object / method connections
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
template< auto Method, typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connect( T& receiver )
{
	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	return connectImpl(
		HandlerType::template create< Method >( &receiver ),
		tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< auto Method, typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connect( Tracked< T > tracked )
{
	return connectImpl(
		HandlerType::template create< Method >( &tracked.receiver ),
		&tracked.tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connect(
	T& receiver,
	ReturnType ( T::*method )( Args... ) )
{
	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	return connectImpl(
		HandlerType::create(
			[ &receiver, method ]( Args... a ) -> ReturnType {
				return ( receiver.*method )( a... );
			} ),
		tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connect(
	Tracked< T > tracked,
	ReturnType ( T::*method )( Args... ) )
{
	return connectImpl(
		HandlerType::create(
			[ &r = tracked.receiver, method ]( Args... a ) -> ReturnType {
				return ( r.*method )( a... );
			} ),
		&tracked.tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< auto Method, typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnce( T& receiver )
{
	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	return connectImpl(
		HandlerType::template create< Method >( &receiver ),
		tracker, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< auto Method, typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnce( Tracked< T > tracked )
{
	return connectImpl(
		HandlerType::template create< Method >( &tracked.receiver ),
		&tracked.tracker, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnce(
	T& receiver,
	ReturnType ( T::*method )( Args... ) )
{
	Trackable* tracker = nullptr;
	if constexpr ( std::is_base_of_v< Trackable, T > )
	{
		tracker = static_cast< Trackable* >( &receiver );
	}
	return connectImpl(
		HandlerType::create(
			[ &receiver, method ]( Args... a ) -> ReturnType {
				return ( receiver.*method )( a... );
			} ),
		tracker, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename T >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnce(
	Tracked< T > tracked,
	ReturnType ( T::*method )( Args... ) )
{
	return connectImpl(
		HandlerType::create(
			[ &r = tracked.receiver, method ]( Args... a ) -> ReturnType {
				return ( r.*method )( a... );
			} ),
		&tracked.tracker, true );
}


// ===========================================================================
// free-function connections
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
template< ReturnType (*Func)( Args... ) >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectFree()
{
	return connectImpl( HandlerType::template create< Func >(), nullptr, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectFree( ReturnType (*func)( Args... ) )
{
	return connectImpl( HandlerType::create( func ), nullptr, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< ReturnType (*Func)( Args... ) >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceFree()
{
	return connectImpl( HandlerType::template create< Func >(), nullptr, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceFree( ReturnType (*func)( Args... ) )
{
	return connectImpl( HandlerType::create( func ), nullptr, true );
}


// ===========================================================================
// lambda / Callable connections
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectLambda( HandlerType&& handler )
{
	return connectImpl( std::move( handler ), nullptr, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectLambda(
	Trackable& tracker,
	HandlerType&& handler )
{
	return connectImpl( std::move( handler ), &tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename F, typename >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectLambda( F&& functor )
{
	return connectImpl( HandlerType::create( std::forward< F >( functor ) ), nullptr, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename F >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectLambda(
	Trackable& tracker,
	F&& functor )
{
	return connectImpl( HandlerType::create( std::forward< F >( functor ) ), &tracker, false );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceLambda( HandlerType&& handler )
{
	return connectImpl( std::move( handler ), nullptr, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceLambda(
	Trackable& tracker,
	HandlerType&& handler )
{
	return connectImpl( std::move( handler ), &tracker, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename F, typename >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceLambda( F&& functor )
{
	return connectImpl( HandlerType::create( std::forward< F >( functor ) ), nullptr, true );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< typename F >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectOnceLambda(
	Trackable& tracker,
	F&& functor )
{
	return connectImpl( HandlerType::create( std::forward< F >( functor ) ), &tracker, true );
}


// ===========================================================================
// disconnection
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline void CombiningEvent< Combiner, ReturnType, Args... >::disconnect( Trackable& tracker )
{
	auto connections = tracker.extractConnectionsTo( _impl.get() );
	_impl->disconnectHandlers( connections );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< auto Method, typename T >
inline void CombiningEvent< Combiner, ReturnType, Args... >::disconnect( T& receiver )
{
	const HandlerType target = HandlerType::template create< Method >( &receiver );

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< platform::Mutex > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, entry.generation );
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename Combiner, typename ReturnType, typename... Args >
template< ReturnType (*Func)( Args... ) >
inline void CombiningEvent< Combiner, ReturnType, Args... >::disconnectFree()
{
	const HandlerType target = HandlerType::template create< Func >();

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< platform::Mutex > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, entry.generation );
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline void CombiningEvent< Combiner, ReturnType, Args... >::disconnectFree(
	ReturnType (*func)( Args... ) )
{
	const HandlerType target = HandlerType::create( func );

	std::vector< GenData > toDisconnect;
	{
		platform::LockGuard< platform::Mutex > lock( _impl->mutex );
		for ( uint32_t i = 0; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
		{
			auto& entry = _impl->handlers[ i ];
			if ( entry.flags.isActive() && entry.handler == target )
			{
				toDisconnect.emplace_back( i, entry.generation );
			}
		}
	}
	_impl->disconnectHandlers( toDisconnect );
}

template< typename Combiner, typename ReturnType, typename... Args >
inline void CombiningEvent< Combiner, ReturnType, Args... >::disconnectAll()
{
	platform::LockGuard< platform::Mutex > lock( _impl->mutex );
	_impl->preLockDisconnectAll();
}


// ===========================================================================
// inspection
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline std::size_t
CombiningEvent< Combiner, ReturnType, Args... >::slotCount() const
{
	platform::LockGuard< platform::Mutex > lock( _impl->mutex );
	return _impl->handlers.size();
}


// ===========================================================================
// connectImpl
// ===========================================================================

template< typename Combiner, typename ReturnType, typename... Args >
inline Connection CombiningEvent< Combiner, ReturnType, Args... >::connectImpl(
	HandlerType&& handler,
	Trackable* tracker,
	bool singleShot )
{
	uint32_t index;
	uint16_t gen;
	const uint64_t tag = tracker ? tracker->migrationTag() : 0;

	{
		platform::LockGuard< platform::Mutex > lock( _impl->mutex );

		if ( _impl->nextFree < _impl->handlers.size() )
		{
			index = _impl->nextFree;
			gen = _impl->handlers[ index ].generation;
			_impl->handlers[ index ].init( std::move( handler ), tag, singleShot );
			_impl->order.push_back( index );

			uint32_t next = static_cast< uint32_t >( _impl->handlers.size() );
			for ( uint32_t i = index + 1; i < static_cast< uint32_t >( _impl->handlers.size() ); ++i )
			{
				if ( ! _impl->handlers[ i ].flags.isActive() )
				{
					next = i;
					break;
				}
			}
			_impl->nextFree = next;
		}
		else
		{
			index = static_cast< uint32_t >( _impl->handlers.size() );
			gen = 0;

			typename Impl::EntryType entry;
			entry.init( std::move( handler ), tag, singleShot );
			_impl->handlers.push_back( std::move( entry ) );
			_impl->order.push_back( index );
			_impl->nextFree = static_cast< uint32_t >( _impl->handlers.size() );
		}
	}

	platform::WeakPtr< EventImplBase > weak( _impl );
	if ( tracker )
	{
		tracker->trackConnection( weak, index, gen );
	}

	return Connection( std::move( weak ), index, gen );
}


// ===========================================================================
// aliases
// ===========================================================================

/** @brief Alias for users that prefer signal/emit terminology. */
template< typename Combiner, typename ReturnType, typename... Args >
using CombiningSignal = CombiningEvent< Combiner, ReturnType, Args... >;

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_COMBINING_EVENT_H
