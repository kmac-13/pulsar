#include "test_helpers.h"

// ---------------------------------------------------------------------------
// Combiners
//
// Covers the reducer strategies in combiners.h beyond LogicalAnd/LogicalOr
// (already exercised in test_combining_and_groups.cpp): Sum, Product, Mean
// (and its Average alias), Maximum, Minimum, CountTrue, First,
// FirstNonDefault, Last, LastNonDefault.
//
// Every combiner is tested against both a normal multi-handler case and the
// empty-range case (zero connected handlers), since every combiner's doc
// comment makes an explicit claim about empty-range behaviour.
//
// A note on "priority" in the source comments: First/FirstNonDefault/Last/
// LastNonDefault's doc comments describe results in terms of handler
// "priority" ("the first handler executed has the highest priority").
// CombiningEvent has no priority mechanism at all, though - no prio()/
// ConnParams overload appears anywhere in its connect API.  "first"/"last"
// here mean connection order: CombiningEventImpl maintains a separate
// `order` list of slot indices in the order connections were made (see
// CombiningEventImpl::order), walked at emit() time instead of iterating
// `handlers` by raw slot index - so a later connection that happens to
// reuse an earlier, now-freed slot is still combined in its true
// chronological position, not wherever it landed physically.  Verified
// directly below (see the "slot reuse" section near the end of this file).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Sum
// ---------------------------------------------------------------------------

TEST( Combiners, SumAddsAllReturnValues )
{
	stellyra::CombiningEvent< stellyra::Combiners::Sum< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int x ) { return x; } );
	ev.connectLambda( []( int x ) { return x * 2; } );
	ev.connectLambda( []( int x ) { return x * 3; } );

	EXPECT_EQ( ev.emit( 10 ), 60 );  // 10 + 20 + 30
}

TEST( Combiners, SumOfEmptyRangeIsZero )
{
	stellyra::CombiningEvent< stellyra::Combiners::Sum< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 42 ), 0 );  // T{} for int
}

// ---------------------------------------------------------------------------
// Product
// ---------------------------------------------------------------------------

TEST( Combiners, ProductMultipliesAllReturnValues )
{
	stellyra::CombiningEvent< stellyra::Combiners::Product< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int x ) { return x; } );
	ev.connectLambda( []( int x ) { return x + 1; } );
	ev.connectLambda( []( int x ) { return x + 2; } );

	EXPECT_EQ( ev.emit( 2 ), 24 );  // 2 * 3 * 4
}

TEST( Combiners, ProductOfEmptyRangeIsOne )
{
	stellyra::CombiningEvent< stellyra::Combiners::Product< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 99 ), 1 );  // T{1}, not T{} - the multiplicative identity
}

// ---------------------------------------------------------------------------
// Mean (and its Average alias)
// ---------------------------------------------------------------------------

TEST( Combiners, MeanAveragesAllReturnValues )
{
	stellyra::CombiningEvent< stellyra::Combiners::Mean< double >, double, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 10.0; } );
	ev.connectLambda( []( int ) { return 20.0; } );
	ev.connectLambda( []( int ) { return 30.0; } );

	EXPECT_DOUBLE_EQ( ev.emit( 0 ), 20.0 );
}

TEST( Combiners, MeanOfEmptyRangeIsZero )
{
	stellyra::CombiningEvent< stellyra::Combiners::Mean< double >, double, int > ev{ nullptr };
	EXPECT_DOUBLE_EQ( ev.emit( 0 ), 0.0 );
}

TEST( Combiners, AverageAliasBehavesIdenticallyToMean )
{
	// Average<T> is a template alias for Mean<T> - same type, just confirms
	// it's usable as a CombiningEvent's Combiner and produces the same result
	stellyra::CombiningEvent< stellyra::Combiners::Average< double >, double, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 4.0; } );
	ev.connectLambda( []( int ) { return 8.0; } );

	EXPECT_DOUBLE_EQ( ev.emit( 0 ), 6.0 );
}

// ---------------------------------------------------------------------------
// Maximum / Minimum
// ---------------------------------------------------------------------------

TEST( Combiners, MaximumReturnsLargestValue )
{
	stellyra::CombiningEvent< stellyra::Combiners::Maximum< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 5; } );
	ev.connectLambda( []( int ) { return 9; } );
	ev.connectLambda( []( int ) { return 3; } );

	EXPECT_EQ( ev.emit( 0 ), 9 );
}

TEST( Combiners, MaximumOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::Maximum< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 1 ), 0 );
}

TEST( Combiners, MinimumReturnsSmallestValue )
{
	stellyra::CombiningEvent< stellyra::Combiners::Minimum< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 5; } );
	ev.connectLambda( []( int ) { return 9; } );
	ev.connectLambda( []( int ) { return 3; } );

	EXPECT_EQ( ev.emit( 0 ), 3 );
}

TEST( Combiners, MinimumOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::Minimum< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 1 ), 0 );
}

// ---------------------------------------------------------------------------
// CountTrue
// ---------------------------------------------------------------------------

TEST( Combiners, CountTrueCountsTrueResults )
{
	stellyra::CombiningEvent< stellyra::Combiners::CountTrue, int, int > ev{ nullptr };

	ev.connectLambda( []( int x ) { return x > 0; } );
	ev.connectLambda( []( int x ) { return x > 5; } );
	ev.connectLambda( []( int x ) { return x > 10; } );

	EXPECT_EQ( ev.emit( 7 ), 2 );  // > 0 and > 5, not > 10
}

TEST( Combiners, CountTrueOfEmptyRangeIsZero )
{
	stellyra::CombiningEvent< stellyra::Combiners::CountTrue, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 7 ), 0 );
}

// ---------------------------------------------------------------------------
// First / FirstNonDefault
//
// "First"/"Last" here mean connection order (see file header) - handlers
// below are connected in the order their comments list them, and that
// connection order is what determines which result each combiner picks.
// ---------------------------------------------------------------------------

TEST( Combiners, FirstReturnsFirstConnectedHandlersValue )
{
	stellyra::CombiningEvent< stellyra::Combiners::First< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 100; } );  // connected first
	ev.connectLambda( []( int ) { return 200; } );
	ev.connectLambda( []( int ) { return 300; } );

	EXPECT_EQ( ev.emit( 0 ), 100 );
}

TEST( Combiners, FirstOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::First< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 0 ), 0 );
}

TEST( Combiners, FirstNonDefaultSkipsLeadingDefaultResults )
{
	stellyra::CombiningEvent< stellyra::Combiners::FirstNonDefault< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 0; } );  // default - skipped
	ev.connectLambda( []( int ) { return 0; } );  // default - skipped
	ev.connectLambda( []( int ) { return 5; } );  // first non-default
	ev.connectLambda( []( int ) { return 7; } );  // never reached

	EXPECT_EQ( ev.emit( 0 ), 5 );
}

TEST( Combiners, FirstNonDefaultAllDefaultReturnsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::FirstNonDefault< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 0; } );
	ev.connectLambda( []( int ) { return 0; } );

	EXPECT_EQ( ev.emit( 0 ), 0 );
}

TEST( Combiners, FirstNonDefaultOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::FirstNonDefault< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 0 ), 0 );
}

// ---------------------------------------------------------------------------
// Last / LastNonDefault
// ---------------------------------------------------------------------------

TEST( Combiners, LastReturnsLastConnectedHandlersValue )
{
	stellyra::CombiningEvent< stellyra::Combiners::Last< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 100; } );
	ev.connectLambda( []( int ) { return 200; } );
	ev.connectLambda( []( int ) { return 300; } );  // connected last

	EXPECT_EQ( ev.emit( 0 ), 300 );
}

TEST( Combiners, LastOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::Last< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 0 ), 0 );
}

TEST( Combiners, LastNonDefaultSkipsTrailingDefaultResults )
{
	stellyra::CombiningEvent< stellyra::Combiners::LastNonDefault< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 5; } );  // first non-default (not picked)
	ev.connectLambda( []( int ) { return 7; } );  // last non-default - picked
	ev.connectLambda( []( int ) { return 0; } );  // default - skipped
	ev.connectLambda( []( int ) { return 0; } );  // default - skipped

	EXPECT_EQ( ev.emit( 0 ), 7 );
}

TEST( Combiners, LastNonDefaultAllDefaultReturnsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::LastNonDefault< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 0; } );
	ev.connectLambda( []( int ) { return 0; } );

	EXPECT_EQ( ev.emit( 0 ), 0 );
}

TEST( Combiners, LastNonDefaultOfEmptyRangeIsDefault )
{
	stellyra::CombiningEvent< stellyra::Combiners::LastNonDefault< int >, int, int > ev{ nullptr };
	EXPECT_EQ( ev.emit( 0 ), 0 );
}

// ---------------------------------------------------------------------------
// First vs Last consistency: connecting the same three handlers, First and
// Last must disagree (picking opposite ends of the same connection order),
// confirming both combiners are reading the same underlying order rather
// than e.g. both silently defaulting to slot-index-0 due to a copy/paste bug.
// ---------------------------------------------------------------------------

TEST( Combiners, FirstAndLastDisagreeOnMultipleHandlers )
{
	stellyra::CombiningEvent< stellyra::Combiners::First< int >, int, int > firstEv{ nullptr };
	stellyra::CombiningEvent< stellyra::Combiners::Last< int >, int, int > lastEv{ nullptr };

	auto h1 = []( int ) { return 1; };
	auto h2 = []( int ) { return 2; };
	auto h3 = []( int ) { return 3; };

	firstEv.connectLambda( h1 );
	firstEv.connectLambda( h2 );
	firstEv.connectLambda( h3 );

	lastEv.connectLambda( h1 );
	lastEv.connectLambda( h2 );
	lastEv.connectLambda( h3 );

	EXPECT_EQ( firstEv.emit( 0 ), 1 );
	EXPECT_EQ( lastEv.emit( 0 ), 3 );
}

// ---------------------------------------------------------------------------
// Slot reuse must not affect connection order
//
// CombiningEventImpl reuses a disconnected connection's storage slot for the
// next new connection (the same slot-reuse strategy BasicEvent's EventImpl
// uses), so slot index alone is not chronological connection order once a
// disconnect has happened.  Every combiner here is defined in terms of
// connection order specifically so that reused slots don't scramble the
// result - verified directly by forcing a slot-reuse scenario and confirming
// the combiners still read in the order connections were actually made.
// ---------------------------------------------------------------------------

TEST( Combiners, FirstSurvivesSlotReuse )
{
	stellyra::CombiningEvent< stellyra::Combiners::First< int >, int, int > ev{ nullptr };

	auto c1 = ev.connectLambda( []( int ) { return 1; } );  // slot 0
	ev.connectLambda( []( int ) { return 2; } );            // slot 1
	ev.connectLambda( []( int ) { return 3; } );            // slot 2

	c1.disconnect();  // frees slot 0

	// connected chronologically AFTER handlers 2 and 3, but physically
	// lands in the freed slot 0 - if First read by slot index rather than
	// connection order, it would wrongly return this handler's value (4)
	// instead of the value from the handler that is now first by
	// connection order (2)
	ev.connectLambda( []( int ) { return 4; } );

	EXPECT_EQ( ev.emit( 0 ), 2 );
}

TEST( Combiners, LastSurvivesSlotReuse )
{
	stellyra::CombiningEvent< stellyra::Combiners::Last< int >, int, int > ev{ nullptr };

	auto c1 = ev.connectLambda( []( int ) { return 1; } );  // slot 0
	ev.connectLambda( []( int ) { return 2; } );            // slot 1
	auto c3 = ev.connectLambda( []( int ) { return 3; } );  // slot 2

	c1.disconnect();  // frees slot 0
	c3.disconnect();  // frees slot 2

	// connected chronologically last, but reuses slot 0 (the lowest freed
	// slot) rather than landing at the physical end of storage - Last must
	// still identify this as the most recently connected handler
	ev.connectLambda( []( int ) { return 4; } );

	EXPECT_EQ( ev.emit( 0 ), 4 );
}

TEST( Combiners, SumIteratesInConnectionOrderNotSlotOrder )
{
	// Sum's own result is order-independent, but this confirms every
	// combiner - not just First/Last - reads the connection-order list
	// (CombiningEventImpl::order) rather than raw slot order for which
	// handlers are included, by checking which VALUES appear in the sum
	// after a slot-reuse scenario, not just their total
	stellyra::CombiningEvent< stellyra::Combiners::Sum< int >, int, int > ev{ nullptr };

	auto c1 = ev.connectLambda( []( int x ) { return x; } );  // slot 0, value x
	ev.connectLambda( []( int x ) { return x * 10; } );       // slot 1, value 10x

	c1.disconnect();  // frees slot 0

	ev.connectLambda( []( int x ) { return x * 100; } );  // reuses slot 0

	// both surviving handlers (10x and 100x) fire; the disconnected one (x)
	// does not - this is really testing disconnectSlotLocked() correctly
	// removes the freed index from the order list, not just marks it
	// inactive in handlers, since a stale leftover entry in order would
	// double-count or crash on a disconnected slot's freed Callable
	EXPECT_EQ( ev.emit( 1 ), 110 );  // 10 + 100, not 1 + 10 + 100
}

TEST( Combiners, DisconnectAllClearsConnectionOrder )
{
	// disconnectAll() must clear CombiningEventImpl::order along with
	// marking every slot inactive - otherwise a subsequent connect() would
	// append into a stale order list still referencing disconnected slots
	stellyra::CombiningEvent< stellyra::Combiners::First< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 1; } );
	ev.connectLambda( []( int ) { return 2; } );
	ev.disconnectAll();

	ev.connectLambda( []( int ) { return 99; } );

	EXPECT_EQ( ev.emit( 0 ), 99 );
}

TEST( Combiners, SingleShotRemovedFromConnectionOrderAfterFiring )
{
	// a single-shot connection is removed from CombiningEventImpl::order
	// (not just marked inactive in handlers) once it fires, so a later
	// connection reusing its freed slot is correctly placed at the end of
	// connection order rather than appearing to occupy the fired
	// single-shot's old position
	stellyra::CombiningEvent< stellyra::Combiners::Last< int >, int, int > ev{ nullptr };

	ev.connectLambda( []( int ) { return 1; } );      // slot 0, permanent
	ev.connectOnceLambda( []( int ) { return 2; } );  // slot 1, fires once

	EXPECT_EQ( ev.emit( 0 ), 2 );  // slot 1 (once-handler) is last in connection order

	// once-handler already fired and auto-disconnected; slot 1 is free
	ev.connectLambda( []( int ) { return 3; } );  // reuses slot 1, but is chronologically last now

	EXPECT_EQ( ev.emit( 0 ), 3 );
}
