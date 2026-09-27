// ===========================================================================
// Behaviours specific to the stable-slot dispatch engine: lazy priority
// ordering (activation and reversion), priority order maintained across
// reentrant churn, and generation-based staleness after slot reuse.
//
// Split into two typed suites:
//   - StableSlots (AllEventTypes): no reentrant connect/disconnect from
//     within dispatch, safe across Event/SharedEvent/SingleThreadedEvent.
//   - StableSlotsReentrant (Event/SingleThreadedEvent only): the two tests
//     that connect or self-disconnect from within a handler invoked by that
//     same event's own dispatch.  SharedEvent's shared-lock dispatch path
//     doesn't support this (would need the exclusive lock while already
//     holding the shared one - see test_pending_removal_stress.cpp for the
//     confirmed deadlock), so it's excluded here rather than included and
//     hung.
// ===========================================================================

#include "test_helpers.hpp"

#include <vector>

template< typename EventT >
class StableSlots : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SharedEvent< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( StableSlots, EventTypes );

TYPED_TEST( StableSlots, PriorityActivatesAndRevertsToFastPath )
{
	TypeParam ev;
	std::vector< int > order;

	// all default priority: plain slot-order fast path
	ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); } );
	ev.connectLambda( [ &order ]( int ) { order.push_back( 2 ); } );
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 1, 2 } ) );

	// a non-default priority activates the ordered path
	order.clear();
	auto hi = ev.connectLambda( [ &order ]( int ) { order.push_back( 9 ); },
		pulsar::ConnectionType::Auto, 9 );
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 9, 1, 2 } ) );

	// removing it reverts to the fast path, remaining handlers intact
	order.clear();
	hi.disconnect();
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 1, 2 } ) );
}

TYPED_TEST( StableSlots, PriorityOrderSurvivesChurn )
{
	TypeParam ev;
	std::vector< int > order;

	ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 1 ) );
	ev.connectLambda( [ &order ]( int ) { order.push_back( 5 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 5 ) );
	auto p3 = ev.connectLambda( [ &order ]( int ) { order.push_back( 3 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 3 ) );

	order.clear();
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 5, 3, 1 } ) );

	// churn: drop the middle priority, add two more; order must stay by prio
	p3.disconnect();
	ev.connectLambda( [ &order ]( int ) { order.push_back( 7 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 7 ) );
	ev.connectLambda( [ &order ]( int ) { order.push_back( 2 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 2 ) );

	order.clear();
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 7, 5, 2, 1 } ) );
}

TYPED_TEST( StableSlots, StaleConnectionAfterSlotReuseIsIgnored )
{
	// disconnecting frees a slot; a new connection reuses it with a bumped
	// generation, so the old Connection handle must read as disconnected and
	// its operations must be no-ops on the new occupant
	TypeParam ev;
	int firstHits = 0;
	int secondHits = 0;

	auto first = ev.connectLambda( [ &firstHits ]( int ) { ++firstHits; } );
	first.disconnect();

	auto second = ev.connectLambda( [ &secondHits ]( int ) { ++secondHits; } );  // reuses the slot

	EXPECT_FALSE( first.isConnected() );
	EXPECT_TRUE( second.isConnected() );

	// operating through the stale handle must not touch the new occupant
	first.disconnect();
	first.block();

	ev( 0 );
	EXPECT_EQ( firstHits, 0 );
	EXPECT_EQ( secondHits, 1 );  // unaffected by stale-handle operations
	EXPECT_TRUE( second.isConnected() );
}

// ---------------------------------------------------------------------------
// Reentrant connect/self-disconnect during priority dispatch - Event and
// SingleThreadedEvent only (see file header).
// ---------------------------------------------------------------------------

template< typename EventT >
class StableSlotsReentrant : public ::testing::Test {};

using ReentrantEventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( StableSlotsReentrant, ReentrantEventTypes );

TYPED_TEST( StableSlotsReentrant, ReentrantConnectWithPriorityDefersToNextEmission )
{
	// a handler connected (with priority) from within a firing handler must
	// not fire in the current emission, and must land in priority order on
	// the next one
	TypeParam ev;
	std::vector< int > order;
	bool added = false;

	ev.connectLambda( [ & ]( int ) {
		order.push_back( 1 );
		if ( ! added )
		{
			added = true;
			ev.connectLambda( [ &order ]( int ) { order.push_back( 99 ); },
				pulsar::ConnectionType::Auto, ev.params().prio( 100 ) );
		}
	} );

	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 1 } ) );  // reentrant add excluded

	order.clear();
	ev( 0 );
	// now active and highest priority -> fires first
	EXPECT_EQ( order, ( std::vector< int >{ 99, 1 } ) );
}

TYPED_TEST( StableSlotsReentrant, ReentrantDisconnectDuringPriorityDispatch )
{
	// a once-style self-disconnect from a priority handler mid-dispatch is
	// applied cleanly, and ordering is correct afterwards
	TypeParam ev;
	std::vector< int > order;
	pulsar::Connection self;

	self = ev.connectLambda( [ & ]( int ) {
		order.push_back( 5 );
		self.disconnect();
	}, pulsar::ConnectionType::Auto, ev.params().prio( 5 ) );

	ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); },
		pulsar::ConnectionType::Auto, ev.params().prio( 1 ) );

	order.clear();
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 5, 1 } ) );  // both fire, prio 5 first

	order.clear();
	ev( 0 );
	EXPECT_EQ( order, ( std::vector< int >{ 1 } ) );  // self-disconnected handler gone
	EXPECT_FALSE( self.isConnected() );
}

TYPED_TEST( StableSlotsReentrant, LastPriorityConnectionDisconnectingMidDispatchCollapsesOrder )
{
	// disconnecting the ONLY non-default-priority connection from within its
	// own handler mid-dispatch drops priorityConnCount to 0, that collapse
	// deferred (like all reentrant topology changes) until the outer trigger
	// unwinds - reconcileAfterTrigger() -> recomputeOrdering() is what
	// actually clears the priority-ordering `order` list and reverts the
	// event to the plain slot-order fast path; this confirms that that
	// collapse actually happens rather than leaving order half-torn-down
	TypeParam ev;
	std::vector< int > order;

	// the only priority connection - slot 0
	pulsar::Connection self = ev.connectLambda( [ & ]( int ) {
		order.push_back( 100 );
		self.disconnect();
	}, pulsar::ConnectionType::Auto, ev.params().prio( 5 ) );

	auto c1 = ev.connectLambda( [ &order ]( int ) { order.push_back( 1 ); } );  // slot 1, default priority
	ev.connectLambda( [ &order ]( int ) { order.push_back( 2 ); } );            // slot 2, default priority

	order.clear();
	ev( 0 );
	// order list is active (priorityConnCount == 1): walks slot 0 (prio 5),
	// then slots 1/2 (prio 0, tie broken by slot index) - self fires first,
	// then self-disconnects (deferred - order isn't touched mid-walk)
	EXPECT_EQ( order, ( std::vector< int >{ 100, 1, 2 } ) );

	// by the time ev(0) above returned, reconcileAfterTrigger() already ran:
	// priorityConnCount recomputed to 0 (no active handler has non-zero
	// priority anymore) -> order cleared -> nextFree rescanned to slot 0
	// (self's now-inactive slot, the earliest one)
	c1.disconnect();  // frees slot 1; nextFree stays at 0 (1 is not < 0)

	// reuses slot 0 - the earliest free slot, which is self's old slot, NOT
	// c1's just-freed slot 1 - if this landed in slot 1 instead, or if
	// dispatch order below came out wrong, that would mean order/nextFree
	// were not correctly torn down and rescanned after the collapse
	ev.connectLambda( [ &order ]( int ) { order.push_back( 3 ); } );

	order.clear();
	ev( 0 );
	// plain fast-path slot walk, since priorityConnCount is 0: slot 0
	// ("3", reused), slot 1 (inactive - c1 - skipped), slot 2 ("2")
	EXPECT_EQ( order, ( std::vector< int >{ 3, 2 } ) );
}
