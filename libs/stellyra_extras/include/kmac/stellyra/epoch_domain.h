#pragma once
#ifndef KMAC_STELLYRA_EPOCH_DOMAIN_H
#define KMAC_STELLYRA_EPOCH_DOMAIN_H

/**
 * @brief Upper bound on concurrently-active enter() calls at any one instant,
 * across all threads, including reentrant nesting - NOT total connection
 * count, and NOT total thread count over the program's lifetime.  Fixed at
 * compile time to use std::array (not std::vector) so the reader-slot pool
 * lives inline inside EpochDomain rather than as a separate heap allocation -
 * one fewer allocation per BasicConcurrentEventImpl, and the pool's memory is
 * part of the same block as everything else in it.
 *
 * Define this before including this header to override the default; applies
 * program-wide to every EpochDomain, matching the standard "define before
 * include" idiom for compile-time library tunables.
 *
 * Exceeding it asserts loudly in EpochDomain::enter() rather than corrupting
 * anything silently - raise it if your real concurrent- dispatch depth could
 * exceed the default.
 */
#ifndef STELLYRA_MAX_CONCURRENT_TRIGGERS
#define STELLYRA_MAX_CONCURRENT_TRIGGERS 32
#endif

/**
 * @file epoch_domain.h
 * @brief EpochDomain - epoch-based reclamation for a whole critical section,
 * not a single pointer.  Each reader indicates they are active once for the
 * duration of an entire dispatch() call, and everything touched inside that
 * call - including any number of independent per-slot atomic pointers - is
 * protected by that single announcement.  Reclamation is the writer's job:
 * retire() defers freeing until it can prove no reader could
 * still be inside a critical section that started before the retired
 * object was removed.
 *
 * EpochDomain pays its enter/exit cost ONCE per whole dispatch() call, then
 * every per-slot atomic<Entry*> read inside is a single plain atomic load -
 * the epoch guard already covers it.
 *
 * Protocol - standard 3-epoch Epoch-Based Reclamation (EBR), e.g. Fraser
 * 2004, simplified to a single global epoch counter:
 * - a fixed pool of reader slots, each holding either FREE or the epoch
 *   value the reader observed on entry; enter() claims a free slot via
 *   one CAS combined with publishing the current epoch
 * - retire(obj) files obj into the bin for the CURRENT epoch, then tries to
 *   advance: if every currently-active reader slot already shows the current
 *   epoch (none lagging behind), the epoch can safely advance, and the bin
 *   from two epochs back can be freed - by that point, any reader that could
 *   have been active during that older epoch has necessarily either exited
 *   or been forced to observe a later epoch, because the immediately
 *   preceding advance already checked for exactly that
 * - if a reader is genuinely stuck (never exits), every advance attempt from
 *   that point on fails and retire bins grow without bound until it does exit;
 *   this is the classic, expected epoch-reclamation failure mode - bounded
 *   here to at most the duration of one dispatch() call, since enter() is
 *   claimed per-call, not registered once per thread for the thread's whole
 *   lifetime
 *
 * One optimization layered onto the base protocol, added after initial
 * validation showed where real cost remained - a high-water mark bounds
 * retire()'s scan to reader slots that have actually been claimed at least
 * once, not the full configured pool size.  Monotonic: once raised, it never
 * lowers, even if concurrency later drops. retire() cost is flat regardless
 * of pool capacity when only a handful of slots are ever actually used,
 * instead of scaling with the configured pool size.
 *
 * Note that a second optimization - a "next free" hint for enter()'s claim
 * step - was attempted, but was only an improvement for single-threaded
 * processing whereas for higher thread counts it caused a substantial
 * regression on real multi-core hardware - over 2x slower at 4 threads, worst
 * specifically at low-to-moderate thread counts rather than growing with
 * thread count, which points to a contention problem, not a fixed-cost one.
 * A single shared hint concentrates every concurrent enter() call's first CAS
 * attempt onto the exact same cache line, so instead of threads naturally
 * spreading across different slots the way independent scans starting from
 * the same index still tend to (only one succeeds per contested index, the
 * rest move on), a shared hint makes ALL of them contend for the identical
 * slot first - worse than no hint at all under real cross-core contention.
 * Lesson: a single shared "next free" pointer/index is a known anti-pattern
 * in lock-free design for exactly this reason.  A per-thread or randomized
 * starting offset would avoid the convergence, but wasn't pursued further
 * given the base protocol already performs well.
 *
 * Reclaiming more than one type through one domain: retire() is type-erased
 * internally (stores a `void*` plus a small deleter function pointer, not
 * std::function) specifically so a single domain can retire more than one
 * distinct object type - e.g. BasicConcurrentEventImpl's per-handler Entry
 * objects AND its growable slot array's backing storage - through the SAME
 * epoch counter.  One dispatch() enter() call then protects both, rather
 * than needing two separate epoch domains (and two separate enter() calls,
 * doubling that cost) for what is, from a reclamation point of view, the
 * same problem applied to two different object types.  A templated
 * retire(T*) convenience overload is kept for the common single-type case,
 * and is what the existing standalone test suite already uses.
 */

#include <kmac/stellyra/stellyra_assert.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace kmac {
namespace stellyra {

// ===========================================================================
// EpochDomain
// ===========================================================================

class EpochDomain
{
public:
	static constexpr uint64_t FREE = UINT64_MAX;

	using Deleter = void (*)( void* );

private:
	struct RetiredItem
	{
		void* obj;
		Deleter deleter;
	};

	mutable std::array< std::atomic< uint64_t >, STELLYRA_MAX_CONCURRENT_TRIGGERS > _readerSlots;
	mutable std::atomic< size_t > _highWaterMark{ 0 };  // bounds retire()'s scan to slots ever actually claimed
	std::atomic< uint64_t > _epoch{ 0 };
	std::vector< RetiredItem > _retireBins[ 3 ];  // writer-only, under the caller's own lock

public:
	// =======================================================================
	// Guard
	// =======================================================================

	/**
	 * @brief RAII handle representing "I am active in the epoch observed
	 * at construction time."  Move-only.  Releasing (on destruction) marks
	 * the claimed slot free again.
	 */
	class Guard
	{
		friend class EpochDomain;

	private:
		std::atomic< uint64_t >* _slot = nullptr;

		/**
		 * @brief Construct a guard for an already-claimed reader slot.
		 */
		explicit Guard( std::atomic< uint64_t >* slot );

	public:
		Guard() = default;

		/**
		 * @brief Move-construct a guard from another guard.
		 *
		 * @param other guard whose reader slot is transferred
		 */
		Guard( Guard&& other ) noexcept;

		/**
		 * @brief Release the reader slot held by this guard.
		 */
		~Guard();

		Guard( const Guard& ) = delete;
		Guard& operator=( const Guard& ) = delete;

		/**
		 * @brief Move-assign a guard from another guard.
		 *
		 * @param other guard whose reader slot is transferred
		 * @return this guard
		 */
		Guard& operator=( Guard&& other ) noexcept;

	private:
		/// Mark the claimed reader slot as free.
		void release();
	};

	/**
	 * @brief Pool size is fixed at compile time via
	 * STELLYRA_MAX_CONCURRENT_TRIGGERS (see top of file) - no runtime
	 * parameter, since std::array's size can't vary per instance.
	 */
	EpochDomain();

	/**
	 * @brief Destroy the domain and release all retired objects.
	 *
	 * The caller must ensure that no enter() operation is currently active
	 * .
	 */
	~EpochDomain();

	EpochDomain( const EpochDomain& ) = delete;
	EpochDomain& operator=( const EpochDomain& ) = delete;

	// =======================================================================
	// Reader-side API
	// =======================================================================

	/**
	 * @brief Enter a read-side critical section.  Everything read through
	 * any atomic<T*> (or similar) for as long as the returned Guard is
	 * alive is protected - no per-object protection needed inside.
	 */
	Guard enter() const;

	/**
	 * @brief Retire an object and attempt to advance the epoch and reclaim
	 * anything now provably safe.
	 *
	 * @param obj object that is no longer reachable from any atomic pointer
	 */
	template< typename T >
	void retire( T* obj );

	/**
	 * @brief Type-erased retire, for callers (like BasicConcurrentEventImpl's
	 * growable slot array) that need to reclaim a type other than the
	 * domain's "primary" one through this same epoch counter.
	 *
	 * @param obj object that is no longer reachable from any atomic pointer
	 * @param deleter function used to destroy the object
	 */
	void retireErased( void* obj, Deleter deleter );

private:
	/**
	 * @brief Claim a reader slot and update the high-water mark.
	 */
	Guard claim( size_t index ) const;

	/**
	 * @brief Advance the epoch and reclaim the bin that is now provably safe.
	 */
	void tryAdvanceAndReclaim( uint64_t e );
};


// ===========================================================================
// EpochDomain
// ===========================================================================

inline EpochDomain::EpochDomain()
{
	for ( auto& slot : _readerSlots )
	{
		slot.store( FREE, std::memory_order_relaxed );
	}
}

inline EpochDomain::~EpochDomain()
{
	// caller guarantees no enter() is active before destruction
	for ( auto& bin : _retireBins )
	{
		for ( const RetiredItem& item : bin )
		{
			item.deleter( item.obj );
		}
	}
}


// ===========================================================================
// EpochDomain::enter / retire
// ===========================================================================

inline EpochDomain::Guard EpochDomain::enter() const
{
	const uint64_t e = _epoch.load( std::memory_order_acquire );
	const size_t bound = _readerSlots.size();

	constexpr int MAX_SPIN_ROUNDS = 1000;
	for ( int round = 0; round < MAX_SPIN_ROUNDS; ++round )
	{
		for ( size_t i = 0; i < bound; ++i )
		{
			uint64_t expected = FREE;
			if ( _readerSlots[ i ].compare_exchange_strong( expected, e,
				std::memory_order_acq_rel, std::memory_order_relaxed ) )
			{
				return claim( i );
			}
		}
		std::this_thread::yield();
	}

	STELLYRA_ASSERT_ALWAYS( false, "EpochDomain: reader slot pool exhausted - increase readerSlotCount" );
	return Guard();
}

template< typename T >
inline void EpochDomain::retire( T* obj )
{
	if ( obj == nullptr )
	{
		return;
	}

	retireErased( obj, []( void* p ) { delete static_cast< T* >( p ); } );
}

inline void EpochDomain::retireErased( void* obj, Deleter deleter )
{
	if ( obj == nullptr )
	{
		return;
	}

	const uint64_t e = _epoch.load( std::memory_order_relaxed );
	_retireBins[ e % 3 ].push_back( RetiredItem{ obj, deleter } );

	tryAdvanceAndReclaim( e );
}


// ===========================================================================
// EpochDomain private helpers
// ===========================================================================

inline EpochDomain::Guard EpochDomain::claim( size_t index ) const
{
	size_t hwm = _highWaterMark.load( std::memory_order_relaxed );
	while ( hwm < index + 1
		&& ! _highWaterMark.compare_exchange_weak(
			hwm,
			index + 1,
			std::memory_order_relaxed,
			std::memory_order_relaxed ) )
	{
		// loop: high water mark was updated by the failed CAS to the latest value; retry
	}

	return Guard( &_readerSlots[ index ] );
}

inline void EpochDomain::tryAdvanceAndReclaim( uint64_t e )
{
	const size_t bound = _highWaterMark.load( std::memory_order_relaxed );
	for ( size_t i = 0; i < bound; ++i )
	{
		const uint64_t v = _readerSlots[ i ].load( std::memory_order_acquire );
		if ( v != FREE && v != e )
		{
			// some reader is still active from an older epoch - can't
			// advance yet, try again on the next retire()
			return;
		}
	}

	const uint64_t newEpoch = e + 1;
	_epoch.store( newEpoch, std::memory_order_release );

	// two epochs back from the new epoch is now provably unreachable:
	// the advance that just succeeded confirms no reader is still
	// active at `e` or earlier, and no reader can newly observe an
	// epoch older than what _epoch.load() returns going forward
	auto& reclaimBin = _retireBins[ ( newEpoch + 1 ) % 3 ];
	for ( const RetiredItem& item : reclaimBin )
	{
		item.deleter( item.obj );
	}
	reclaimBin.clear();
}


// ===========================================================================
// EpochDomain::Guard
// ===========================================================================

inline EpochDomain::Guard::Guard( std::atomic< uint64_t >* slot ) :
	_slot( slot )
{
}

inline EpochDomain::Guard::Guard( EpochDomain::Guard&& other ) noexcept
	: _slot( other._slot )
{
	other._slot = nullptr;
}

inline EpochDomain::Guard::~Guard()
{
	release();
}

inline EpochDomain::Guard&
EpochDomain::Guard::operator=( EpochDomain::Guard&& other ) noexcept
{
	if ( this != &other )
	{
		release();
		_slot = other._slot;
		other._slot = nullptr;
	}
	return *this;
}

inline void EpochDomain::Guard::release()
{
	if ( _slot != nullptr )
	{
		_slot->store( EpochDomain::FREE, std::memory_order_release );
		_slot = nullptr;
	}
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_EPOCH_DOMAIN_H
