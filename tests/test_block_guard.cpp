#include "test_helpers.h"

// ---------------------------------------------------------------------------
// BlockGuard and whole-event block()/unblock()
//
// test_advanced_connections.cpp already covers per-connection blocking and
// a basic BlockGuard nesting case for Event specifically.  This file covers
// the parts of whole-event blocking that are easy to leave under-tested:
//
//   - Parity across all three MutexType variants (Event/SharedEvent/
//     SingleThreadedEvent) - SharedEvent's shared-lock path in particular
//     is easy to leave unexercised for whole-event blocking.
//   - BlockGuard's lifetime guarantee: a guard that outlives the event (and
//     its owning Trackable) it was created from must still destruct safely.
//     BlockGuard holds a SharedPtr<EventImplBase> (lifetime only) plus a
//     Callable<void()> bound to the concrete EventImpl's unblock call while
//     the concrete type was still known - this is what makes outliving the
//     event safe.
//   - EventImpl::BlockDepthType is conditionally atomic vs. a plain
//     unsigned int depending on MutexType (NullMutex/SingleThreadedEvent
//     gets the plain counter, since it has no concurrent access to guard
//     against) - confirmed at compile time via static_assert.
// ---------------------------------------------------------------------------

namespace {

template< typename EventT >
struct Sender : public stellyra::Trackable
{
	EventT event{ this };
};

} // namespace

// ---------------------------------------------------------------------------
// Cross-MutexType parity: block()/unblock() and blockGuard() nesting behave
// identically for Event, SharedEvent, and SingleThreadedEvent.
// ---------------------------------------------------------------------------

template< typename EventT >
class BlockGuardAcrossMutexTypes : public ::testing::Test {};

using EventTypes = ::testing::Types<
	stellyra::Event< int >,
	stellyra::SharedEvent< int >,
	stellyra::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( BlockGuardAcrossMutexTypes, EventTypes );

TYPED_TEST( BlockGuardAcrossMutexTypes, BlockUnblock )
{
	Sender< TypeParam > sender;
	int calls = 0;
	sender.event.connectLambda( sender, [ & ]( int ) { calls++; } );

	sender.event.block();
	sender.event( 1 );
	EXPECT_EQ( calls, 0 );

	sender.event.unblock();
	sender.event( 1 );
	EXPECT_EQ( calls, 1 );
}

TYPED_TEST( BlockGuardAcrossMutexTypes, BlockNesting )
{
	Sender< TypeParam > sender;
	int calls = 0;
	sender.event.connectLambda( sender, [ & ]( int ) { calls++; } );

	sender.event.block();
	sender.event.block();
	sender.event( 1 );
	EXPECT_EQ( calls, 0 );

	sender.event.unblock();
	sender.event( 1 );  // still blocked - depth 1
	EXPECT_EQ( calls, 0 );

	sender.event.unblock();
	sender.event( 1 );
	EXPECT_EQ( calls, 1 );
}

TYPED_TEST( BlockGuardAcrossMutexTypes, BlockGuardNesting )
{
	Sender< TypeParam > sender;
	int calls = 0;
	sender.event.connectLambda( sender, [ & ]( int ) { calls++; } );

	{
		auto g1 = sender.event.blockGuard();
		{
			auto g2 = sender.event.blockGuard();
			sender.event( 1 );
			EXPECT_EQ( calls, 0 );
		}

		sender.event( 1 );  // g2 released, still blocked by g1
		EXPECT_EQ( calls, 0 );
	}

	sender.event( 1 );  // both released
	EXPECT_EQ( calls, 1 );
}

// ---------------------------------------------------------------------------
// The lifetime case the SharedPtr in BlockGuard exists for: a guard that
// outlives the event (and its owning Sender) it was created from must still
// destruct safely, for every MutexType.
// ---------------------------------------------------------------------------

TYPED_TEST( BlockGuardAcrossMutexTypes, GuardOutlivesDestroyedEvent )
{
	stellyra::BlockGuard guard = []() {
		auto sender = std::make_unique< Sender< TypeParam > >();
		sender->event.connectLambda( *sender, []( int ) {} );
		return sender->event.blockGuard();
		// sender destroyed here; guard must not dangle
	}();
	// guard destructs here, unblocking an EventImpl kept alive solely by
	// the guard's own SharedPtr - must not crash under ASan.
}

// ---------------------------------------------------------------------------
// Conditional BlockDepthType: compile-time confirmation that NullMutex
// (SingleThreadedEvent) genuinely gets a plain, non-atomic counter rather
// than paying for atomicity it doesn't need, while the other two still do.
// ---------------------------------------------------------------------------

TEST( BlockGuard, BlockDepthTypeIsConditionalOnMutexType )
{
	static_assert(
		std::is_same_v<
			stellyra::BasicEventImpl< stellyra::platform::NullMutex, int >::BlockDepthType,
			unsigned int >,
		"SingleThreadedEvent (NullMutex) should use a plain unsigned int blockDepth" );

	static_assert(
		std::is_same_v<
			stellyra::BasicEventImpl< stellyra::platform::RecursiveMutex, int >::BlockDepthType,
			stellyra::platform::Atomic< unsigned int > >,
		"Event (RecursiveMutex) should use an atomic blockDepth" );

	static_assert(
		std::is_same_v<
			stellyra::BasicEventImpl< stellyra::platform::SharedMutex, int >::BlockDepthType,
			stellyra::platform::Atomic< unsigned int > >,
		"SharedEvent (SharedMutex) should use an atomic blockDepth" );

	SUCCEED();
}
