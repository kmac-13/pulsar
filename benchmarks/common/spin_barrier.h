#ifndef SPIN_BARRIER_H
#define SPIN_BARRIER_H

/**
 * @file spin_barrier.h
 * @brief Reusable spin/block barrier for std::thread coordination, shared by
 * every library's thread-creating benchmark files.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// ============================================================================
// SpinBarrier
//
// arrive() spin-waits until all N parties have called arrive().
//
// Deliberately spin-based rather than condition_variable-based: Google
// Benchmark's cpu_time is measured per calling thread by default, and a
// blocking wait (condition_variable::wait) makes that thread's own CPU time
// under-report real elapsed work relative to wall time while it's asleep,
// which can cause the items_per_second and cpu_time measurements to be
// misleading for the cross-thread scenarios.  Spinning keeps cpu_time
// tracking wall time here, matching Pulsar's own Barrier.
// ============================================================================

class SpinBarrier
{
private:
	// number of pure-spin attempts (no yield) before falling back to a
	// real blocking wait - see the note in arrive() above for why the
	// fallback exists
	static constexpr int SPIN_ATTEMPTS_BEFORE_BLOCKING = 1000;

	const int _count;
	std::atomic< int > _waiting{ 0 };
	std::atomic< int > _generation{ 0 };
	std::mutex _mutex;
	std::condition_variable _cv;

public:
	explicit SpinBarrier( int count ) : _count( count ) {}

	void arrive()
	{
		const int gen = _generation.load( std::memory_order_relaxed );
		if ( _waiting.fetch_add( 1, std::memory_order_acq_rel ) + 1 == _count )
		{
			_waiting.store( 0, std::memory_order_relaxed );
			_generation.fetch_add( 1, std::memory_order_release );

			// safe to notify without holding _mutex: _cv.wait(lock,
			// predicate) below re-checks the predicate (backed by the
			// atomic _generation) under the lock before actually
			// blocking, so a notify that "arrives early" relative to a
			// waiter isn't lost - the waiter's own recheck will see it
			_cv.notify_all();
			return;
		}

		// short pure-spin phase for the common case: at low thread
		// counts (at or below the CPU's core count), the barrier
		// resolves within microseconds and spinning avoids the cost of
		// a real OS wait/wake round trip
		for ( int spin = 0; spin < SPIN_ATTEMPTS_BEFORE_BLOCKING; ++spin )
		{
			if ( _generation.load( std::memory_order_acquire ) != gen )
			{
				return;
			}
			std::this_thread::yield();
		}

		// fall back to a real blocking wait once thread count meets or
		// exceeds the CPU's core count - once thread count exceeds core
		// count, a thread waiting on another thread that happens to be
		// scheduled on a different core can spin indefinitely without
		// ever seeing progress, since yield() can only ever hand off to a
		// thread on the SAME core; a real condition_variable wait doesn't
		// have this limitation - notify_all() correctly wakes waiters
		// regardless of which core they're on
		std::unique_lock< std::mutex > lock( _mutex );
		while ( _generation.load( std::memory_order_acquire ) == gen )
		{
			_cv.wait_for( lock, std::chrono::milliseconds( 5 ) );
		}
	}
};

#endif // SPIN_BARRIER_H
