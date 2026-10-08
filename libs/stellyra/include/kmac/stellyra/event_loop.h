#pragma once
#ifndef KMAC_STELLYRA_EVENT_LOOP_H
#define KMAC_STELLYRA_EVENT_LOOP_H

/**
 * @file event_loop.h
 * @brief Manually-drained task queue for Deferred connection dispatch.
 *
 * @section task Task type
 *
 * Tasks are Callable<void()>.  post() takes ownership via move.
 *
 * @section vectors Queue and drain vectors
 *
 * Two vectors are maintained:
 *  - _pending: tasks waiting to be drained, protected by _pendingMutex
 *  - _active:  tasks currently being processed by drain(); only one thread
 *              can drain this vector at a time
 *
 * drain() swaps _pending into _active under a brief lock, then processes
 * _active without holding any lock.  Both vectors retain their allocated
 * capacity across calls by default; construct with retainCapacity = false
 * to release _active after each batch instead.
 *
 * @section migration Task migration
 *
 * migratePendingTo( dest, tag ) moves all entries in _pending whose tag
 * matches the given value into dest._pending, preserving their relative
 * order.  Entries in _active (already being drained) are left untouched.
 * This is how Trackable::setEventLoop() transfers in-flight deferred tasks
 * to a new loop without disrupting any work already underway.
 *
 * Every connection where a given Trackable is the receiver shares that
 * Trackable's single migration tag (see Trackable::migrationTag()), so one
 * migratePendingTo() call moves all of a receiver's pending tasks together
 * in one pass, regardless of which event or connection produced each one -
 * there is no need to collect or pass more than one tag per receiver.
 *
 * If any tasks are migrated, dest's post-notification hook (if attached,
 * e.g. via AutoDrainThread) is invoked afterward, the same as post() does -
 * so a destination loop with a waiting drain thread wakes up to process the
 * migrated tasks rather than leaving them pending until something else
 * happens to post() or drain() that loop.
 *
 * No ordering guarantees are made across multiple migration calls: each call
 * appends its extracted tasks to dest._pending after whatever was already
 * there, so the arrival order in the destination reflects the sequence of
 * migration calls rather than the original posting order.
 *
 * @section drain Drain semantics
 *
 * Any thread may call drain().  Concurrent calls are short-circuited via
 * atomic compare-exchange.  Tasks posted during drain() are processed before
 * drain() completes, although there is a short window during which drain()
 * is wrapping up and new tasks can be posted without being handled.
 *
 * @section drain_thread Drain thread
 *
 * An optional registered drain thread ID supports sender-affinity queries.
 * Set via setDrainThread(), callable from any thread.
 */

#include "platform.h"

#include "callable.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace kmac {
namespace stellyra {

class EventLoop
{
	friend class AutoDrainThread;

public:
	using Task = Callable< void() >;

private:
	/**
	 * @brief A queued task with an optional owner ID for migration.
	 * The ID matches EventImplBase::_id of the owning event.
	 */
	struct QueueEntry
	{
		Task task;
		uint64_t tag = 0;
	};

	mutable platform::Mutex _pendingMutex;  ///< guards _pending only; _active is lock-free (see class docs)
	std::vector< QueueEntry > _pending;     ///< tasks waiting to be drained
	std::vector< QueueEntry > _active;      ///< tasks currently being drained

	platform::Atomic< bool > _isDraining { false };

	/**
	 * @brief The thread currently executing drain() for this loop, if any.
	 *
	 * Distinct from _drainThread/_hasDrainThread below: this tracks live
	 * drain state rather than a persistent registration, and needs no
	 * setup call - it's written by drain() itself.  Reset to the
	 * default-constructed ThreadId{} sentinel before _isDraining is
	 * cleared, so that whichever thread next wins the _isDraining CAS is
	 * guaranteed to see the sentinel (never a *different* thread's stale
	 * id) during the brief window before it writes its own id here - a
	 * reader can therefore only ever get a false negative, never a false
	 * positive.
	 */
	platform::Atomic< platform::ThreadId > _activeDrainThreadId { platform::ThreadId{} };

	/**
	 * @brief Atomic because drainThread() may be read from any thread
	 * concurrently with setDrainThread().  Relaxed ordering is sufficient;
	 * the ThreadId value should be set once at setup, well before drain calls.
	 */
	platform::Atomic< platform::ThreadId > _drainThread {};
	platform::Atomic< bool > _hasDrainThread { false };

	/**
	 * @brief Called at the end of post() to wake a waiting drain thread.
	 * Null when no AutoDrainThread is attached.  Only AutoDrainThread may
	 * set or clear this via the private accessors below.
	 */
	Callable< void() > _postNotify;

	bool _retainCapacity;  ///< whether drain should maintain storage or reset to default-constructed lists

#ifdef STELLYRA_TEST_INJECT
	void ( *_migrateTestHook )() = nullptr;
#endif

public:
	/**
	 * @brief Construct an EventLoop.
	 *
	 * @param retainCapacity true (default) to indicate that both vectors should
	 *   retain their capacity between drains, false to indicate that the _active
	 *   queue is deallocated (reassigned to a default constructed vector) at the
	 *   end of each drain
	 */
	explicit EventLoop( bool retainCapacity = true ) noexcept;

	~EventLoop() = default;

	EventLoop( const EventLoop& ) = delete;
	EventLoop& operator=( const EventLoop& ) = delete;
	EventLoop( EventLoop&& ) = delete;
	EventLoop& operator=( EventLoop&& ) = delete;

	// -------------------------------------------------------------------------

	/**
	 * @brief True while any thread is inside a drain() call for this loop.
	 */
	bool isDraining() const noexcept;

	/**
	 * @brief Returns true if `id` is, right now, the thread executing
	 * this loop's drain() call.
	 *
	 * Requires no setup - unlike hasDrainThread()/drainThread() below,
	 * which track a persistent registration, this answers "is drain()
	 * for this loop active on this exact thread at this exact moment".
	 */
	bool isDrainingOnThread( platform::ThreadId id ) const noexcept;

	/**
	 * @brief True if a thread has been registered via setDrainThread() and
	 * not yet cleared via clearDrainThread().
	 */
	bool hasDrainThread() const noexcept;

	/**
	 * @brief The registered drain thread ID.  Meaningless if
	 * hasDrainThread() is false (returns a default-constructed ThreadId{}
	 * in that case, not a sentinel to check against directly - check
	 * hasDrainThread() first).
	 */
	platform::ThreadId drainThread() const noexcept;

	/**
	 * @brief Register a thread as the designated drain thread.
	 *
	 * May be called from any thread at any time.
	 */
	void setDrainThread( platform::ThreadId id ) noexcept;

	/**
	 * @brief Clear the drain thread registration.
	 *
	 * After this call hasDrainThread() returns false and operator() on any
	 * event with this loop as its sender loop will fire synchronously again.
	 */
	void clearDrainThread() noexcept;

	/**
	 * @brief The gate BasicEvent::operator() (and BasicRecordableEvent's
	 * equivalent) uses to decide whether a trigger dispatches synchronously
	 * on the calling thread immediately or is deferred to this loop's queue
	 * to run whenever it is next drained (i.e. whether sender-loop
	 * deferral applies for the given thread).
	 *
	 * Returns true if `id` is already this loop's execution context -
	 * either because it has been registered via setDrainThread() (a thread
	 * that has promised to service this loop may always dispatch directly -
	 * queuing then immediately draining on the same thread has no
	 * observable difference), or because it is, right now, actively
	 * executing this loop's drain() call (so a reentrant trigger during
	 * that very drain dispatches directly instead of taking an unnecessary
	 * queue round-trip).  Any other thread defers, so the actual dispatch
	 * runs on whichever thread is responsible for this loop rather than
	 * unsynchronized on the caller's thread.
	 *
	 * This governs only whether event triggering happens now vs. later - it
	 * has no bearing on how each connected handler is invoked once dispatch
	 * does proceed.  That is decided separately and per-connection by
	 * resolveConnectionType(), cached initially at connect time and
	 * re-resolved on setEventLoop() switches as each handler's
	 * ResolvedConnectionType.
	 */
	bool shouldDispatchDirectlyOnThread( platform::ThreadId id ) const noexcept;

	/**
	 * @brief Enqueue a task.  Thread-safe; takes ownership.
	 *
	 * @param task the task to enqueue
	 * @param tag the receiver's migration tag (Trackable::migrationTag()),
	 *   used by migratePendingTo() to find all of a receiver's pending
	 *   tasks in one pass; pass 0 (default) for untagged tasks
	 */
	void post( Task task, uint64_t tag = 0 );

	/**
	 * @brief Process all queued tasks in FIFO order.
	 *
	 * If a drain is already in progress this call returns immediately.
	 * Tasks are processed until both the _active and _pending lists are
	 * empty, so tasks posted during drain() are processed.
	 *
	 * Safe to call from any thread.
	 */
	void drain();

	/**
	 * @brief Move all pending tasks tagged with `tag` to dest, preserving
	 * their relative order.
	 *
	 * @note Untracked tasks, i.e. those with tag == 0, are not migrated - 0
	 * is reserved as the "no particular receiver" sentinel (see
	 * Trackable::migrationTag()), so a call with tag == 0 is always a no-op
	 * rather than matching every untagged task in _pending.
	 *
	 * If any tasks are migrated, dest's post-notification hook (if attached)
	 * is invoked afterward - the same as post() does - so a destination loop
	 * with a waiting drain thread wakes to process them immediately.
	 *
	 * Every connection where a given Trackable is the receiver shares that
	 * Trackable's single migration tag, so one call here moves all of a
	 * receiver's pending tasks together, regardless of which event or
	 * connection produced each one - see Trackable::setEventLoop().
	 */
	void migratePendingTo( EventLoop* dest, uint64_t tag );

#ifdef STELLYRA_TEST_INJECT
	/**
	 * @brief Test-only: installs a callback invoked from migratePendingTo()
	 * immediately after both loops' _pendingMutex locks are acquired, but
	 * before the partition/erase work begins.  Lets a test hold that
	 * critical section open for a controlled duration, to deterministically
	 * prove a concurrent drain() on the same source loop actually blocks on
	 * the shared mutex, rather than hoping the scheduler happens to
	 * interleave the two calls.
	 *
	 * Compiled out entirely - including this method and its backing member -
	 * unless STELLYRA_TEST_INJECT is defined, so normal builds (and every
	 * test that doesn't opt in) are completely unaffected; sizeof(EventLoop)
	 * and migratePendingTo()'s behavior are identical to the non-injected
	 * build whenever no hook is installed.
	 */
	void setMigrateTestHook( void ( *hook )() );
#endif

private:
	/**
	 * @brief Installs the hook post() calls after enqueueing, to wake a
	 * waiting drain thread.  Only AutoDrainThread calls this.
	 */
	void setPostNotify( Callable< void() > notify );

	/**
	 * @brief Removes the hook installed by setPostNotify(), restoring the
	 * no-drain-thread-attached behaviour.
	 */
	void clearPostNotify();
};

// ---------------------------------------------------------------------------

inline EventLoop::EventLoop( bool retainCapacity ) noexcept
	: _retainCapacity( retainCapacity )
{
}

inline bool EventLoop::isDraining() const noexcept
{
	return _isDraining.load( std::memory_order_relaxed );
}

inline bool EventLoop::isDrainingOnThread( platform::ThreadId id ) const noexcept
{
	return _isDraining.load( std::memory_order_acquire )
		&& _activeDrainThreadId.load( std::memory_order_relaxed ) == id;
}

inline bool EventLoop::hasDrainThread() const noexcept
{
	return _hasDrainThread.load( std::memory_order_acquire );
}

inline platform::ThreadId EventLoop::drainThread() const noexcept
{
	return _drainThread.load( std::memory_order_relaxed );
}

inline void EventLoop::setDrainThread( platform::ThreadId id ) noexcept
{
	_drainThread.store( id, std::memory_order_relaxed );
	_hasDrainThread.store( true, std::memory_order_release );
}

inline void EventLoop::clearDrainThread() noexcept
{
	_hasDrainThread.store( false, std::memory_order_release );
}

inline bool EventLoop::shouldDispatchDirectlyOnThread( platform::ThreadId id ) const noexcept
{
	return ( hasDrainThread() && drainThread() == id ) || isDrainingOnThread( id );
}

inline void EventLoop::post( Task task, uint64_t tag )
{
	{
		std::lock_guard< platform::Mutex > lock( _pendingMutex );
		_pending.push_back( { std::move( task ), tag } );
	}

	// notify outside the lock (if notify call is valid), so the drain
	// thread wakes without contending on _pendingMutex
	if ( _postNotify )
	{
		_postNotify();
	}
}

inline void EventLoop::drain()
{
	bool expected = false;
	if ( ! _isDraining.compare_exchange_strong(
		expected, true, std::memory_order_acquire, std::memory_order_relaxed ) )
	{
		return;
	}

	// safe: only the thread that just won the CAS above reaches this line,
	// so there is no concurrent writer to race against
	_activeDrainThreadId.store( platform::currentThreadId(), std::memory_order_release );

	while ( true )
	{
		bool hasWork;
		{
			std::lock_guard< platform::Mutex > lock( _pendingMutex );
			hasWork = ! _pending.empty();
			if ( hasWork )
			{
				std::swap( _pending, _active );
			}
		}

		// exit the loop if there's no more work
		if ( ! hasWork )
		{
			break;
		}

		// process all active tasks
		for ( auto& entry : _active )
		{
			entry.task();
		}

		// clear the list if capacity should be retained
		if ( _retainCapacity )
		{
			_active.clear();
		}
		// otherwise, reset the list (i.e. default construct a new list)
		else
		{
			_active = std::vector< QueueEntry >{};
		}
	}

	// reset before releasing _isDraining, so the sentinel-only invariant
	// holds for whichever thread wins the CAS next
	_activeDrainThreadId.store( platform::ThreadId{}, std::memory_order_release );
	_isDraining.store( false, std::memory_order_release );
}

inline void EventLoop::migratePendingTo( EventLoop* dest, uint64_t tag )
{
	// terminate early if the destination loop is invalid or if it's this loop;
	// tag == 0 is the "untagged" sentinel and is never migrated
	if ( ! dest || dest == this || tag == 0 )
	{
		return;
	}

	bool migratedAny = false;
	{
		platform::UniqueLock< platform::Mutex > lockSrc( _pendingMutex, std::defer_lock );
		platform::UniqueLock< platform::Mutex > lockDst( dest->_pendingMutex, std::defer_lock );
		std::lock( lockSrc, lockDst );

#ifdef STELLYRA_TEST_INJECT
		if ( _migrateTestHook )
		{
			_migrateTestHook();
		}
#endif

		// single pass over _pending: match this tag, preserving the
		// relative order of all the receiver's own tasks
		auto pivot = std::stable_partition( _pending.begin(), _pending.end(),
			[ tag ]( const QueueEntry& e ) {
				return e.tag != tag;  // keep in left partition unless it matches
			} );

		migratedAny = ( pivot != _pending.end() );

		for ( auto it = pivot; it != _pending.end(); ++it )
		{
			dest->_pending.push_back( std::move( *it ) );
		}

		_pending.erase( pivot, _pending.end() );
	}  // locks released here

	// notify dest's drain thread (if any) outside the lock, so a migration
	// into an AutoDrainThread-backed loop wakes it the same way post() does -
	// otherwise migrated tasks could sit unnoticed until something else
	// happens to post() or drain() that loop
	if ( migratedAny && dest->_postNotify )
	{
		dest->_postNotify();
	}
}

#ifdef STELLYRA_TEST_INJECT
inline void EventLoop::setMigrateTestHook( void ( *hook )() )
{
	_migrateTestHook = hook;
}
#endif

inline void EventLoop::setPostNotify( Callable< void() > notify )
{
	_postNotify = std::move( notify );
}

inline void EventLoop::clearPostNotify()
{
	_postNotify = Callable< void() >{};
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_EVENT_LOOP_H
