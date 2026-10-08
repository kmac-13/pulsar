#pragma once
#ifndef KMAC_STELLYRA_EVENT_IMPL_H
#define KMAC_STELLYRA_EVENT_IMPL_H

/**
 * @file event_impl.h
 * @brief EventImpl<MutexType, Args...> - the heap-allocated implementation
 * object behind every BasicEvent.  Owns the handler vector, mutex, dispatch
 * bookkeeping, and the full dispatch path (fast and slow).
 *
 * Separate from event.h so BasicEvent can be read as pure public API.
 *
 * Storage model: `handlers` holds each connection in a stable slot that is
 * never moved.  A disconnect turns its slot into an inactive hole (the
 * HandlerEntry stays in place, its generation bumped); a connect fills the
 * earliest such hole (tracked by `nextFree`) or appends when none is free.
 * A Connection therefore refers to its slot directly - index plus the slot's
 * generation - with no separate handle-to-slot indirection to maintain.
 *
 * Because slots never move, dispatch order is not implied by physical
 * position and is defined per priority level.  When every active connection
 * has the default priority (0), dispatch is a plain walk over `handlers` in
 * slot order and no ordering structure exists at all.  The first non-default
 * priority connection activates `order`, a list of active slots sorted by
 * (priority descending, then slot index) that dispatch walks instead; it is
 * torn back down once the last non-default-priority connection disconnects,
 * so priority ordering is paid for only while it is in use.  Within one
 * priority level the order is slot order - the same order the default fast
 * path uses - not connection order; a specific trigger order is expressed
 * through priority.
 */

#include "platform.h"

#include "callable.h"
#include "connection_type.h"
#include "event_impl_base.h"
#include "event_loop.h"
#include "gen_data.h"
#include "handler_entry.h"
#include "event_detail.h"
#include "trackable.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

namespace kmac {
namespace stellyra {

// forward declaration so EventImpl can befriend BasicEvent
template< typename MutexType, typename... Args > class BasicEvent;

/**
 * @brief Heap-allocated implementation object for BasicEvent.
 *
 * @tparam MutexType synchronisation strategy (RecursiveMutex, NullMutex,
 *    SharedMutex, FastRecursiveMutex)
 * @tparam Args the event argument types
 */
template< typename MutexType, typename... Args >
struct BasicEventImpl : EventImplBase
{
	using HandlerType = Callable< void( Args... ) >;
	using HandlerEntryType = HandlerEntry< Args... >;
	using PredicateType = Callable< bool( Args... ) >;

	// ---- storage ----------------------------------------------------------

	mutable MutexType mutex;

	/**
	 * @brief Handler storage indexed by stable slot.  A disconnected slot
	 * stays here as an inactive hole (retaining its generation) until reused.
	 * A Connection's index is its slot in this array directly.
	 */
	std::vector< HandlerEntryType > handlers;

	/**
	 * @brief Index of the earliest known inactive slot, or handlers.size()
	 * when no free slot is known.  Maintained on the non-reentrant paths;
	 * rebuilt by reconcileAfterTrigger() when reentrant activity occurred.
	 */
	uint32_t nextFree = 0;

	/**
	 * @brief Active slots in dispatch order (priority descending, then slot
	 * index), maintained only while at least one non-default-priority
	 * connection exists (see priorityConnCount).  Empty - and unwalked - in
	 * the common all-default-priority case, where dispatch walks `handlers`
	 * in slot order instead.
	 */
	std::vector< uint32_t > order;

	/**
	 * @brief Count of active connections whose priority is non-zero.  While
	 * greater than zero, `order` is maintained and dispatch walks it; when it
	 * returns to zero, `order` is cleared and dispatch reverts to the plain
	 * slot-order walk.
	 */
	uint32_t priorityConnCount = 0;

	/**
	 * @brief Nesting depth of active dispatch() calls on this thread, under
	 * the exclusive lock only - see the class-level note on why the shared
	 * (concurrent-reader) lock path never touches this.  While > 0, connect/
	 * disconnect still update isActive()/generation immediately (so
	 * isConnected() stays synchronous), but avoid the two mutations that
	 * would disturb an in-progress walk: a connect always appends rather than
	 * reusing a hole (an appended slot sits past the size the walk snapshotted,
	 * so it is never visited in the current pass), and neither connect nor
	 * disconnect touches `nextFree` or `order`/priorityConnCount.  Instead
	 * `hadReentrantActivity` is set, and reconcileAfterTrigger() rescans
	 * `nextFree` and rebuilds the ordering once the outermost dispatch unwinds.
	 *
	 * @note This counter, and the append-only connect behaviour it gates
	 * (see addConnection), only ever come into play for a connect/disconnect
	 * made by the same thread that is still inside dispatch()'s call stack -
	 * e.g. a handler that connects a new handler to this same event while
	 * firing.  connectImpl()/disconnect() take the same mutex dispatch()
	 * holds, so a call from any other thread simply blocks until dispatch()
	 * releases that lock; by the time it acquires the lock and runs,
	 * triggerDepth has already returned to 0 and reconcileAfterTrigger() has
	 * already run, so it always takes the ordinary first-fit path below.
	 *
	 * @note Reentrant connect/disconnect from within a SharedEvent's own
	 * Direct handler already deadlocks by design (see ConnParams::once()'s
	 * static_assert against SharedEvent) before it could ever reach this
	 * counter, since it would require acquiring the exclusive lock while
	 * this thread already holds the shared one - so the shared-lock dispatch
	 * paths never need to participate in this deferral scheme at all.
	 */
	uint32_t triggerDepth = 0;

	/**
	 * @brief Set when a connect or disconnect happens while triggerDepth > 0,
	 * so reconcileAfterTrigger() can skip its rescan entirely when the
	 * dispatch made no structural changes.
	 */
	bool hadReentrantActivity = false;

	Trackable* owner = nullptr;  ///< the Trackable this event was constructed with, or nullptr; its EventLoop is the sender loop for Auto resolution

	/**
	 * @brief Count of active connections whose resolved type is not Direct
	 * (i.e. Deferred or None).  Zero means the fast dispatch path applies:
	 * no per-slot connection-type switch, no task posting.  Kept in step by
	 * addConnection(), disconnectSlotLocked(), updateSenderLoop(), and
	 * updateHandlerLoop() so dispatch() never has to scan for it.
	 */
	uint32_t nonDirectCount = 0;

	/**
	 * @brief Count of active connections with a predicate attached.  Zero
	 * means the fast dispatch path applies: no predicate evaluation, and
	 * `predicates` need not even be sized to match `handlers`.
	 */
	uint32_t predicateCount = 0;

	/**
	 * @brief Sender-context predicates, parallel to handlers by slot; lazily
	 * allocated the first time a predicate is attached via ConnParams
	 * (constructor or when()) and grown to match handlers from that point
	 * on.  Because slots are stable, an entry keeps its predicate slot for
	 * life; a slot without a predicate holds a default-constructed (null)
	 * Callable.
	 */
	std::vector< PredicateType > predicates;

	// stable self-reference used by dispatch() for deferred task closures;
	// set by BasicEvent at construction, valid across BasicEvent moves
	platform::WeakPtr< EventImplBase > weakSelf;

	// ---- whole-event blocking ----------------------------------------------

	/**
	 * @brief Plain unsigned int for NullMutex (SingleThreadedEvent) - no
	 * locking happens anywhere else on this event either, so an atomic here
	 * would be paying for thread-safety the rest of the type doesn't have.
	 * Atomic<unsigned int> otherwise, since block()/unblock() are documented
	 * as callable from any thread.  Lives here rather than on EventImplBase
	 * specifically so this can vary by MutexType at all - see BlockGuard in
	 * connection.h for how a type-erased guard still reaches this.
	 */
	using BlockDepthType = std::conditional_t<
		std::is_same_v< MutexType, platform::NullMutex >,
		unsigned int,
		platform::Atomic< unsigned int > >;

	BlockDepthType blockDepth { 0 };

	/**
	 * @brief Returns true if the event is currently blocked.
	 */
	bool isBlocked() const noexcept;

	/**
	 * @brief Increment the block depth; triggers are silently dropped while
	 * depth > 0.  May be called from any thread (see BlockDepthType above).
	 */
	void block() noexcept;

	/**
	 * @brief Decrement the block depth.  Triggers resume when depth reaches 0.
	 */
	void unblock() noexcept;

	// ---- EventImplBase overrides --------------------------------------------

	bool isHandlerConnected( uint32_t index, uint32_t generation ) const override;

	void disconnectHandler( uint32_t index, uint32_t generation ) override;

	void disconnectHandlers( const std::vector< GenData >& entries ) override;

	bool isHandlerBlocked( uint32_t index, uint32_t generation ) const override;

	void blockHandler( uint32_t index, uint32_t generation ) override;

	void unblockHandler( uint32_t index, uint32_t generation ) override;

	void updateSenderLoop( EventLoop* loop ) override;

	void updateHandlerLoop( uint32_t index, uint32_t generation, EventLoop* loop ) override;

	void invokeDeferred(
		uint32_t index,
		uint32_t generation,
		const std::shared_ptr< void >& args ) override;

	// ---- API called directly by EventStorage/BasicEvent ----------------------

	/**
	 * @brief Place a new connection in a slot: initialise it in place, wire
	 * flags, predicate and counts, and bring it into dispatch order (or, when
	 * a dispatch is in progress, defer ordering to reconcileAfterTrigger).
	 *
	 * @param handler the callable to invoke on dispatch
	 * @param tag the receiver's migration tag (Trackable::migrationTag()), or
	 *   0 for an untracked connection; see HandlerEntry::connectionTag
	 * @param priority user-defined dispatch ordering; 0 is default/unordered
	 * @param declaredType the ConnectionType set at connect() time
	 * @param resolved declaredType already resolved against current loop topology
	 * @param receiverLoop the tracked receiver's EventLoop, or nullptr
	 * @param hasOwner true if bound to a receiver/owner object rather than a free function or lambda (see EventFlags::hasOwner())
	 * @param isSingleShot true if this connection should auto-disconnect after firing once
	 * @param hasPredicate true if `predicate` is non-empty and should be attached
	 * @param predicate the conditional predicate, moved in; ignored if hasPredicate is false
	 * @param receiverContextPredicate  true for PredicateContext::Receiver, false for ::Sender
	 * @param outGen [out] the generation to embed in the resulting Connection
	 * @returns the slot (index) this connection was placed in
	 */
	uint32_t addConnection(
		HandlerType&& handler, uint64_t tag, uint16_t priority,
		ConnectionType declaredType, ResolvedConnectionType resolved,
		EventLoop* receiverLoop, bool hasOwner, bool isSingleShot,
		bool hasPredicate, PredicateType&& predicate, bool receiverContextPredicate,
		uint16_t& outGen );

	/**
	 * @brief Trigger the event: dispatch args to every active handler in
	 * dispatch order (see `order`/priorityConnCount above), each according
	 * to its own resolved connection type.  Called by BasicEvent::operator().
	 *
	 * Two lock strategies, chosen at compile time via HasLockShared_v:
	 *   - SharedMutex (SharedEvent): takes a SharedLock, so concurrent
	 *     triggers from multiple threads can dispatch in parallel; connect/
	 *     disconnect take the exclusive lock and so block until dispatch
	 *     completes.  Reentrant connect/disconnect from within a handler
	 *     invoked this way is not supported (see triggerDepth's @note above).
	 *   - Everything else: takes the exclusive lock for the duration of
	 *     dispatch, tracking triggerDepth/hadReentrantActivity so a handler
	 *     that reentrantly connects or disconnects on this same event is
	 *     handled correctly (see the class-level storage-model doc, and
	 *     triggerDepth/reconcileAfterTrigger below).
	 *
	 * Within each lock strategy, a fast path (nonDirectCount == 0 &&
	 * predicateCount == 0: every active connection is Direct with no
	 * predicate) walks `handlers` and calls each handler directly with no
	 * per-slot branching; otherwise the slow path (dispatchEntry) evaluates
	 * predicates and switches on each slot's resolved connection type
	 * (Direct / Deferred / None).
	 */
	void dispatch( Args... args );

	/**
	 * @brief Disconnect all active handlers without acquiring the mutex.
	 * Caller must hold the exclusive lock.
	 */
	void preLockDisconnectAll();

	/**
	 * @brief Locate the single earliest active handler for which `matches`
	 * returns true, under the exclusive lock, and return it as a one-element
	 * GenData vector (empty if none matched).  Deliberately returns rather
	 * than disconnecting directly: the caller releases this lock before
	 * calling disconnectHandlers(), which re-locks - taking the lock
	 * twice rather than nesting it.  ImplT-agnostic callers therefore
	 * never need to know EventImpl's storage layout at all.
	 */
	template< typename MatchFn >
	std::vector< GenData > findFirstMatch( MatchFn&& matches ) const;

private:
	// ---- internal plumbing - nothing outside EventImpl calls these ----------

	/**
	 * @brief True if `slot` currently holds a live connection matching
	 * `generation` (in range, active, generation matches).
	 */
	bool isSlotLive( uint32_t slot, uint32_t generation ) const;

	/**
	 * @brief Disconnects the connection at `slot`.
	 *
	 * Precondition: caller holds `mutex` and has already confirmed the slot
	 * is live (via slotLive()).
	 *
	 * Releases the handler and predicate, marks the slot inactive, and bumps
	 * its generation so a stale Connection can no longer reach it.  If no
	 * dispatch is in progress, the slot is also returned to `nextFree` and
	 * priority bookkeeping (`priorityConnCount`/`order`) is updated right
	 * away; if a dispatch is in progress, that part is deferred instead (see
	 * triggerDepth) and `hadReentrantActivity` is set so it happens later.
	 *
	 * Shared by disconnectHandler() and disconnectHandlers() so this logic
	 * is written once.
	 */
	void disconnectSlotLocked( uint32_t slot );

	/**
	 * @brief Insert `slot` into `order` at its (priority descending, slot
	 * index) position.  Only called while order is active.
	 */
	void orderInsert( uint32_t slot );

	/**
	 * @brief Remove `slot` from `order` if present.
	 */
	void orderRemove( uint32_t slot );

	/**
	 * @brief Rebuild `order` from all active slots when priorityConnCount is
	 * non-zero, or clear it otherwise.
	 */
	void rebuildOrder();

	/**
	 * @brief Bring a just-added active slot into the ordering scheme: bump
	 * priorityConnCount for a non-default priority and, if that makes order
	 * active, (re)build it, otherwise insert the slot when order is already
	 * active.
	 */
	void activateOrdering( uint32_t slot, uint16_t priority );

	/**
	 * @brief Restore the bookkeeping skipped during a dispatch, once
	 * triggerDepth returns to 0: rescan for the earliest free slot (nextFree)
	 * and recompute priorityConnCount/order from the surviving active
	 * handlers.  Returns immediately when the dispatch made no structural
	 * changes (see hadReentrantActivity).
	 */
	void reconcileAfterTrigger();

	/**
	 * @brief Recompute priorityConnCount from the active handlers and rebuild
	 * `order` to match.  Used after reentrant churn, where incremental
	 * bookkeeping would be error-prone.
	 */
	void recomputeOrdering();
};

// ===========================================================================
// whole-event blocking
// ===========================================================================

template< typename MutexType, typename... Args >
inline bool BasicEventImpl< MutexType, Args... >::isBlocked() const noexcept
{
	if constexpr ( std::is_same_v< MutexType, platform::NullMutex > )
	{
		return blockDepth > 0;
	}
	else
	{
		return blockDepth.load( std::memory_order_relaxed ) > 0;
	}
}

template< typename MutexType, typename... Args >
inline void BasicEventImpl< MutexType, Args... >::block() noexcept
{
	if constexpr ( std::is_same_v< MutexType, platform::NullMutex > )
	{
		++blockDepth;
	}
	else
	{
		blockDepth.fetch_add( 1, std::memory_order_relaxed );
	}
}

template< typename MutexType, typename... Args >
inline void BasicEventImpl< MutexType, Args... >::unblock() noexcept
{
	if constexpr ( std::is_same_v< MutexType, platform::NullMutex > )
	{
		if ( blockDepth > 0 )
		{
			--blockDepth;
		}
	}
	else
	{
		unsigned int current = blockDepth.load( std::memory_order_relaxed );
		while ( current > 0
			&& ! blockDepth.compare_exchange_weak(
				current, current - 1, std::memory_order_relaxed ) )
		{
		}
	}
}

// ===========================================================================
// EventImplBase overrides
// ===========================================================================

template< typename MutexType, typename... Args >
bool BasicEventImpl< MutexType, Args... >::isHandlerConnected( uint32_t index, uint32_t generation ) const
{
	if constexpr ( detail::HasLockShared_v< MutexType > )
	{
		platform::SharedLock< MutexType > lock( mutex );
		return isSlotLive( index, generation );
	}
	else
	{
		platform::LockGuard< MutexType > lock( mutex );
		return isSlotLive( index, generation );
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::disconnectHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< MutexType > lock( mutex );

	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}

	disconnectSlotLocked( index );
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::disconnectHandlers( const std::vector< GenData >& entries )
{
	platform::LockGuard< MutexType > lock( mutex );

	for ( auto [ index, generation ] : entries )
	{
		if ( ! isSlotLive( index, generation ) )
		{
			continue;
		}

		disconnectSlotLocked( index );
	}
}

template< typename MutexType, typename... Args >
bool BasicEventImpl< MutexType, Args... >::isHandlerBlocked( uint32_t index, uint32_t generation ) const
{
	if constexpr ( detail::HasLockShared_v< MutexType > )
	{
		platform::SharedLock< MutexType > lock( mutex );
		if ( ! isSlotLive( index, generation ) )
		{
			return false;
		}
		return handlers[ index ].flags.isBlocked();
	}
	else
	{
		platform::LockGuard< MutexType > lock( mutex );
		if ( ! isSlotLive( index, generation ) )
		{
			return false;
		}
		return handlers[ index ].flags.isBlocked();
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::blockHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< MutexType > lock( mutex );
	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}
	handlers[ index ].flags.setBlocked( true );
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::unblockHandler( uint32_t index, uint32_t generation )
{
	platform::LockGuard< MutexType > lock( mutex );
	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}
	handlers[ index ].flags.setBlocked( false );
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::updateSenderLoop( EventLoop* loop )
{
	platform::LockGuard< MutexType > lock( mutex );
	uint32_t count = 0;
	for ( auto& entry : handlers )
	{
		if ( ! entry.flags.isActive() )
		{
			continue;
		}

		if ( entry.connTypes.declaredConnType() == ConnectionType::Auto )
		{
			entry.connTypes.setResolvedConnType( detail::resolveConnectionType(
				ConnectionType::Auto, loop, entry.receiverLoop ) );
		}

		if ( entry.connTypes.resolvedConnType() != ResolvedConnectionType::Direct )
		{
			++count;
		}
	}
	nonDirectCount = count;
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::updateHandlerLoop( uint32_t index, uint32_t generation, EventLoop* loop )
{
	platform::LockGuard< MutexType > lock( mutex );

	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}

	auto& entry = handlers[ index ];

	// direct connections are not affected by receiver loop changes
	if ( entry.connTypes.declaredConnType() == ConnectionType::Direct )
	{
		return;
	}

	const bool wasNonDirect = ( entry.connTypes.resolvedConnType() != ResolvedConnectionType::Direct );

	entry.receiverLoop = loop;

	EventLoop* senderLoop = owner ? owner->eventLoop() : nullptr;
	entry.connTypes.setResolvedConnType(
		detail::resolveConnectionType( entry.connTypes.declaredConnType(), senderLoop, loop ) );

	const bool isNonDirect = ( entry.connTypes.resolvedConnType() != ResolvedConnectionType::Direct );

	if ( wasNonDirect && ! isNonDirect )
	{
		--nonDirectCount;
	}
	else if ( ! wasNonDirect && isNonDirect )
	{
		++nonDirectCount;
	}
}

// ---------------------------------------------------------------------------
// BasicEventImpl::invokeDeferred and dispatch
// ---------------------------------------------------------------------------

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::invokeDeferred(
	uint32_t index,
	uint32_t generation,
	const std::shared_ptr< void >& args )
{
	platform::LockGuard< MutexType > lock( mutex );

	if ( ! isSlotLive( index, generation ) )
	{
		return;
	}

	uint32_t slot = index;

	// receiver-context predicate: evaluated here on the drain thread
	// using the captured event arguments, letting the receiver filter
	// based on its state at handling time rather than at trigger time
	if ( handlers[ slot ].flags.hasPredicate()
		&& handlers[ slot ].flags.isReceiverContextPredicate()
		&& slot < predicates.size() )
	{
		auto& predicateArgs = *std::static_pointer_cast<
			std::tuple< std::decay_t< Args >... > >( args );

		bool passed = std::apply(
			[ this, slot ]( auto&&... a ) {
				return predicates[ slot ]( std::forward< decltype( a ) >( a )... );
			},
			predicateArgs );

		if ( ! passed )
		{
			return;
		}
	}

	auto& typedArgs = *std::static_pointer_cast<
		std::tuple< std::decay_t< Args >... > >( args );

	const bool once = handlers[ slot ].flags.isSingleShot();

	std::apply(
		[ this, slot ]( auto&&... a ) {
			handlers[ slot ].handler( std::forward< decltype( a ) >( a )... );
		},
		typedArgs );

	// disconnect after handler returns; re-entrant under RecursiveMutex/NullMutex
	if ( once )
	{
		disconnectHandler( index, generation );
	}
}

template< typename MutexType, typename... Args >
uint32_t BasicEventImpl< MutexType, Args... >::addConnection(
	HandlerType&& handler, uint64_t tag, uint16_t priority,
	ConnectionType declaredType, ResolvedConnectionType resolved,
	EventLoop* receiverLoop, bool hasOwner, bool isSingleShot,
	bool hasPredicate, PredicateType&& predicate, bool receiverContextPredicate,
	uint16_t& outGen )
{
	const bool predicatesActive = hasPredicate || predicateCount > 0;

	uint32_t slot;
	if ( triggerDepth > 0 )
	{
		// reached only by the same thread that is still inside dispatch()'s
		// call stack (see the note on triggerDepth) - append rather than
		// reuse a free slot, since the walk has already snapshotted
		// handlers.size() and would never visit an appended slot in the
		// ongoing pass; dfer ordering to reconcileAfterTrigger(); modifying
		// order here would corrupt an in-progress range iteration over it
		slot = static_cast< uint32_t >( handlers.size() );
		handlers.emplace_back();
		if ( predicatesActive && predicates.size() < handlers.size() )
		{
			predicates.resize( handlers.size() );
		}
		hadReentrantActivity = true;
	}
	else if ( nextFree < static_cast< uint32_t >( handlers.size() ) )
	{
		slot = nextFree;

		// advance nextFree to the next free slot beyond the one just taken
		uint32_t next = static_cast< uint32_t >( handlers.size() );
		for ( uint32_t i = nextFree + 1; i < static_cast< uint32_t >( handlers.size() ); ++i )
		{
			if ( ! handlers[ i ].flags.isActive() )
			{
				next = i;
				break;
			}
		}
		nextFree = next;
	}
	else
	{
		slot = static_cast< uint32_t >( handlers.size() );
		handlers.emplace_back();
		if ( predicatesActive && predicates.size() < handlers.size() )
		{
			predicates.resize( handlers.size() );
		}
		nextFree = static_cast< uint32_t >( handlers.size() );
	}

	HandlerEntryType& entry = handlers[ slot ];
	entry.init( std::move( handler ), tag, priority, declaredType, resolved, receiverLoop );
	entry.flags.setIsSingleShot( isSingleShot );
	entry.flags.setHasOwner( hasOwner );
	outGen = entry.generation;

	if ( hasPredicate )
	{
		predicates[ slot ] = std::move( predicate );
		entry.flags.setHasPredicate( true );
		entry.flags.setReceiverContextPredicate( receiverContextPredicate );
		++predicateCount;
	}

	if ( resolved != ResolvedConnectionType::Direct )
	{
		++nonDirectCount;
	}

	// ordering deferred when reentrant; reconcileAfterTrigger rebuilds from
	// scratch so no incremental update is needed here in that case
	if ( triggerDepth == 0 )
	{
		activateOrdering( slot, priority );
	}

	return slot;
}

// TODO: break this up into smaller methods
template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::dispatch( Args... args )
{
	using Task = EventLoop::Task;
	using ArgsCapture = std::tuple< std::decay_t< Args >... >;

	// capturedArgs, ensureCapture, and dispatchEntry are shared between the
	// SharedLock and exclusive slow paths; defined here once to avoid
	// duplication - both slow paths hold an exclusive lock by the time they
	// use these, so the captures are safe.
	std::shared_ptr< void > capturedArgs;

	auto ensureCapture = [ & ] {
		if ( ! capturedArgs )
		{
			capturedArgs = std::make_shared< ArgsCapture >( args... );
		}
	};

	// dispatchEntry never holds a persistent reference into `handlers`
	// across the predicate or handler call - each is a user callback that
	// may reentrantly connect(), and a reentrant connect can push_back
	// (reallocating the vector) even though it defers the *sorted*
	// placement; re-indexing via `slot` after such a call is always safe,
	// holding a stale reference across it is not.
	auto dispatchEntry = [ & ]( uint32_t slot ) {
		if ( ! handlers[ slot ].flags.isDispatchReady() )
		{
			return;
		}

		// sender-context predicate: evaluated here for both Direct and Deferred;
		// receiver-context predicates on Deferred connections are re-evaluated
		// in invokeDeferred() on the drain side
		if ( predicateCount > 0 && handlers[ slot ].flags.hasPredicate() )
		{
			const bool isDirect = ( handlers[ slot ].connTypes.resolvedConnType() == ResolvedConnectionType::Direct );
			if ( ( isDirect || ! handlers[ slot ].flags.isReceiverContextPredicate() )
				&& slot < predicates.size()
				&& ! predicates[ slot ]( args... ) )
			{
				return;
			}
		}

		switch ( handlers[ slot ].connTypes.resolvedConnType() )
		{
		case ResolvedConnectionType::Direct:
		{
			// the predicate above may have reentrantly disconnected or
			// blocked this very entry (directly, or via disconnectAll())
			// under the recursive mutex - re-check before invoking; a
			// reentrant disconnect only flips isActive() while
			// triggerDepth > 0 (the slot's reuse is deferred), so `slot`
			// still safely refers to the same logical entry here;
			// TODO: possible single-shot fast path area
			if ( ! handlers[ slot ].flags.isDispatchReady() )
			{
				break;
			}

			const bool once = handlers[ slot ].flags.isSingleShot();
			const uint32_t gen = handlers[ slot ].generation;
			handlers[ slot ].handler( args... );
			if ( once )
			{
				disconnectHandler( slot, gen );
			}
			break;
		}

		case ResolvedConnectionType::Deferred:
		{
			ensureCapture();
			const auto w = weakSelf;
			const uint32_t ch = slot;
			const uint32_t cg = handlers[ slot ].generation;
			handlers[ slot ].receiverLoop->post( Task::create(
				[ w, ch, cg, capturedArgs ]() {
					if ( auto impl = w.lock() )
					{
						impl->invokeDeferred( ch, cg, capturedArgs );
					}
				} ), handlers[ slot ].connectionTag );
			break;
		}

		case ResolvedConnectionType::None:
			break;
		}
	};

	// walk active slots in dispatch order - when `order` is populated (a
	// non-default priority is in use) it is the dispatch order and is never
	// mutated mid-dispatch, so range iteration stays valid across reentrant
	// callbacks; otherwise slots are walked in index order, with the count
	// snapshotted up front so that a slot a reentrant connect appends past
	// that count is not visited until the next dispatch
	auto walkActive = [ & ]( auto&& perSlot ) {
		if ( ! order.empty() )
		{
			for ( uint32_t slot : order )
			{
				perSlot( slot );
			}
		}
		else
		{
			const uint32_t cnt = static_cast< uint32_t >( handlers.size() );
			for ( uint32_t i = 0; i < cnt; ++i )
			{
				perSlot( i );
			}
		}
	};

	if constexpr ( detail::HasLockShared_v< MutexType > )
	{
		platform::SharedLock< MutexType > lock( mutex );

		if ( nonDirectCount == 0 && predicateCount == 0 )
		{
			// shared fast path: all Direct
			walkActive( [ & ]( uint32_t slot ) {
					if ( handlers[ slot ].flags.isDispatchReady() )
					{
						handlers[ slot ].handler( args... );
					}
				} );
		}
		else
		{
			// shared slow path: Deferred/None or predicates present
			walkActive( [ & ]( uint32_t slot ) { dispatchEntry( slot ); } );
		}
	}
	else
	{
		platform::LockGuard< MutexType > lock( mutex );

		triggerDepth++;

		// TODO: consider gating on singleShotCount == 0 for an additional
		// fast-path improvement - mirroring nonDirectCount/predicateCount -
		// to skip the isSingleShot/generation reads here when no once-connections
		// exist (estimated low single-digit % at large N, but unmeasured)
		if ( nonDirectCount == 0 && predicateCount == 0 )
		{
			// exclusive fast path: all Direct - single-shot entries are
			// disconnected after firing
			// TODO: possible single-shot fast path area
			walkActive( [ & ]( uint32_t slot ) {
					if ( ! handlers[ slot ].flags.isDispatchReady() )
					{
						return;
					}

					const bool once = handlers[ slot ].flags.isSingleShot();
					const uint32_t gen = handlers[ slot ].generation;
					handlers[ slot ].handler( args... );

					if ( once )
					{
						disconnectHandler( slot, gen );
					}
				} );
		}
		else
		{
			// exclusive slow path: Deferred/None or predicates present
			// TODO: possible single-shot fast path area
			walkActive( [ & ]( uint32_t slot ) { dispatchEntry( slot ); } );
		}

		triggerDepth--;
		if ( triggerDepth == 0 )
		{
			reconcileAfterTrigger();
		}
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::preLockDisconnectAll()
{
	if ( triggerDepth == 0 )
	{
		// not reentrant - safe to fully wipe immediately
		handlers.clear();
		order.clear();
		predicates.clear();
		priorityConnCount = 0;
		nonDirectCount = 0;
		predicateCount = 0;
		nextFree = 0;
		hadReentrantActivity = false;
	}
	else
	{
		// reentrant (e.g. disconnectAll() called from within a handler) -
		// mark everything inactive and bump generations now so isConnected()
		// is synchronously false; reconcileAfterTrigger() rebuilds the rest
		for ( uint32_t slot = 0; slot < static_cast< uint32_t >( handlers.size() ); ++slot )
		{
			if ( ! handlers[ slot ].flags.isActive() )
			{
				continue;
			}

			handlers[ slot ].handler = HandlerType{};
			if ( slot < predicates.size() )
			{
				predicates[ slot ] = {};
			}

			// zero all flags (active, hasPredicate, isBlocked, isSingleShot,
			// hasOwner, isReceiverContextPredicate, ...) so no stale state
			// leaks into the next connection that reuses this slot - matching
			// disconnectSlotLocked's reset below
			handlers[ slot ].flags = EventFlags{};
			++handlers[ slot ].generation;
		}
		hadReentrantActivity = true;
	}

	nonDirectCount = 0;
	predicateCount = 0;
}

template< typename MutexType, typename... Args >
template< typename MatchFn >
std::vector< GenData > BasicEventImpl< MutexType, Args... >::findFirstMatch( MatchFn&& matches ) const
{
	std::vector< GenData > result;
	platform::LockGuard< MutexType > lock( mutex );
	for ( uint32_t i = 0; i < static_cast< uint32_t >( handlers.size() ); ++i )
	{
		if ( handlers[ i ].flags.isActive() && matches( handlers[ i ].handler ) )
		{
			result.emplace_back( i, handlers[ i ].generation );
			break;  // one match only; duplicates left in place
		}
	}
	return result;
}

// ===========================================================================
// internal plumbing
// ===========================================================================

template< typename MutexType, typename... Args >
bool BasicEventImpl< MutexType, Args... >::isSlotLive( uint32_t slot, uint32_t generation ) const
{
	return slot < static_cast< uint32_t >( handlers.size() )
		&& handlers[ slot ].generation == generation
		&& handlers[ slot ].flags.isActive();
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::disconnectSlotLocked( uint32_t slot )
{
	HandlerEntryType& entry = handlers[ slot ];

	if ( entry.connTypes.resolvedConnType() != ResolvedConnectionType::Direct )
	{
		--nonDirectCount;
	}

	if ( entry.flags.hasPredicate() )
	{
		if ( slot < predicates.size() )
		{
			PredicateType released = std::move( predicates[ slot ] );
		}
		entry.flags.setHasPredicate( false );
		--predicateCount;
	}

	entry.flags.setActive( false );
	{
		HandlerType released = std::move( entry.handler );
	}

	// zero all flags so no stale state (isBlocked, isSingleShot, hasOwner, etc.)
	// leaks into the next connection that reuses this slot
	entry.flags = EventFlags{};
	++entry.generation;

	if ( triggerDepth > 0 )
	{
		// defer nextFree update and ordering changes — modifying order while
		// dispatch is iterating it would corrupt the walk;
		// the isDispatchReady() check in the dispatch loop skips this now-
		// inactive slot for the remainder of the current pass
		hadReentrantActivity = true;
		return;
	}

	if ( slot < nextFree )
	{
		nextFree = slot;
	}

	if ( entry.priority != 0 && priorityConnCount > 0 )
	{
		--priorityConnCount;
	}

	if ( priorityConnCount == 0 )
	{
		order.clear();
	}
	else if ( ! order.empty() )
	{
		orderRemove( slot );
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::orderInsert( uint32_t slot )
{
	const uint16_t p = handlers[ slot ].priority;
	auto it = std::partition_point( order.begin(), order.end(),
		[ this, p, slot ]( uint32_t s ) {
			return handlers[ s ].priority > p
				|| ( handlers[ s ].priority == p && s < slot );
		} );
	order.insert( it, slot );
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::orderRemove( uint32_t slot )
{
	auto it = std::find( order.begin(), order.end(), slot );
	if ( it != order.end() )
	{
		order.erase( it );
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::rebuildOrder()
{
	order.clear();
	if ( priorityConnCount == 0 )
	{
		return;
	}

	for ( uint32_t s = 0; s < static_cast< uint32_t >( handlers.size() ); ++s )
	{
		if ( handlers[ s ].flags.isActive() )
		{
			order.push_back( s );
		}
	}

	// stable_sort by priority descending; pushed in slot order, so ties keep
	// slot-index order within a priority level
	std::stable_sort( order.begin(), order.end(),
		[ this ]( uint32_t a, uint32_t b ) {
			return handlers[ a ].priority > handlers[ b ].priority;
		} );
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::activateOrdering( uint32_t slot, uint16_t priority )
{
	const bool wasActive = ( priorityConnCount > 0 );
	if ( priority != 0 )
	{
		++priorityConnCount;
	}
	const bool nowActive = ( priorityConnCount > 0 );

	if ( ! wasActive && nowActive )
	{
		rebuildOrder();
	}
	else if ( nowActive )
	{
		orderInsert( slot );
	}
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::reconcileAfterTrigger()
{
	if ( ! hadReentrantActivity )
	{
		return;
	}

	hadReentrantActivity = false;

	// rescan for the new earliest free slot
	nextFree = static_cast< uint32_t >( handlers.size() );
	for ( uint32_t i = 0; i < static_cast< uint32_t >( handlers.size() ); ++i )
	{
		if ( ! handlers[ i ].flags.isActive() )
		{
			nextFree = i;
			break;
		}
	}

	// reentrant connects and disconnects may have both added and removed
	// connections; rebuild priority ordering from scratch rather than
	// attempting incremental bookkeeping across the two deferred sets
	recomputeOrdering();
}

template< typename MutexType, typename... Args >
void BasicEventImpl< MutexType, Args... >::recomputeOrdering()
{
	priorityConnCount = 0;
	for ( uint32_t s = 0; s < static_cast< uint32_t >( handlers.size() ); ++s )
	{
		if ( handlers[ s ].flags.isActive() && handlers[ s ].priority != 0 )
		{
			++priorityConnCount;
		}
	}
	rebuildOrder();
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_EVENT_IMPL_H
