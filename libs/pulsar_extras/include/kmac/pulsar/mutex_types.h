#pragma once
#ifndef KMAC_PULSAR_MUTEX_TYPES_H
#define KMAC_PULSAR_MUTEX_TYPES_H

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>

/**
 * @file mutex_types.h
 * @brief Optional mutex types for BasicEvent<MutexType, Args...>
 *
 * BasicEvent takes its mutex type as a free template parameter -- Event,
 * SharedEvent, and SingleThreadedEvent (in <kmac/pulsar/platform.h>, part
 * of the core library) are just convenience aliases for
 * std::recursive_mutex, std::shared_mutex, and a no-op null mutex
 * respectively. The two types in this header are additional, OPTIONAL
 * mutex types that can be used the same way
 * (BasicEvent<FastRecursiveMutex, Args...>, etc.) but aren't part of any
 * shipped Event alias, and aren't held to the same "used throughout the
 * core library" bar those are.
 *
 * FastRecursiveMutex carries forward whatever validation it already had
 * from its time in the core library. SpinRecursiveMutex has been checked
 * with: a single-threaded reentrancy test (a handler re-entering
 * trigger() on the same event through several levels, the exact pattern
 * a non-reentrant spin lock would deadlock on) and a 16-thread/800,000-
 * emission stress test with a deliberately non-atomic shared counter
 * inside the handler, run clean under ThreadSanitizer with no data races
 * reported and no lost updates. That's real evidence of correctness for
 * what it covers, but it isn't exhaustive -- no long-duration soak
 * testing, no testing on real Windows/MinGW (only Linux/libstdc++ at
 * time of writing), no testing at higher thread counts than 16. Treat it
 * as meaningfully more validated than a brand-new primitive, not as
 * carrying the same track record as std::recursive_mutex or
 * FastRecursiveMutex.
 *
 * Namespace: kmac::pulsar::platform
 */

namespace kmac {
namespace pulsar {
namespace platform {

/**
 * @brief Experimental recursive mutex with a cheaper uncontended/
 * non-reentrant fast path than std::recursive_mutex.
 *
 * Moved here from platform.h -- it was defined in the core library but
 * never used by any shipped Event alias (Event uses plain
 * std::recursive_mutex), only by unit tests and benchmarks exploring
 * whether it should be. This is exactly the kind of optional,
 * not-used-by-the-core-library type this header exists for.
 *
 * Rationale: std::recursive_mutex's per-platform implementation typically
 * does extra bookkeeping (e.g. a thread-ID comparison plus internal
 * recursion-count tracking) on EVERY lock() call, even when the calling
 * thread never actually re-enters - the overwhelmingly common case for
 * Event::_mutex (most emissions are not reentrant).  Measured on Linux/
 * glibc, std::recursive_mutex costs ~2 ns more than std::mutex per
 * uncontended lock(); the gap is suspected larger on Windows/MinGW,
 * consistent with Pulsar's measured ~7-8 ns ST-vs-TS emission gap.
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
 * Same Lockable-only contract as RecursiveMutex (lock/unlock/try_lock) -
 * no condition_variable compatibility needed, matching Event::_mutex's
 * actual usage (LockGuard/UniqueLock only, no wait()).
 *
 * THREAD SAFETY NOTE: _owner is read via relaxed load by every lock()
 * call (including from threads that do NOT hold the lock) - this is safe
 * because:
 *  - a thread only ever WRITES _owner to its own ID (after acquiring
 *    _inner) or to the "no owner" sentinel (right before releasing
 *    _inner), so a reader can only ever observe either some OTHER
 *    thread's ID (in which case it must contend for _inner regardless -
 *    correct, since it is genuinely not the owner) or its OWN ID
 *    (which is only possible if this thread itself wrote it, establishing
 *    the fast path correctly) - there is no value _owner could hold that
 *    would cause a non-owning thread to incorrectly take the fast path.
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
		// _depth and _owner here are only ever touched by the thread that
		// currently owns the lock (this call only makes sense if the
		// calling thread holds it - same contract as recursive_mutex::
		// unlock() called without a matching lock(), which is UB), so no
		// atomicity is needed for the decrement itself.
		if ( --_depth == 0 )
		{
			// clear ownership BEFORE unlocking _inner: once _inner is
			// unlocked, another thread may immediately acquire it and
			// must not observe a stale _owner pointing at this thread
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
 * hardware threads than contending software threads (or with
 * hyperthreading, where a spinning sibling can starve the core the lock
 * HOLDER needs to make progress on), an indefinite spin can measurably
 * delay the thread that's supposed to release the lock, making
 * contention worse the longer it goes on rather than resolving quickly
 * the way a short spin is meant to. Yielding after a bounded number of
 * failed attempts is the standard mitigation - it keeps the fast,
 * no-syscall path for the common case (the lock becomes free within a
 * few spins) while not pathologically monopolizing a core once
 * contention runs longer than that.
 *
 * Intended use is the same as FastRecursiveMutex: as an experimental
 * BasicEvent<SpinRecursiveMutex, Args...> instantiation to compare
 * against Event/SharedEvent's real, shipped mutex types - most useful
 * for very short critical sections under light contention, and
 * increasingly the wrong choice as contention or handler execution time
 * grows, same tradeoff any spin lock makes.
 *
 * Checked with a single-threaded reentrancy test and a 16-thread/
 * 800,000-emission stress test (non-atomic shared counter inside the
 * handler) run clean under ThreadSanitizer - see the file-level comment
 * above for exactly what that does and doesn't cover. Meaningfully more
 * validated than a brand-new primitive, but still short of
 * RecursiveMutex/SharedMutex's track record - treat it as experimental,
 * not a production-ready primitive, until broader validation exists.
 */
class SpinRecursiveMutex
{
	static_assert( std::is_trivially_copyable_v< std::thread::id >,
		"SpinRecursiveMutex requires std::thread::id to be trivially copyable "
		"for std::atomic<std::thread::id> to be well-formed per the standard; "
		"true for libstdc++ (Linux, MinGW) and MSVC's STL" );

private:
	/// Number of pure-spin attempts (no yield) before falling back to
	/// std::this_thread::yield() -- chosen as a small, conservative
	/// constant rather than tuned against any specific workload; revisit
	/// if profiling ever justifies a different value.
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
				if ( !_locked.test_and_set( std::memory_order_acquire ) )
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

		if ( !_locked.test_and_set( std::memory_order_acquire ) )
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
} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_MUTEX_TYPES_H
