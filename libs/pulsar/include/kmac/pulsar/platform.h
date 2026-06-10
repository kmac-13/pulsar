#ifndef KMAC_PULSAR_PLATFORM_H
#define KMAC_PULSAR_PLATFORM_H

/**
 * @file platform.h
 * @brief Platform-level type abstractions for the Pulsar event library.
 *
 * Provides mutex types that resolve to real synchronisation primitives when
 * thread safety is enabled, or to zero-overhead no-op stubs when it is
 * disabled.  A single compile-time flag governs the entire library.
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
 * - @c <mutex> and @c <shared_mutex> are not included, allowing compilation
 *   on bare-metal targets where those headers are unavailable
 * - @c std::lock_guard, @c std::unique_lock, and @c std::shared_lock all
 *   compile correctly against the no-op types since the stubs satisfy the
 *   @c Lockable and @c SharedMutex named requirements
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
 */

#ifndef PULSAR_THREAD_SAFE
#	define PULSAR_THREAD_SAFE 1
#endif

#if PULSAR_THREAD_SAFE
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

#else // PULSAR_THREAD_SAFE == 0

// ---------------------------------------------------------------------------
// Single-threaded / bare-metal mode: zero-overhead no-op stubs
//
// <mutex> and <shared_mutex> are NOT included, so this compiles on targets
// where those headers are unavailable.  All lock guards and mutexes are
// empty structs that the compiler optimises away entirely.
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

#endif // PULSAR_THREAD_SAFE

} // namespace platform
} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_PLATFORM_H
