#ifndef KMAC_PULSAR_PLATFORM_H
#define KMAC_PULSAR_PLATFORM_H

/**
 * @file platform.h
 * @brief Platform-level type abstractions for the Pulsar event library.
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
 * @c PULSAR_THREAD_SAFE=0 before including any Pulsar header (typically via
 * a compiler flag or CMakeLists.txt):
 *
 * @code
 * // CMakeLists.txt
 * target_compile_definitions( app PRIVATE PULSAR_THREAD_SAFE=0 )
 * @endcode
 *
 * When disabled:
 * - all @c platform::Mutex and @c platform::SharedMutex instances are
 *   zero-size structs with no-op lock/unlock methods
 * - @c <mutex>, @c <shared_mutex>, and @c <atomic> are not included,
 *   allowing compilation on bare-metal targets where those headers are
 *   unavailable
 * - @c std::lock_guard, @c std::unique_lock, and @c std::shared_lock all
 *   compile correctly against the no-op types since the stubs satisfy the
 *   @c Lockable and @c SharedMutex named requirements
 * - @c platform::Atomic<T> uses @c volatile instead of atomic instructions;
 *   safe for single-core bare-metal targets, not for multi-core (see below)
 *
 * @section types Types
 *
 * - **platform::Mutex** - used where a plain exclusive mutex suffices:
 *   Object::_connectionsMutex, EventLoop::_queueMutex,
 *   CombiningEvent::_mutex, ConnectionGuard::_mutex
 *
 * - **platform::SharedMutex** - used where concurrent reads are beneficial:
 *   Event::_mutex (emissions take a shared lock; connect/disconnect take an
 *   exclusive lock)
 *
 * - **platform::Atomic<T>** - used for flags and state variables that may
 *   be read or written from multiple threads: Event::_destroying,
 *   ConnectionImpl::_connected, ConnectionImpl::_blocked, etc.
 *
 * @section atomic_safety Atomic Safety on Bare-Metal Targets
 *
 * When @c PULSAR_THREAD_SAFE=0, @c platform::Atomic<T> uses @c volatile T
 * rather than @c std::atomic<T>.  This is sufficient for single-core
 * bare-metal targets where the only concurrency concern is compiler
 * reordering (e.g. an ISR reading a variable the main loop writes).
 * It is @b not safe on multi-core targets.
 *
 * For multi-core bare-metal or RTOS targets that cannot use @c std::atomic,
 * provide a custom @c platform::Atomic<T> specialisation before including
 * any Pulsar header.  See the custom implementation section at the bottom
 * of this file for examples.
 */

#ifndef PULSAR_THREAD_SAFE
#	define PULSAR_THREAD_SAFE 1
#endif

#if PULSAR_THREAD_SAFE
#	include <atomic>
#	include <mutex>
#	include <shared_mutex>
#endif

namespace kmac {
namespace pulsar {
namespace platform {

#if PULSAR_THREAD_SAFE

// ---------------------------------------------------------------------------
// Thread-safe mode: delegate to standard library primitives
// ---------------------------------------------------------------------------

/**
 * @brief Exclusive mutex - satisfies the @c Lockable named requirement.
 *
 * Maps to @c std::mutex when @c PULSAR_THREAD_SAFE is set (the default).
 */
using Mutex = std::mutex;

/**
 * @brief Shared/exclusive mutex - satisfies the @c SharedMutex named requirement.
 *
 * Maps to @c std::shared_mutex when @c PULSAR_THREAD_SAFE is set (the default).
 * Allows concurrent shared locks (e.g. multiple simultaneous emissions) while
 * still providing exclusive locks for writes (connect/disconnect).
 */
using SharedMutex = std::shared_mutex;

/**
 * @brief Exclusive lock guard - aliases @c std::lock_guard.
 */
template< typename M >
using LockGuard = std::lock_guard< M >;

/**
 * @brief Unique (exclusive, movable) lock - aliases @c std::unique_lock.
 */
template< typename M >
using UniqueLock = std::unique_lock< M >;

/**
 * @brief Shared (read) lock - aliases @c std::shared_lock.
 */
template< typename M >
using SharedLock = std::shared_lock< M >;

/**
 * @brief Atomic value - aliases @c std::atomic<T>.
 *
 * Provides full memory ordering guarantees for multi-core targets.
 * Use @c load(), @c store(), and @c exchange() with explicit memory
 * orders where ordering matters for correctness.
 */
template< typename T >
using Atomic = std::atomic< T >;

#else // PULSAR_THREAD_SAFE == 0

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
 * Satisfies the @c Lockable named requirement.
 * All methods are no-ops; the struct has no data members (zero size).
 */
struct Mutex
{
	void lock() {}
	void unlock() {}
	bool try_lock() { return true; }
};

/**
 * @brief No-op shared mutex for single-threaded / bare-metal targets.
 *
 * Satisfies the @c SharedMutex named requirement.
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
 */
template< typename M >
struct UniqueLock
{
	explicit UniqueLock( M& ) {}
};

/**
 * @brief No-op shared (read) lock.
 */
template< typename M >
struct SharedLock
{
	explicit SharedLock( M& ) {}
};

/**
 * @brief Non-atomic volatile value for single-threaded / bare-metal targets.
 *
 * Mirrors the @c std::atomic<T> interface so that code using
 * @c platform::Atomic<T> compiles without modification regardless of
 * whether @c PULSAR_THREAD_SAFE is set.  Memory order arguments are
 * accepted and silently ignored via variadic overloads - on a single-core
 * target there is no hardware reordering to guard against.
 *
 * Uses @c volatile T internally to prevent the compiler from caching the
 * value in a register, which is sufficient on single-core bare-metal
 * targets to protect against ISR access.
 *
 * @warning Not safe for multi-core targets.  When @c PULSAR_THREAD_SAFE=1
 *   (the default), @c platform::Atomic<T> maps to @c std::atomic<T> which
 *   provides full multi-core ordering guarantees.  For multi-core
 *   bare-metal targets that cannot use @c std::atomic, define
 *   @c PULSAR_CUSTOM_ATOMIC and provide your own implementation before
 *   including any Pulsar header - see the custom implementation section
 *   at the bottom of platform.h for examples.
 *
 * @tparam T value type; should be trivially copyable
 */
#ifndef PULSAR_CUSTOM_ATOMIC
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
	 * @brief Read-modify-write: stores @p value and returns the previous value.
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
	 * compatibility with @c std::atomic.
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

	// -------------------------------------------------------------------------
	// Convenience operators
	// -------------------------------------------------------------------------

	/**
	 * @brief Implicit conversion - allows @c if ( _destroying ) and similar.
	 */
	operator T() const noexcept { return _value; }

	/**
	 * @brief Assignment - allows @c _flag = true and brace-initialisation.
	 */
	Atomic& operator=( T value ) noexcept { _value = value; return *this; }
};
#endif // PULSAR_CUSTOM_ATOMIC

#endif // PULSAR_THREAD_SAFE

} // namespace platform
} // namespace pulsar
} // namespace kmac

// ============================================================================
// Custom Atomic Implementation
//
// For multi-core bare-metal or RTOS targets where std::atomic is unavailable
// but the volatile stub is insufficient, define PULSAR_CUSTOM_ATOMIC before
// including any Pulsar header and provide your own platform::Atomic<T>
// in the kmac::pulsar::platform namespace.
//
// Defining PULSAR_CUSTOM_ATOMIC suppresses the built-in volatile stub so
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
//   class kmac::pulsar::platform::Atomic
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
//   class kmac::pulsar::platform::Atomic
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

#endif // KMAC_PULSAR_PLATFORM_H
