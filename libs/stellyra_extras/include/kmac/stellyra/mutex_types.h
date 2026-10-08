#pragma once
#ifndef KMAC_STELLYRA_MUTEX_TYPES_H
#define KMAC_STELLYRA_MUTEX_TYPES_H

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>

/**
 * @file mutex_types.h
 * @brief Optional mutex types for BasicEvent<MutexType, Args...>
 *
 * BasicEvent takes its mutex type as a free template parameter - Event,
 * SharedEvent, and SingleThreadedEvent (in <kmac/stellyra/platform.h>, part
 * of the core library) are just convenience aliases for
 * std::recursive_mutex, std::shared_mutex, and a no-op null mutex
 * respectively.  The two types in this header are additional, OPTIONAL
 * mutex types that can be used the same way
 * (BasicEvent<FastRecursiveMutex, Args...>, etc.) but aren't part of any
 * shipped Event alias, and aren't held to the same "used throughout the
 * core library" bar those are.
 *
 * Namespace: kmac::stellyra::platform
 */

namespace kmac {
namespace stellyra {
namespace platform {

/**
 * @brief Experimental recursive mutex with a cheaper uncontended/
 * non-reentrant fast path than std::recursive_mutex.
 *
 * Rationale: std::recursive_mutex's per-platform implementation may do
 * extra bookkeeping (e.g. a thread-ID comparison plus internal
 * recursion-count tracking) on EVERY lock() call, even when the calling
 * thread never actually re-enters - the overwhelmingly common case for
 * Event::_mutex (most emissions are not reentrant).  Whether that
 * bookkeeping actually costs more than this class's own atomic-owner-check
 * fast path is a per-platform/per-toolchain question, not a settled fact;
 * should be measured on each target -
 * benchmarks/bench_platform/bench_mutex_comparison.cpp measures std::mutex,
 * std::recursive_mutex, FastRecursiveMutex, and the other mutex types side
 * by side, and is one way to help deciee which is the best mutex type to
 * use on your own toolchain.
 *
 * Design: wraps a plain std::mutex (cheaper uncontended lock than
 * recursive_mutex per the above) plus a manually-tracked owner thread ID
 * and depth counter.  lock() checks "is the calling thread already the
 * owner?" via a relaxed atomic load BEFORE attempting the underlying
 * mutex - if so, this is a reentrant call, depth is incremented, and the
 * underlying mutex is untouched (already held by this thread from the
 * outer lock() call).  If not, lock() blocks on the underlying mutex as
 * normal, then records ownership.
 *
 * This is the same lockable-only contract as RecursiveMutex
 * (lock/unlock/try_lock) - no condition_variable compatibility needed,
 * matching Event::_mutex's actual usage (LockGuard/UniqueLock only, no
 * wait()).
 *
 * THREAD SAFETY NOTE: _owner is read via relaxed load by every lock() call
 * (including from threads that do NOT hold the lock) - this is safe because:
 * - a thread only ever WRITES _owner to its own ID (after acquiring
 *   _inner) or to the "no owner" sentinel (right before releasing
 *   _inner), so a reader can only ever observe either some OTHER
 *   thread's ID (in which case it must contend for _inner regardless
 * - correct, since it is genuinely not the owner) or its OWN ID
 *   (which is only possible if this thread itself wrote it, establishing
 *   the fast path correctly) - there is no value _owner could hold that
 *   would cause a non-owning thread to incorrectly take the fast path
 */
class FastRecursiveMutex
{
	static_assert( std::is_trivially_copyable_v< std::thread::id >,
		"FastRecursiveMutex requires std::thread::id to be trivially copyable "
		"for std::atomic<std::thread::id> to be well-formed per the standard; "
		"true for libstdc++ (Linux, MinGW) and MSVC's STL" );

private:
	std::mutex _inner;
	std::atomic< std::thread::id > _owner { std::thread::id{} };
	int _depth = 0;  ///< only touched while _inner is held by the owning thread

public:
	void lock()
	{
		const std::thread::id self = std::this_thread::get_id();

		if ( _owner.load( std::memory_order_relaxed ) == self )
		{
			// reentrant: this thread already holds _inner from an outer
			// lock() call - no need to touch _inner again
			++_depth;
			return;
		}

		_inner.lock();
		_owner.store( self, std::memory_order_relaxed );
		_depth = 1;
	}

	bool try_lock()
	{
		const std::thread::id self = std::this_thread::get_id();

		if ( _owner.load( std::memory_order_relaxed ) == self )
		{
			++_depth;
			return true;
		}

		if ( _inner.try_lock() )
		{
			_owner.store( self, std::memory_order_relaxed );
			_depth = 1;
			return true;
		}

		return false;
	}

	void unlock()
	{
		// _depth and _owner here are only ever touched by the thread that currently
		// owns the lock (this call only makes sense if the calling thread holds it -
		// same contract as recursive_mutex::unlock() called without a matching lock(),
		// which is UB), so no atomicity is needed for the decrement itself
		if ( --_depth == 0 )
		{
			// clear ownership BEFORE unlocking _inner: once _inner is unlocked, another
			// thread may immediately acquire it and must not observe a stale _owner
			// pointing at this thread
			_owner.store( std::thread::id{}, std::memory_order_relaxed );
			_inner.unlock();
		}
	}
};

/**
 * @brief Experimental reentrant spin lock.
 *
 * Same owner-plus-depth-counter design as FastRecursiveMutex above, and
 * the identical correctness reasoning applies verbatim to the _owner
 * fast-path check here (see FastRecursiveMutex's THREAD SAFETY NOTE) -
 * the only difference is what happens when the calling thread is NOT
 * already the owner: FastRecursiveMutex blocks on an inner std::mutex;
 * this spins on an atomic_flag instead, falling back to
 * std::this_thread::yield() after a short pure-spin phase rather than
 * spinning indefinitely.
 *
 * That backoff exists because pure, unconditional busy-waiting is a real
 * production hazard, not just a performance nitpick: on a CPU with fewer
 * hardware threads than contending software threads (or with hyperthreading,
 * where a spinning sibling can starve the core the lock HOLDER needs to make
 * progress on), an indefinite spin can measurably delay the thread that's
 * supposed to release the lock, making contention worse the longer it goes
 * on rather than resolving quickly the way a short spin is meant to.
 * Yielding after a bounded number of failed attempts is the standard
 * mitigation - it keeps the fast, no-syscall path for the common case (the
 * lock becomes free within a few spins) while not pathologically
 * monopolizing a core once contention runs longer than that.
 *
 * Intended use is the same as FastRecursiveMutex: as an experimental
 * BasicEvent<SpinRecursiveMutex, Args...> instantiation to compare
 * against Event/SharedEvent's real, shipped mutex types - most useful
 * for very short critical sections under light contention, and
 * increasingly the wrong choice as contention or handler execution time
 * grows, same tradeoff any spin lock makes.
 */
class SpinRecursiveMutex
{
	static_assert( std::is_trivially_copyable_v< std::thread::id >,
		"SpinRecursiveMutex requires std::thread::id to be trivially copyable "
		"for std::atomic<std::thread::id> to be well-formed per the standard; "
		"true for libstdc++ (Linux, MinGW) and MSVC's STL" );

private:
	/// number of pure-spin attempts (no yield) before falling back to
	/// std::this_thread::yield() - chosen as a small, conservative
	/// constant rather than tuned against any specific workload
	/// TODO: revisit if profiling ever justifies a different value
	static constexpr int SPIN_ATTEMPTS_BEFORE_YIELD = 64;

	std::atomic_flag _locked = ATOMIC_FLAG_INIT;
	std::atomic< std::thread::id > _owner { std::thread::id{} };
	int _depth = 0;  ///< only touched while _locked is held by the owning thread

	void acquireSlowPath() noexcept
	{
		for ( ;; )
		{
			for ( int spin = 0; spin < SPIN_ATTEMPTS_BEFORE_YIELD; ++spin )
			{
				if ( ! _locked.test_and_set( std::memory_order_acquire ) )
				{
					return;
				}
			}

			// still contended after a short pure spin - stop monopolizing
			// this core and let the OS scheduler run someone else,
			// plausibly including whichever thread is holding the lock
			std::this_thread::yield();
		}
	}

public:
	void lock() noexcept
	{
		const std::thread::id self = std::this_thread::get_id();

		if ( _owner.load( std::memory_order_relaxed ) == self )
		{
			// reentrant: this thread already holds _locked from an outer
			// lock() call - see FastRecursiveMutex's THREAD SAFETY NOTE
			// above for why this check is safe against non-owning readers
			++_depth;
			return;
		}

		acquireSlowPath();
		_owner.store( self, std::memory_order_relaxed );
		_depth = 1;
	}

	bool try_lock() noexcept
	{
		const std::thread::id self = std::this_thread::get_id();

		if ( _owner.load( std::memory_order_relaxed ) == self )
		{
			++_depth;
			return true;
		}

		if ( ! _locked.test_and_set( std::memory_order_acquire ) )
		{
			_owner.store( self, std::memory_order_relaxed );
			_depth = 1;
			return true;
		}

		return false;
	}

	void unlock() noexcept
	{
		// same reasoning as FastRecursiveMutex::unlock(): only ever
		// called by the owning thread, so the decrement itself needs no
		// extra synchronization
		if ( --_depth == 0 )
		{
			// clear ownership BEFORE releasing _locked - identical
			// ordering requirement and reasoning as FastRecursiveMutex,
			// substituting "another thread's test_and_set succeeds" for
			// "another thread's _inner.lock() returns"
			_owner.store( std::thread::id{}, std::memory_order_relaxed );
			_locked.clear( std::memory_order_release );
		}
	}
};

} // namespace platform
} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_MUTEX_TYPES_H
