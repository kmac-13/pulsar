/**
 * @file bench_mutex_comparison.cpp
 * @brief Raw uncontended lock/unlock cost, std::mutex vs. std::recursive_mutex
 * vs. Pulsar's optional mutex types vs. (on Windows) a raw SRWLOCK.
 *
 * This is a platform/toolchain probe, not a Pulsar-vs-other-library
 * comparison: no events, no dispatch, no connections - just lock()/unlock()
 * in a tight loop.  It exists to help identify a MutexType for
 * BasicEvent<MutexType, Args...> (or any other lockable-templated code) on
 * a given target, by measuring the primitives directly rather than through
 * Pulsar's dispatch machinery the way bench_pulsar_mutex_variants.cpp does.
 *
 * Deliberately NOT wired into compare.py: there is no cross-library
 * comparison point for a raw mutex, and this is a diagnostic for the person
 * running it on their own machine, not a result meant to sit in the shared
 * benchmarks table.
 *
 * platform::NullMutex is included as the zero-overhead floor (loop
 * overhead only) that every real mutex's cost should be read against.
 */

#include <kmac/pulsar/platform.h>
#include <kmac/pulsar/mutex_types.h>

#include <benchmark/benchmark.h>

#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pulsar = kmac::pulsar;

#ifdef _WIN32
// Minimal Lockable wrapper around SRWLOCK, Windows' lightest-weight native
// exclusive lock primitive - no kernel object, pure userspace on the
// uncontended path. NOT recursive: SRWLOCK does not support recursive
// acquisition at all (undefined behaviour), same restriction std::mutex has.
// See windows_srwlock_bench.cpp for the standalone version this is
// duplicated from - kept separate rather than shared so this file has no
// dependency on that one.
class SrwMutex
{
private:
	SRWLOCK _lock;

public:
	SrwMutex() { InitializeSRWLock( &_lock ); }
	void lock() { AcquireSRWLockExclusive( &_lock ); }
	void unlock() { ReleaseSRWLockExclusive( &_lock ); }
	bool try_lock() { return TryAcquireSRWLockExclusive( &_lock ) != 0; }
};
#endif

// ============================================================================
// Uncontended lock/unlock, one thread, no held critical section
// ============================================================================

template< typename MutexType >
static void BM_UncontendedLockUnlock( benchmark::State& state )
{
	MutexType m;

	for ( auto _ : state )
	{
		m.lock();
		m.unlock();
	}
}
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, std::mutex );            // pulsar::platform::Mutex
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, std::recursive_mutex );  // pulsar::platform::RecursiveMutex
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, pulsar::platform::SharedMutex );
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, pulsar::platform::NullMutex );
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, pulsar::platform::FastRecursiveMutex );
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, pulsar::platform::SpinRecursiveMutex );
#ifdef _WIN32
BENCHMARK_TEMPLATE( BM_UncontendedLockUnlock, SrwMutex );
#endif

BENCHMARK_MAIN();
