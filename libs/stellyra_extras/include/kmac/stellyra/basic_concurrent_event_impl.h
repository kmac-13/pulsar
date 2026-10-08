#pragma once
#ifndef KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_IMPL_H
#define KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_IMPL_H

/**
 * @file basic_concurrent_event_impl.h
 * @brief BasicConcurrentEventImpl<MutexType, Args...> - the primary, EventStorage-
 * compatible lock-free-dispatch implementation behind BasicConcurrentEvent.
 *
 * Each slot is a std::atomic<Entry*>, reclaimed through EpochDomain rather
 * than protected by a lock, extended with everything EventStorage's
 * generalised connect/disconnect machinery (see event_storage.h) needs from
 * an ImplT:
 *
 *   - MutexType mutex, Trackable* owner, WeakPtr<EventImplBase> weakSelf
 *   - addConnection(...) with the exact signature EventImpl<MutexType,Args...>
 *     uses, so EventStorage::connectImpl() cannot tell the two apart
 *   - findFirstMatch(...) for by-target disconnect<Method>/disconnectFree/
 *     disconnect(receiver, method)
 *   - preLockDisconnectAll() for disconnectAll()
 *   - block()/unblock()/isBlocked() for EventStorage::block()/unblock()/
 *     blockGuard()
 *   - priority-ordered dispatch and predicate-gated dispatch, matching
 *     EventImpl's semantics for Direct connections (predicate context,
 *     Sender vs Receiver, is meaningless for a Direct-only dispatcher - see
 *     connection_type.h: "for Direct connections both contexts behave
 *     identically" - so both are evaluated synchronously in dispatch(),
 *     with the flag only carried forward for when EventLoop integration
 *     gives it meaning)
 *
 * Scope: Direct dispatch, Deferred (receiver-deferred) dispatch, and
 * Auto re-resolution when the owner's or a receiver's EventLoop changes
 * after connect() time (updateSenderLoop/updateHandlerLoop, both real now -
 * see their docs below for the atomic-fields-not-whole-Entry-replacement
 * design this needed) are all supported. Sender-deferred (marshalling the
 * *trigger* call itself onto the owner's home thread) lives one layer up,
 * in BasicConcurrentEvent::operator() (basic_concurrent_event.h) - entirely
 * orthogonal to anything here, and needed no change to this file at all.
 *
 * Receiver-deferred design note (see invokeDeferred() below for the full
 * argument): a deferred invocation needs "the original Entry, independent
 * of its slot," but that doesn't actually require giving Entry its own
 * reference-counted lifetime. A deferred task instead carries only (index,
 * generation), checked against whatever currently occupies that slot at
 * *invocation* time, under a fresh epoch guard - exactly
 * EventImpl::invokeDeferred's own contract (event_impl.h): if the original
 * connection was disconnected, or the slot was reused by something else
 * entirely in the interval, the generation simply won't match and the task
 * is a silent no-op, the same "it's just gone from the list by then"
 * reasoning already used for sender-deferred. No separate lifetime needed -
 * the epoch guard that already protects every other read in this file
 * protects this one too.
 *
 * Locking model: dispatch() never touches `mutex` - it is pure epoch
 * enter/exit. `mutex` guards connect/disconnect/priority-order rebuilds
 * only, so any MutexType works correctly here, including a genuinely
 * non-recursive one: unlike EventImpl, a handler that reentrantly
 * connects/disconnects from within its own Direct dispatch is not
 * re-entering a lock it already holds, because dispatch was never holding
 * `mutex` in the first place. This is the same recursive-mutex-not-required
 * conclusion documented for Event::_mutex (see platform.h's RecursiveMutex
 * docs), and it applies even more directly here, since dispatch() never
 * takes `mutex` at all.
 *
 * Priority ordering: maintained as a single atomically-published
 * OrderSnapshot (a plain std::vector<uint32_t>, sorted priority-descending/
 * slot-index-ascending, exactly matching EventImpl::rebuildOrder's
 * semantics), reclaimed through the same EpochDomain that already reclaims
 * Entry objects and the slot array's backing storage - one dispatch()
 * enter() call protects all three. Rebuilt from scratch on every connect/
 * disconnect while any non-zero-priority connection exists, rather than
 * EventImpl's incremental orderInsert()/orderRemove() - simpler, and
 * correct by construction; worth revisiting only if profiling ever shows
 * per-mutation O(n) rebuild cost actually matters for a workload with many
 * priority connections churning at runtime (expected rare - most call
 * sites use default priority 0, where `order` stays null and dispatch
 * takes the plain slot-order walk, unaffected by any of this).
 */

#include "epoch_domain.h"

#include <kmac/stellyra/callable.h>
#include <kmac/stellyra/connection_type.h>
#include <kmac/stellyra/event_detail.h>
#include <kmac/stellyra/event_impl_base.h>
#include <kmac/stellyra/event_loop.h>
#include <kmac/stellyra/gen_data.h>
#include <kmac/stellyra/platform.h>
#include <kmac/stellyra/stellyra_assert.h>
#include <kmac/stellyra/trackable.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace kmac {
namespace stellyra {

template< typename MutexType, typename... Args >
class BasicConcurrentEventImpl : public EventImplBase
{
public:
	using HandlerType = Callable< void ( Args... ) >;
	using PredicateType = Callable< bool ( Args... ) >;

private:
	/**
	 * @brief One handler, exclusively owned by whichever slot currently
	 * points to it, reclaimed through the same EpochDomain that protects
	 * every other atomic read in this file (see epoch_domain.h). Extended
	 * with the fields
	 * EventStorage's generalised connect surface needs: connectionTag
	 * (Trackable migration tag), priority, and a predicate.
	 *
	 * Every field is immutable after construction except `blocked`,
	 * `resolvedType`, and `receiverLoop` - all three of which get mutated
	 * post-construction (block()/unblock() the first, updateSenderLoop()/
	 * updateHandlerLoop() the other two) while a concurrent dispatch() may
	 * be reading this same Entry with no lock at all. Callable is
	 * deliberately not one of the three: it is move-only (see callable.h),
	 * so an in-place "replace this Entry's callable" is not something that
	 * could ever be done data-race-free without a second synchronisation
	 * mechanism beyond the epoch guard that already protects the Entry
	 * itself from deallocation - and nothing here needs to replace it.
	 * `resolvedType`/`receiverLoop` are small, trivially-copyable, and nothing
	 * else in Entry needs to change post-construction, so plain atomics are
	 * the whole fix: writers (holding `mutex`, serialised against each
	 * other and against connect/disconnect) store; dispatch() loads. This
	 * means BasicConcurrentEvent's re-resolution guarantee is weaker than BasicEvent's:
	 * BasicEvent's updateSenderLoop()/updateHandlerLoop() run under the
	 * exact same mutex dispatch() holds, so an in-flight dispatch() never
	 * observes a topology change mid-walk. Here dispatch() never takes
	 * `mutex` at all, so a concurrent updateSenderLoop()/updateHandlerLoop()
	 * can genuinely race a live dispatch() - each individual entry's
	 * resolvedType is read atomically (never torn), but which entries in
	 * one dispatch() walk see the old vs. new topology is not
	 * deterministic. Eventually consistent, not linearizable against
	 * dispatch() - a deliberate, documented trade for lock-free dispatch,
	 * not an oversight.
	 */
	struct Entry
	{
		HandlerType callable;
		PredicateType predicate;
		uint64_t connectionTag = 0;
		uint16_t generation;
		uint16_t priority = 0;
		ConnectionType declaredType = ConnectionType::Direct;
		bool singleShot;
		bool hasPredicate = false;
		bool isReceiverContextPredicate = false;
		std::atomic< ResolvedConnectionType > resolvedType{ ResolvedConnectionType::Direct };
		std::atomic< EventLoop* > receiverLoop{ nullptr };
		std::atomic< bool > blocked{ false };

		Entry(
			HandlerType c, uint64_t tag, uint16_t prio, uint16_t gen, bool ss,
			bool hasPred, PredicateType pred, bool receiverCtxPred, EventLoop* loop,
			ConnectionType declared, ResolvedConnectionType resolved )
			: callable( std::move( c ) )
			, predicate( std::move( pred ) )
			, connectionTag( tag )
			, generation( gen )
			, priority( prio )
			, declaredType( declared )
			, singleShot( ss )
			, hasPredicate( hasPred )
			, isReceiverContextPredicate( receiverCtxPred )
			, resolvedType( resolved )
			, receiverLoop( loop )
		{
		}
	};

	using SlotArray = std::atomic< Entry* >;

	/**
	 * @brief One published priority-dispatch-order snapshot - see file docs.
	 * Immutable once published: a writer that needs to change the order
	 * builds a whole new OrderSnapshot and swaps the published pointer,
	 * never mutates one already visible to readers.
	 */
	struct OrderSnapshot
	{
		std::vector< uint32_t > order;
	};

	std::atomic< SlotArray* > _slotsPtr;
	size_t _capacity;                        // writer-only, under `mutex`
	std::vector< uint16_t > _nextGeneration;  // writer-only, under `mutex`
	std::atomic< uint32_t > _usedCount{ 0 };
	std::atomic< OrderSnapshot* > _orderSnapshot{ nullptr };
	uint32_t _priorityConnCount = 0;          // writer-only, under `mutex`
	EpochDomain _epoch;

public:
	/// Initial slot array size; doubles on demand when addConnection() runs out of room (see growSlots() below).
	static constexpr size_t INITIAL_CAPACITY = 8;

	/// see EventStorage's constructors - set directly, matching EventImpl's owner/weakSelf fields
	Trackable* owner = nullptr;
	platform::WeakPtr< EventImplBase > weakSelf;

	/// guards connect/disconnect/priority-order rebuilds only - never touched by dispatch()
	mutable MutexType mutex;

	BasicConcurrentEventImpl();

	~BasicConcurrentEventImpl() override;

	BasicConcurrentEventImpl( const BasicConcurrentEventImpl& ) = delete;
	BasicConcurrentEventImpl& operator=( const BasicConcurrentEventImpl& ) = delete;

	// ---- whole-event blocking (mirrors EventImpl - see event.h/event_impl.h) --------

	using BlockDepthType = std::conditional_t<
		std::is_same_v< MutexType, platform::NullMutex >,
		unsigned int,
		platform::Atomic< unsigned int > >;

	BlockDepthType blockDepth{ 0 };

	bool isBlocked() const noexcept;

	void block() noexcept;

	void unblock() noexcept;

	// ---- API called directly by EventStorage --------------------------------------

	/**
	 * @brief Place a new connection. Signature matches
	 * EventImpl::addConnection exactly - EventStorage::connectImpl() calls
	 * this generically through ImplT and cannot tell the two apart.
	 *
	 * @note Caller (EventStorage::connectImpl(), event_storage.h) already
	 * holds `mutex` for the whole call - this does NOT lock internally,
	 * exactly matching EventImpl::addConnection's contract (event_impl.h).
	 * This was a real double-lock bug for several revisions: an internal
	 * LockGuard here, stacked under connectImpl's own already-held lock,
	 * deadlocks with any genuinely non-recursive MutexType - silently
	 * masked by RecursiveMutex (the default used throughout this session's
	 * testing), which permits the same thread to lock it twice. Caught only
	 * when testing against a plain mutex specifically - see the git history/
	 * conversation for how. TSan cannot catch a successful recursive
	 * relock; it isn't a race, just wrong.
	 *
	 * @note `hasOwner` is accepted for interface parity but not yet stored
	 * anywhere: it only matters for BasicEvent-style introspection
	 * (EventInspector), which does not yet support BasicConcurrentEvent - a real gap,
	 * not lost information (nothing reads it, nothing depends on it being
	 * right, yet).
	 */
	uint32_t addConnection(
		HandlerType&& handler, uint64_t tag, uint16_t priority,
		ConnectionType declaredType, ResolvedConnectionType resolved,
		EventLoop* receiverLoop, bool hasOwner, bool isSingleShot,
		bool hasPredicate, PredicateType&& predicate, bool receiverContextPredicate,
		uint16_t& outGen );

	/**
	 * @brief Dispatch args to every active, unblocked, predicate-passing
	 * handler, in priority order if any non-default priority is in use.
	 * One epoch enter() for the whole call, then plain atomic loads.
	 *
	 * Direct connections fire synchronously here. Deferred connections get
	 * a task posted to their receiver's EventLoop instead - see
	 * invokeDeferred() below for the other half. None-resolved connections
	 * are silently skipped (matches EventImpl/connection_type.h: a valid,
	 * live connection whose current loop topology just doesn't support
	 * either Direct or Deferred dispatch right now).
	 *
	 * Argument capture for Deferred posting is lazy and shared across every
	 * Deferred entry in this one trigger - see ensureCapture() - exactly
	 * mirroring EventImpl::dispatch()'s capturedArgs.
	 */
	void dispatch( Args... args );

	/**
	 * @brief Disconnect all active handlers. Caller must already hold `mutex`
	 * - same contract as EventImpl::preLockDisconnectAll().
	 */
	void preLockDisconnectAll();

	/**
	 * @brief Locate the single earliest active handler for which `matches`
	 * returns true, under `mutex`, returned as a one-element GenData vector
	 * (empty if none matched) - see EventImpl::findFirstMatch's docs
	 * (event_impl.h), which this mirrors exactly for EventStorage's benefit.
	 */
	template< typename MatchFn >
	std::vector< GenData > findFirstMatch( MatchFn&& matches ) const;

	// ---- EventImplBase overrides -------------------------------------------

	bool isHandlerConnected( uint32_t index, uint32_t generation ) const override;

	void disconnectHandler( uint32_t index, uint32_t generation ) override;

	/**
	 * @brief Disconnect every still-live entry in `entries`. O(1) per entry
	 * for the slot itself; if priority ordering is (or was) active, the
	 * published order snapshot is rebuilt once for the whole batch.
	 */
	void disconnectHandlers( const std::vector< GenData >& entries ) override;

	bool isHandlerBlocked( uint32_t index, uint32_t generation ) const override;

	void blockHandler( uint32_t index, uint32_t generation ) override;

	void unblockHandler( uint32_t index, uint32_t generation ) override;

	/**
	 * @brief Re-resolve every active Auto connection against the owner's
	 * new sender EventLoop. Called by Trackable::setEventLoop() (via
	 * EventImplBase) when this event's owner's loop changes - see
	 * event_storage.h's constructors for how `owner` gets wired up, and
	 * Trackable::setEventLoop() (trackable.h) for the caller.
	 *
	 * Direct- and Deferred-declared connections are untouched: Direct
	 * always resolves Direct regardless of loop topology, and Deferred's
	 * resolution depends only on the receiver's loop, never the sender's -
	 * see resolveConnectionType() (event_detail.h). Only mutates the two
	 * atomic fields on each affected Entry - see Entry's docs above for why
	 * that's sufficient and safe against a concurrent dispatch().
	 */
	void updateSenderLoop( EventLoop* loop ) override;

	/**
	 * @brief Update one connection's receiver EventLoop and re-resolve it
	 * if declared Auto (or if it's Deferred and just lost/gained a loop).
	 * Called by Trackable::setEventLoop() for every connection where this
	 * event's owning Trackable is the receiver - see trackable.h.
	 *
	 * A stale (index, generation) - already disconnected, or the slot
	 * reused by an unrelated later connection - is a silent no-op, same
	 * generation-check discipline as every other per-connection entry
	 * point in this file (disconnectHandler, blockHandler, invokeDeferred).
	 */
	void updateHandlerLoop( uint32_t index, uint32_t generation, EventLoop* loop ) override;

	/**
	 * @brief Execute one specific Deferred handler, posted earlier by
	 * dispatch(), on whichever thread actually drains its receiverLoop.
	 *
	 * Takes a fresh epoch guard - dispatch()'s original guard is long gone
	 * by the time this runs, possibly on a different thread entirely.
	 * `generation` is checked against the slot's *current* occupant before
	 * touching anything: if the connection was disconnected in the interval
	 * (slot now null) or the slot was reused by an unrelated later
	 * connection (an Entry is present but its generation differs), this is
	 * a silent no-op - exactly EventImpl::invokeDeferred's contract
	 * (event_impl.h), and exactly what makes this safe without needing the
	 * original Entry to outlive its slot: nothing here depends on holding
	 * onto that specific Entry across the interval. Whatever currently
	 * occupies the slot is either genuinely still the same connection
	 * (generation matches - the only case that proceeds) or is provably a
	 * different one (generation differs - skipped), and either way it's
	 * memory-safe to read via the epoch guard, the same guard already
	 * protecting isHandlerConnected()/isHandlerBlocked()/setBlocked() above.
	 */
	void invokeDeferred( uint32_t index, uint32_t generation, const std::shared_ptr< void >& args ) override;

private:
	static SlotArray* allocateSlotArray( size_t capacity );

	/**
	 * @brief Called under `mutex`. Doubles the slot array, publishes it, retires the old one.
	 */
	SlotArray* growSlots();

	/**
	 * @brief Rebuild and publish the priority-order snapshot from every
	 * currently active slot, or publish null (order inactive) if no
	 * non-zero-priority connection remains. Called under `mutex`.
	 */
	void rebuildOrderLocked();

	/**
	 * @brief Called under `mutex`. Publishes `snap` (possibly null) and retires whatever was published before.
	 */
	void publishOrder( OrderSnapshot* snap );

	/**
	 * @brief Set the blocked flag on a still-live entry. Takes an epoch
	 * guard rather than `mutex`: dispatch() reads `blocked` through its own
	 * epoch guard without ever taking `mutex` (see Entry's docs above), so
	 * entering the epoch here - not locking `mutex` - is what makes this
	 * store visible-and-safe against that concurrent lock-free read.
	 */
	void setBlocked( uint32_t index, uint32_t generation, bool blocked );
};

template< typename MutexType, typename... Args >
inline BasicConcurrentEventImpl< MutexType, Args... >::BasicConcurrentEventImpl()
	: _capacity( INITIAL_CAPACITY )
{
	_slotsPtr.store( allocateSlotArray( _capacity ), std::memory_order_relaxed );
}

template< typename MutexType, typename... Args >
inline BasicConcurrentEventImpl< MutexType, Args... >::~BasicConcurrentEventImpl()
{
	// no dispatch() can be in flight once the owning event is being
	// destroyed; safe to free live entries, the slot array, and any
	// published order snapshot directly rather than through retire()
	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t count = _usedCount.load( std::memory_order_relaxed );
	for ( uint32_t i = 0; i < count; ++i )
	{
		delete slots[ i ].load( std::memory_order_relaxed );
	}
	delete[] slots;
	delete _orderSnapshot.load( std::memory_order_relaxed );
}

template< typename MutexType, typename... Args >
inline bool BasicConcurrentEventImpl< MutexType, Args... >::isBlocked() const noexcept
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
inline void BasicConcurrentEventImpl< MutexType, Args... >::block() noexcept
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
inline void BasicConcurrentEventImpl< MutexType, Args... >::unblock() noexcept
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
			&& ! blockDepth.compare_exchange_weak( current, current - 1, std::memory_order_relaxed ) )
		{
		}
	}
}

template< typename MutexType, typename... Args >
inline uint32_t BasicConcurrentEventImpl< MutexType, Args... >::addConnection(
	HandlerType&& handler, uint64_t tag, uint16_t priority,
	ConnectionType declaredType, ResolvedConnectionType resolved,
	EventLoop* receiverLoop, bool hasOwner, bool isSingleShot,
	bool hasPredicate, PredicateType&& predicate, bool receiverContextPredicate,
	uint16_t& outGen )
{
	(void) hasOwner;

	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );

	uint32_t index = used;
	for ( uint32_t i = 0; i < used; ++i )
	{
		if ( slots[ i ].load( std::memory_order_relaxed ) == nullptr )
		{
			index = i;
			break;
		}
	}

	if ( index == used )
	{
		if ( used >= _capacity )
		{
			slots = growSlots();
		}
		_nextGeneration.push_back( 0 );
		// publish the (possibly grown) array BEFORE the count that requires
		// it: dispatch() acquire-loads _usedCount and then acquire-loads
		// _slotsPtr (see dispatch() below), so any dispatch() that observes
		// the new count is guaranteed - via that acquire/release pair on
		// _usedCount - to also observe the new array, never the stale,
		// too-small one
		_usedCount.store( used + 1, std::memory_order_release );
	}

	const uint16_t generation = _nextGeneration[ index ];
	Entry* entry = new Entry(
		std::move( handler ), tag, priority, generation, isSingleShot,
		hasPredicate, std::move( predicate ), receiverContextPredicate, receiverLoop,
		declaredType, resolved );
	slots[ index ].store( entry, std::memory_order_release );
	outGen = generation;

	if ( priority != 0 )
	{
		++_priorityConnCount;
	}
	if ( _priorityConnCount > 0 )
	{
		rebuildOrderLocked();
	}

	return index;
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::dispatch( Args... args )
{
	auto guard = _epoch.enter();

	const uint32_t count = _usedCount.load( std::memory_order_acquire );
	SlotArray* slots = _slotsPtr.load( std::memory_order_acquire );
	OrderSnapshot* order = _orderSnapshot.load( std::memory_order_acquire );

	std::vector< GenData > toRetire;
	std::shared_ptr< std::tuple< std::decay_t< Args >... > > capturedArgs;

	auto ensureCapture = [ & ]() {
		if ( ! capturedArgs )
		{
			capturedArgs = std::make_shared< std::tuple< std::decay_t< Args >... > >( args... );
		}
	};

	auto dispatchSlot = [ & ]( uint32_t i ) {
		Entry* entry = slots[ i ].load( std::memory_order_acquire );
		if ( entry == nullptr || entry->blocked.load( std::memory_order_relaxed ) )
		{
			return;
		}

		switch ( entry->resolvedType.load( std::memory_order_acquire ) )
		{
		case ResolvedConnectionType::Direct:
		{
			// both predicate contexts are evaluated identically here -
			// there is no "later" to defer a receiver-context predicate
			// to on a Direct connection (see connection_type.h)
			if ( entry->hasPredicate && ! entry->predicate( args... ) )
			{
				return;
			}

			entry->callable( args... );

			if ( entry->singleShot )
			{
				toRetire.emplace_back( i, entry->generation );
			}
			break;
		}

		case ResolvedConnectionType::Deferred:
		{
			// sender-context predicate: evaluated here, before posting;
			// a false result suppresses the post entirely, so the
			// receiver's EventLoop never sees the task. Receiver-context
			// predicates are re-evaluated in invokeDeferred() on the
			// receiving loop's drain instead - see connection_type.h's
			// PredicateContext docs.
			if ( entry->hasPredicate
				&& ! entry->isReceiverContextPredicate
				&& ! entry->predicate( args... ) )
			{
				return;
			}

			ensureCapture();
			platform::WeakPtr< EventImplBase > w = weakSelf;
			const uint32_t index = i;
			const uint32_t generation = entry->generation;
			entry->receiverLoop.load( std::memory_order_acquire )->post(
				EventLoop::Task::create(
					[ w, index, generation, capturedArgs ]() {
						if ( auto impl = w.lock() )
						{
							impl->invokeDeferred( index, generation, capturedArgs );
						}
					} ),
				entry->connectionTag );
			break;
		}

		case ResolvedConnectionType::None:
			break;
		}
	};

	if ( order != nullptr )
	{
		for ( uint32_t i : order->order )
		{
			dispatchSlot( i );
		}
	}
	else
	{
		for ( uint32_t i = 0; i < count; ++i )
		{
			dispatchSlot( i );
		}
	}

	if ( ! toRetire.empty() )
	{
		disconnectHandlers( toRetire );
	}
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::preLockDisconnectAll()
{
	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );
	for ( uint32_t i = 0; i < used; ++i )
	{
		Entry* entry = slots[ i ].load( std::memory_order_relaxed );
		if ( entry == nullptr )
		{
			continue;
		}

		slots[ i ].store( nullptr, std::memory_order_release );
		++_nextGeneration[ i ];
		_epoch.retire( entry );
	}

	_priorityConnCount = 0;
	publishOrder( nullptr );
}

template< typename MutexType, typename... Args >
template< typename MatchFn >
inline std::vector< GenData > BasicConcurrentEventImpl< MutexType, Args... >::findFirstMatch( MatchFn&& matches ) const
{
	std::vector< GenData > result;
	platform::LockGuard< MutexType > lock( mutex );
	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );
	for ( uint32_t i = 0; i < used; ++i )
	{
		Entry* entry = slots[ i ].load( std::memory_order_relaxed );
		if ( entry != nullptr && matches( entry->callable ) )
		{
			result.emplace_back( i, entry->generation );
			break;  // one match only; duplicates left in place
		}
	}
	return result;
}

template< typename MutexType, typename... Args >
inline bool BasicConcurrentEventImpl< MutexType, Args... >::isHandlerConnected( uint32_t index, uint32_t generation ) const
{
	auto guard = _epoch.enter();
	const uint32_t count = _usedCount.load( std::memory_order_acquire );
	if ( index >= count )
	{
		return false;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_acquire );
	Entry* entry = slots[ index ].load( std::memory_order_acquire );
	return entry != nullptr && entry->generation == generation;
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::disconnectHandler( uint32_t index, uint32_t generation )
{
	std::vector< GenData > entries { GenData( index, static_cast< uint16_t >( generation ) ) };
	disconnectHandlers( entries );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::disconnectHandlers( const std::vector< GenData >& entries )
{
	platform::LockGuard< MutexType > lock( mutex );

	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );
	const bool orderingLive = ( _orderSnapshot.load( std::memory_order_relaxed ) != nullptr )
		|| _priorityConnCount > 0;

	for ( const GenData& entry : entries )
	{
		if ( entry.index >= used )
		{
			continue;
		}

		Entry* old = slots[ entry.index ].load( std::memory_order_relaxed );
		if ( old == nullptr || old->generation != entry.generation )
		{
			continue;
		}

		if ( old->priority != 0 && _priorityConnCount > 0 )
		{
			--_priorityConnCount;
		}

		slots[ entry.index ].store( nullptr, std::memory_order_release );
		++_nextGeneration[ entry.index ];
		_epoch.retire( old );
	}

	if ( orderingLive )
	{
		rebuildOrderLocked();
	}
}

template< typename MutexType, typename... Args >
inline bool BasicConcurrentEventImpl< MutexType, Args... >::isHandlerBlocked( uint32_t index, uint32_t generation ) const
{
	auto guard = _epoch.enter();
	const uint32_t count = _usedCount.load( std::memory_order_acquire );
	if ( index >= count )
	{
		return false;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_acquire );
	Entry* entry = slots[ index ].load( std::memory_order_acquire );
	return entry != nullptr
		&& entry->generation == generation
		&& entry->blocked.load( std::memory_order_relaxed );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::blockHandler( uint32_t index, uint32_t generation )
{
	setBlocked( index, generation, true );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::unblockHandler( uint32_t index, uint32_t generation )
{
	setBlocked( index, generation, false );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::updateSenderLoop( EventLoop* loop )
{
	platform::LockGuard< MutexType > lock( mutex );

	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );

	for ( uint32_t i = 0; i < used; ++i )
	{
		Entry* entry = slots[ i ].load( std::memory_order_relaxed );
		if ( entry == nullptr || entry->declaredType != ConnectionType::Auto )
		{
			continue;
		}

		EventLoop* receiverLoop = entry->receiverLoop.load( std::memory_order_relaxed );
		const ResolvedConnectionType resolved =
			detail::resolveConnectionType( ConnectionType::Auto, loop, receiverLoop );
		entry->resolvedType.store( resolved, std::memory_order_release );
	}
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::updateHandlerLoop( uint32_t index, uint32_t generation, EventLoop* loop )
{
	platform::LockGuard< MutexType > lock( mutex );

	const uint32_t used = _usedCount.load( std::memory_order_relaxed );
	if ( index >= used )
	{
		return;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	Entry* entry = slots[ index ].load( std::memory_order_relaxed );
	if ( entry == nullptr || entry->generation != generation )
	{
		return;
	}

	// Direct connections are not affected by receiver loop changes -
	// matches BasicEventImpl::updateHandlerLoop exactly
	if ( entry->declaredType == ConnectionType::Direct )
	{
		return;
	}

	entry->receiverLoop.store( loop, std::memory_order_relaxed );

	EventLoop* senderLoop = owner ? owner->eventLoop() : nullptr;
	const ResolvedConnectionType resolved =
		detail::resolveConnectionType( entry->declaredType, senderLoop, loop );
	entry->resolvedType.store( resolved, std::memory_order_release );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::invokeDeferred( uint32_t index, uint32_t generation, const std::shared_ptr< void >& args )
{
	auto guard = _epoch.enter();

	const uint32_t count = _usedCount.load( std::memory_order_acquire );
	if ( index >= count )
	{
		return;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_acquire );
	Entry* entry = slots[ index ].load( std::memory_order_acquire );
	if ( entry == nullptr || entry->generation != generation )
	{
		return;  // stale: disconnected, or slot since reused by another connection
	}
	if ( entry->blocked.load( std::memory_order_relaxed ) )
	{
		return;
	}

	auto& typedArgs = *std::static_pointer_cast< std::tuple< std::decay_t< Args >... > >( args );

	// receiver-context predicate: evaluated here, on the drain thread, against the
	// receiver's state at handling time - sender-context predicates were already evaluated
	// in dispatch() before this task was ever posted (see dispatch()'s Deferred case above)
	if ( entry->hasPredicate && entry->isReceiverContextPredicate )
	{
		const bool passed = std::apply(
			[ entry ]( auto&&... a ) { return entry->predicate( std::forward< decltype( a ) >( a )... ); },
			typedArgs );
		if ( ! passed )
		{
			return;
		}
	}

	const bool once = entry->singleShot;
	std::apply(
		[ entry ]( auto&&... a ) { entry->callable( std::forward< decltype( a ) >( a )... ); },
		typedArgs );

	if ( once )
	{
		std::vector< GenData > toRetire { GenData( index, generation ) };
		disconnectHandlers( toRetire );
	}
}

// static
template< typename MutexType, typename... Args >
inline typename BasicConcurrentEventImpl< MutexType, Args... >::SlotArray*
BasicConcurrentEventImpl< MutexType, Args... >::allocateSlotArray( size_t capacity )
{
	SlotArray* array = new SlotArray[ capacity ];
	for ( size_t i = 0; i < capacity; ++i )
	{
		array[ i ].store( nullptr, std::memory_order_relaxed );
	}
	return array;
}

template< typename MutexType, typename... Args >
inline typename BasicConcurrentEventImpl< MutexType, Args... >::SlotArray*
BasicConcurrentEventImpl< MutexType, Args... >::growSlots()
{
	SlotArray* oldSlots = _slotsPtr.load( std::memory_order_relaxed );
	const size_t newCapacity = _capacity * 2;
	SlotArray* newSlots = allocateSlotArray( newCapacity );

	for ( size_t i = 0; i < _capacity; ++i )
	{
		newSlots[ i ].store( oldSlots[ i ].load( std::memory_order_relaxed ), std::memory_order_relaxed );
	}

	_slotsPtr.store( newSlots, std::memory_order_release );
	_capacity = newCapacity;

	_epoch.retireErased( oldSlots, []( void* p ) { delete[] static_cast< SlotArray* >( p ); } );

	return newSlots;
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::rebuildOrderLocked()
{
	if ( _priorityConnCount == 0 )
	{
		publishOrder( nullptr );
		return;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_relaxed );
	const uint32_t used = _usedCount.load( std::memory_order_relaxed );

	auto* snap = new OrderSnapshot();
	for ( uint32_t i = 0; i < used; ++i )
	{
		if ( slots[ i ].load( std::memory_order_relaxed ) != nullptr )
		{
			snap->order.push_back( i );
		}
	}

	// stable_sort by priority descending; pushed in slot order, so ties
	// keep slot-index order within a priority level - matches
	// EventImpl::rebuildOrder exactly
	std::stable_sort( snap->order.begin(), snap->order.end(),
		[ slots ]( uint32_t a, uint32_t b ) {
			return slots[ a ].load( std::memory_order_relaxed )->priority > slots[ b ].load( std::memory_order_relaxed )->priority;
		} );

	publishOrder( snap );
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::publishOrder( OrderSnapshot* snap )
{
	OrderSnapshot* old = _orderSnapshot.exchange( snap, std::memory_order_release );
	if ( old != nullptr )
	{
		_epoch.retire( old );
	}
}

template< typename MutexType, typename... Args >
inline void BasicConcurrentEventImpl< MutexType, Args... >::setBlocked( uint32_t index, uint32_t generation, bool blocked )
{
	auto guard = _epoch.enter();
	const uint32_t count = _usedCount.load( std::memory_order_acquire );
	if ( index >= count )
	{
		return;
	}

	SlotArray* slots = _slotsPtr.load( std::memory_order_acquire );
	Entry* entry = slots[ index ].load( std::memory_order_acquire );
	if ( entry != nullptr && entry->generation == generation )
	{
		entry->blocked.store( blocked, std::memory_order_relaxed );
	}
}


} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_BASIC_CONCURRENT_EVENT_IMPL_H
