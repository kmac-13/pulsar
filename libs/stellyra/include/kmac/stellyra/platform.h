#ifndef KMAC_STELLYRA_PLATFORM_H
#define KMAC_STELLYRA_PLATFORM_H

/**
 * @file platform.h
 * @brief Platform-level type abstractions for the Stellyra event library.
 *
 * Provides mutex and atomic types that resolve to real synchronisation
 * primitives when thread safety is enabled, or to zero-overhead stubs when
 * it is disabled.  A single compile-time flag governs the entire library.
 * Per-instance locking policies (where individual events opt in or out of
 * thread safety) are not currently supported.
 *
 * @section configuration Configuration
 *
 * Thread safety is **enabled by default**.  To disable it, define
 * `STELLYRA_THREAD_SAFE=0` before including any Stellyra header (typically via
 * a compiler flag or CMakeLists.txt):
 *
 * ```cmake
 * # CMakeLists.txt
 * target_compile_definitions( app PRIVATE STELLYRA_THREAD_SAFE=0 )
 * ```
 *
 * When disabled:
 * - all `platform::Mutex` and `platform::SharedMutex` instances are
 *   zero-size structs with no-op lock/unlock methods
 * - `<mutex>`, `<shared_mutex>`, and `<atomic>` are not included,
 *   allowing compilation on bare-metal targets where those headers are
 *   unavailable
 * - `std::lock_guard`, `std::unique_lock`, and `std::shared_lock` all
 *   compile correctly against the no-op types since the stubs satisfy the
 *   `Lockable` and `SharedMutex` named requirements
 * - `platform::Atomic<T>` uses `volatile` instead of atomic instructions;
 *   safe for single-core bare-metal targets, not for multi-core (see below)
 *
 * @section types Types
 *
 * - **platform::Mutex** - used where a plain exclusive mutex suffices:
 *   Trackable::_mutex and EventLoop::_pendingMutex (both in Stellyra Core);
 *   CombiningEvent::mutex and ConnectionGuard::_mutex (both in stellyra_extras)
 *
 * - **platform::SharedMutex** - used where concurrent reads are beneficial:
 *   EventImpl::mutex when MutexType is SharedMutex (i.e. SharedEvent) -
 *   triggers take a shared lock; connect/disconnect take an exclusive lock
 *
 * - **platform::Atomic<T>** - used for flags and state variables that may
 *   be read or written from multiple threads: EventImplBase::_blockDepth,
 *   EventImplBase::_nextId and Connection::_nextTag (both static, process-
 *   wide counters), EventLoop's drain-state members, Trackable::_eventLoop
 *
 * @section atomic_safety Atomic Safety on Bare-Metal Targets
 *
 * When `STELLYRA_THREAD_SAFE=0`, `platform::Atomic<T>` uses `volatile` T
 * rather than `std::atomic<T>`.  This is sufficient for single-core
 * bare-metal targets where the only concurrency concern is compiler
 * reordering (e.g. an ISR reading a variable the main loop writes).
 * It is @b not safe on multi-core targets.
 *
 * For multi-core bare-metal or RTOS targets that cannot use `std::atomic`,
 * provide a custom `platform::Atomic<T>` specialisation before including
 * any Stellyra header.  See the custom implementation section at the bottom
 * of this file for examples.
 */

#ifndef STELLYRA_THREAD_SAFE
#	define STELLYRA_THREAD_SAFE 1
#endif

#if STELLYRA_THREAD_SAFE
#	include <atomic>
#	include <mutex>
#	include <shared_mutex>
#	include <thread>        // for std::thread::id / std::this_thread::get_id (ThreadId)
#	include <type_traits>
#endif

#include <memory>  // for std::shared_ptr / std::weak_ptr (SharedPtr/WeakPtr aliases)
#include <mutex>   // for std::lock / std::defer_lock (used by both modes' UniqueLock)

namespace kmac {
namespace stellyra {
namespace platform {

#if STELLYRA_THREAD_SAFE

// ---------------------------------------------------------------------------
// Thread-safe mode: delegate to standard library primitives
// ---------------------------------------------------------------------------

/**
 * @brief Exclusive mutex - satisfies the `Lockable` named requirement.
 *
 * Maps to `std::mutex` when `STELLYRA_THREAD_SAFE` is set (the default).
 */
using Mutex = std::mutex;

/**
 * @brief Recursive exclusive mutex - satisfies the `Lockable` named
 * requirement, allows the same thread to re-acquire the lock.
 *
 * Maps to `std::recursive_mutex` when `STELLYRA_THREAD_SAFE` is set.
 *
 * Used by Event::_mutex: triggerImpl() may hold this lock through handler
 * invocation for Direct connections, and a handler may legitimately
 * re-enter connect()/disconnect()/trigger() on the SAME Event during that
 * invocation (see test_reentrancy.cpp). Benchmarked (round 15) to have
 * equal-or-better contended-case performance vs std::mutex on at least one
 * platform (MinGW/Windows) even WITHOUT recursion, with no measured
 * downside - safe as Event::_mutex's type unconditionally.
 *
 * NOT used for EventLoop::_queueMutex, which must remain a plain
 * std::mutex for std::condition_variable compatibility (postEvent() does
 * not re-enter Event's methods, so recursion is not needed there).
 */
using RecursiveMutex = std::recursive_mutex;

/**
 * @brief Shared/exclusive mutex - satisfies the `SharedMutex` named requirement.
 *
 * Maps to `std::shared_mutex` when `STELLYRA_THREAD_SAFE` is set (the default).
 * Allows concurrent shared locks (e.g. multiple simultaneous emissions) while
 * still providing exclusive locks for writes (connect/disconnect).
 */
using SharedMutex = std::shared_mutex;

/**
 * @brief Exclusive lock guard - aliases `std::lock_guard`.
 */
template< typename M >
using LockGuard = std::lock_guard< M >;

/**
 * @brief Unique (exclusive, movable) lock - aliases `std::unique_lock`.
 */
template< typename M >
using UniqueLock = std::unique_lock< M >;

/**
 * @brief Shared (read) lock - aliases `std::shared_lock`.
 */
template< typename M >
using SharedLock = std::shared_lock< M >;

/**
 * @brief Atomic value - aliases `std::atomic<T>`.
 *
 * Provides full memory ordering guarantees for multi-core targets.
 * Use `load()`, `store()`, and `exchange()` with explicit memory
 * orders where ordering matters for correctness.
 */
template< typename T >
using Atomic = std::atomic< T >;

/**
 * @brief Shared-ownership smart pointer - aliases `std::shared_ptr<T>`.
 *
 * Centralised alias so a future pool allocator or custom deleter can be
 * substituted in one place without touching every call site.
 */
template< typename T >
using SharedPtr = std::shared_ptr< T >;

/**
 * @brief Weak reference to a SharedPtr-managed object - aliases `std::weak_ptr<T>`.
 */
template< typename T >
using WeakPtr = std::weak_ptr< T >;

/**
 * @brief Opaque thread identity value.
 *
 * Aliases `std::thread::id` in thread-safe mode.  On embedded targets
 * replace with a platform-native handle (e.g. `TaskHandle_t` on FreeRTOS).
 * @see currentThreadId()
 */
using ThreadId = std::thread::id;

/**
 * @brief Returns the identity of the calling thread.
 * Aliases `std::this_thread::get_id()` in thread-safe mode.
 */
inline ThreadId currentThreadId() noexcept
{
	return std::this_thread::get_id();
}

#else // STELLYRA_THREAD_SAFE == 0

// ---------------------------------------------------------------------------
// Single-threaded / bare-metal mode: zero-overhead no-op stubs
//
// <mutex>, <shared_mutex>, and <atomic> are NOT included, so this compiles
// on targets where those headers are unavailable.  All lock guards and
// mutexes are empty structs that the compiler optimises away entirely.
// ---------------------------------------------------------------------------

/**
 * @brief No-op exclusive mutex for single-threaded / bare-metal targets.
 *
 * Satisfies the `Lockable` named requirement.
 * All methods are no-ops; the struct has no data members (zero size).
 */
struct Mutex
{
	void lock() {}
	void unlock() {}
	bool try_lock() { return true; }
};

using RecursiveMutex = Mutex;

/**
 * @brief No-op shared mutex for single-threaded / bare-metal targets.
 *
 * Satisfies the `SharedMutex` named requirement.
 * All methods are no-ops; the struct has no data members (zero size).
 */
struct SharedMutex
{
	void lock() {}
	void unlock() {}
	bool try_lock() { return true; }
	void lock_shared() {}
	void unlock_shared() {}
	bool try_lock_shared() { return true; }
};

/**
 * @brief No-op exclusive lock guard.
 *
 * Acquires the mutex on construction and releases on destruction,
 * both of which are no-ops for the no-op mutex types.
 */
template< typename M >
struct LockGuard
{
	explicit LockGuard( M& ) {}
};

/**
 * @brief No-op unique (exclusive, movable) lock.
 *
 * Models the standard `Lockable` requirement (lock/try_lock/unlock) and
 * supports deferred construction, so single-threaded code can use the same
 * `std::lock( a, b )` / deferred-then-lock patterns as the thread-safe mode.
 * All operations are no-ops - there is no contention to guard against.
 */
template< typename M >
struct UniqueLock
{
	explicit UniqueLock( M& ) {}
	UniqueLock( M&, std::defer_lock_t ) noexcept {}

	void lock() {}
	void unlock() {}
	bool try_lock() { return true; }
};

/**
 * @brief No-op shared (read) lock.
 */
template< typename M >
struct SharedLock
{
	explicit SharedLock( M& ) {}
};

#ifndef STELLYRA_CUSTOM_ATOMIC
/**
 * @brief Non-atomic volatile value for single-threaded / bare-metal targets.
 *
 * Mirrors the `std::atomic<T>` interface so that code using
 * `platform::Atomic<T>` compiles without modification regardless of
 * whether `STELLYRA_THREAD_SAFE` is set.  Memory order arguments are
 * accepted and silently ignored via variadic overloads - on a single-core
 * target there is no hardware reordering to guard against.
 *
 * Uses `volatile` T internally to prevent the compiler from caching the
 * value in a register, which is sufficient on single-core bare-metal
 * targets to protect against ISR access.
 *
 * Not safe for multi-core targets.  When `STELLYRA_THREAD_SAFE=1`
 *   (the default), `platform::Atomic<T>` maps to `std::atomic<T>` which
 *   provides full multi-core ordering guarantees.  For multi-core
 *   bare-metal targets that cannot use `std::atomic`, define
 *   `STELLYRA_CUSTOM_ATOMIC` and provide your own implementation before
 *   including any Stellyra header - see the custom implementation section
 *   at the bottom of platform.h for examples.
 *
 * @tparam T value type; should be trivially copyable
 */
template< typename T >
class Atomic
{
private:
	volatile T _value;

public:
	Atomic() noexcept : _value() {}
	explicit Atomic( T value ) noexcept : _value( value ) {}

	// non-copyable - same contract as std::atomic
	Atomic( const Atomic& ) = delete;
	Atomic& operator=( const Atomic& ) = delete;

	// -------------------------------------------------------------------------
	// Core operations
	// -------------------------------------------------------------------------

	T load() const noexcept
	{
		return _value;
	}

	void store( T value ) noexcept
	{
		_value = value;
	}

	/**
	 * @brief Read-modify-write: stores `value` and returns the previous value.
	 *
	 * On single-core targets the read and write are not interrupted by other
	 * threads (there are none), and the compiler cannot reorder across volatile
	 * accesses, so this is safe for ISR-vs-main-loop use.
	 */
	T exchange( T value ) noexcept
	{
		T old = _value;
		_value = value;
		return old;
	}

	/**
	 * @brief Compare-and-swap (weak).
	 *
	 * On single-core targets this is always strong - spurious failure is
	 * impossible without hardware-level LL/SC.  Provided for interface
	 * compatibility with `std::atomic`.
	 */
	bool compare_exchange_weak( T& expected, T desired ) noexcept
	{
		if ( _value == expected )
		{
			_value = desired;
			return true;
		}
		expected = _value;
		return false;
	}

	/**
	 * @brief Compare-and-swap (strong) - identical to weak on single-core.
	 */
	bool compare_exchange_strong( T& expected, T desired ) noexcept
	{
		return compare_exchange_weak( expected, desired );
	}

	/**
	 * @brief Read-modify-write add: adds `arg` and returns the previous value.
	 *
	 * Uninterrupted on a single-core target, and volatile prevents the
	 * compiler from reordering across the access, so this is safe for
	 * ISR-vs-main-loop use.  Mirrors `std::atomic<T>::fetch_add`.
	 */
	T fetch_add( T arg ) noexcept
	{
		T old = _value;
		_value = static_cast< T >( old + arg );
		return old;
	}

	/**
	 * @brief Read-modify-write subtract: subtracts `arg` and returns the
	 * previous value.  Mirrors `std::atomic<T>::fetch_sub`.
	 */
	T fetch_sub( T arg ) noexcept
	{
		T old = _value;
		_value = static_cast< T >( old - arg );
		return old;
	}

	// -------------------------------------------------------------------------
	// Memory-order overloads - accept and discard any memory_order arguments
	// so that call sites written for std::atomic compile unchanged
	// -------------------------------------------------------------------------

	template< typename... MemOrder >
	T load( MemOrder&&... ) const noexcept { return load(); }

	template< typename... MemOrder >
	void store( T value, MemOrder&&... ) noexcept { store( value ); }

	template< typename... MemOrder >
	T exchange( T value, MemOrder&&... ) noexcept { return exchange( value ); }

	template< typename... MemOrder >
	bool compare_exchange_weak( T& expected, T desired, MemOrder&&... ) noexcept
	{
		return compare_exchange_weak( expected, desired );
	}

	template< typename... MemOrder >
	bool compare_exchange_strong( T& expected, T desired, MemOrder&&... ) noexcept
	{
		return compare_exchange_strong( expected, desired );
	}

	template< typename... MemOrder >
	T fetch_add( T arg, MemOrder&&... ) noexcept { return fetch_add( arg ); }

	template< typename... MemOrder >
	T fetch_sub( T arg, MemOrder&&... ) noexcept { return fetch_sub( arg ); }

	// pre/post increment and decrement, matching std::atomic
	T operator++() noexcept { return static_cast< T >( fetch_add( 1 ) + 1 ); }
	T operator++( int ) noexcept { return fetch_add( 1 ); }
	T operator--() noexcept { return static_cast< T >( fetch_sub( 1 ) - 1 ); }
	T operator--( int ) noexcept { return fetch_sub( 1 ); }
	T operator+=( T arg ) noexcept { return static_cast< T >( fetch_add( arg ) + arg ); }
	T operator-=( T arg ) noexcept { return static_cast< T >( fetch_sub( arg ) - arg ); }

	// -------------------------------------------------------------------------
	// Convenience operators
	// -------------------------------------------------------------------------

	/**
	 * @brief Implicit conversion - allows `if` ( _destroying ) and similar.
	 */
	operator T() const noexcept { return _value; }

	/**
	 * @brief Assignment - allows `_flag` = true and brace-initialisation.
	 */
	Atomic& operator=( T value ) noexcept { _value = value; return *this; }
};
#endif // STELLYRA_CUSTOM_ATOMIC

/**
 * @brief Shared-ownership smart pointer - aliases `std::shared_ptr<T>`.
 *
 * Provided in the ST block so the alias is available regardless of
 * STELLYRA_THREAD_SAFE.  `std::shared_ptr` is available via `<memory>`.
 */
template< typename T >
using SharedPtr = std::shared_ptr< T >;

/**
 * @brief Weak reference to a SharedPtr-managed object - aliases `std::weak_ptr<T>`.
 */
template< typename T >
using WeakPtr = std::weak_ptr< T >;

/**
 * @brief Dummy thread identity for single-threaded / bare-metal targets.
 *
 * Thread identity is meaningless in single-threaded mode; this stub keeps
 * EventLoop compilable so the same code compiles in both modes.  Replace
 * with a platform-native handle if the target has cooperative tasks.
 */
using ThreadId = int;

/// Always returns 0 in single-threaded mode.
inline ThreadId currentThreadId() noexcept { return 0; }

#endif // STELLYRA_THREAD_SAFE

// ---------------------------------------------------------------------------
// NullMutex: always-no-op regardless of STELLYRA_THREAD_SAFE.
// Used by SingleThreadedEvent to get zero-overhead locking for callers that
// guarantee all accesses occur on a single thread.
// ---------------------------------------------------------------------------

// Satisfies BasicLockable so std::lock_guard<NullMutex> compiles with no-ops.
struct NullMutex
{
	void lock() noexcept {}
	void unlock() noexcept {}
	bool try_lock() noexcept { return true; }
};

} // namespace platform
} // namespace stellyra
} // namespace kmac

// ============================================================================
// Custom Atomic Implementation
//
// For multi-core bare-metal or RTOS targets where std::atomic is unavailable
// but the volatile stub is insufficient, define STELLYRA_CUSTOM_ATOMIC before
// including any Stellyra header and provide your own platform::Atomic<T>
// in the kmac::stellyra::platform namespace.
//
// Defining STELLYRA_CUSTOM_ATOMIC suppresses the built-in volatile stub so
// your implementation is used instead.  It must provide:
//   T load() const noexcept
//   void store(T value) noexcept
//   T exchange(T value) noexcept
//   bool compare_exchange_weak(T& expected, T desired) noexcept
//   bool compare_exchange_strong(T& expected, T desired) noexcept
//   operator T() const noexcept
//   Atomic& operator=(T value) noexcept
//   variadic memory-order overloads for each of the above
//
// Example: ARM Cortex-M with LDREX/STREX
//
//   template< typename T >
//   class kmac::stellyra::platform::Atomic
//   {
//   private:
//       T _value;
//   public:
//       T load() const noexcept
//       {
//           T value;
//           __asm__ volatile( "ldrex %0, [%1]" : "=r"(value) : "r"(&_value) : "memory" );
//           __asm__ volatile( "dmb" ::: "memory" );
//           return value;
//       }
//       T exchange( T desired ) noexcept
//       {
//           T old; uint32_t success;
//           do {
//               __asm__ volatile( "ldrex %0, [%2]" : "=r"(old) : "r"(&_value) : "memory" );
//               __asm__ volatile( "strex %0, %2, [%1]"
//                   : "=&r"(success) : "r"(&_value), "r"(desired) : "memory" );
//           } while ( success != 0 );
//           return old;
//       }
//       // ... store, compare_exchange_weak/strong, operators
//   };
//
// Example: FreeRTOS critical sections
//
//   template< typename T >
//   class kmac::stellyra::platform::Atomic
//   {
//   private:
//       T _value;
//   public:
//       T load() const noexcept
//       {
//           taskENTER_CRITICAL();
//           T value = _value;
//           taskEXIT_CRITICAL();
//           return value;
//       }
//       void store( T value ) noexcept
//       {
//           taskENTER_CRITICAL();
//           _value = value;
//           taskEXIT_CRITICAL();
//       }
//       // ... exchange, compare_exchange_weak/strong, operators
//   };
//
// ============================================================================

#endif // KMAC_STELLYRA_PLATFORM_H
