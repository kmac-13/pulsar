/**
 * @file windows_srwlock_bench.cpp
 * @brief Compares std::mutex against a raw SRWLOCK wrapper for uncontended
 * lock/unlock cost.
 *
 * SRWLOCK (Slim Reader/Writer Lock) is Windows' lightest-weight native
 * exclusive lock primitive - no kernel object is created, the uncontended
 * path is pure userspace, unlike CRITICAL_SECTION (which still allocates an
 * event/kernel object lazily under contention) or older Windows mutex
 * primitives.
 *
 * This file only compiles on Windows (uses <windows.h>) - run it on a
 * Windows-based toolchain, not under Linux.
 */

#include <windows.h>

#include <chrono>
#include <iostream>
#include <mutex>

// Minimal Lockable wrapper around SRWLOCK - NOT recursive (SRWLOCK does
// not support recursion at all; recursive acquisition is undefined
// behaviour for SRWLOCK, same restriction std::mutex has).
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

template < typename Mutex >
double bench_uncontended( const char* name )
{
	constexpr int N = 100'000'000;
	Mutex m;
	auto start = std::chrono::high_resolution_clock::now();
	for ( int i = 0; i < N; ++i )
	{
		m.lock();
		m.unlock();
	}
	auto end = std::chrono::high_resolution_clock::now();
	double ns = std::chrono::duration< double, std::nano >( end - start ).count() / N;
	std::cout << name << ": " << ns << " ns/op\n";
	return ns;
}

int main()
{
	bench_uncontended< std::mutex >( "std::mutex" );
	bench_uncontended< std::recursive_mutex >( "std::recursive_mutex" );
	bench_uncontended< SrwMutex >( "SRWLOCK (non-recursive)" );

	std::cout << "\n--- second pass (warm) ---\n";
	bench_uncontended< std::mutex >( "std::mutex" );
	bench_uncontended< std::recursive_mutex >( "std::recursive_mutex" );
	bench_uncontended< SrwMutex >( "SRWLOCK (non-recursive)" );
}
