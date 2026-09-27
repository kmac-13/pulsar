#include "test_helpers.hpp"

// ===========================================================================
// Runtime (non-NTTP) pointer-to-member disconnect-by-target, and the
// single-match behaviour shared by every by-target disconnect form.
//
// - connect(receiver, method) is disconnectable via
//   disconnect(receiver, method) without holding the Connection, matching by
//   receiver pointer and native pointer-to-member equality
// - every by-target disconnect (NTTP method, runtime method, NTTP free,
//   runtime free) removes one matching connection, so a duplicate connect is
//   neither masked nor fully removed by a single disconnect; the bulk forms
//   (tracker, disconnectAll) remove all matches
//
// No test here does reentrant connect/disconnect from within its own
// dispatch, and no test spawns real threads, so all three MutexType
// variants (Event, SharedEvent, SingleThreadedEvent) are safe.
// ===========================================================================

namespace
{
	// two same-signature methods on one object: disconnect(receiver, method)
	// must select by the method pointer, not the receiver alone
	class TwoMethods : public pulsar::Trackable
	{
	public:
		int a = 0;
		int b = 0;

		void onA( int, int ) { a++; }
		void onB( int, int ) { b++; }
	};

	// a partial-arity receiver: onOne takes fewer arguments than the event
	class PartialReceiver : public pulsar::Trackable
	{
	public:
		int calls = 0;

		void onOne( int ) { calls++; }
	};

	// a receiver that does not inherit from Trackable, for the Tracked<T>
	// pair form of connect / disconnect
	class PlainReceiver
	{
	public:
		int calls = 0;

		void onClick( int, int ) { calls++; }
	};

	int _freeCallsA = 0;
	int _freeCallsB = 0;

	void freeA( int, int ) { _freeCallsA++; }
	void freeB( int, int ) { _freeCallsB++; }
}


template< typename MutexType >
class DisconnectByTarget : public ::testing::Test {};

using MutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex,
	pulsar::platform::NullMutex >;
TYPED_TEST_SUITE( DisconnectByTarget, MutexTypes );

// ---------------------------------------------------------------------------
// Runtime PMF disconnect-by-target
// ---------------------------------------------------------------------------

TYPED_TEST( DisconnectByTarget, RuntimePmfRemovesConnection )
{
	auto button = std::make_unique< TestButtonT< TypeParam > >();
	auto handler = std::make_unique< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );
	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 1 );

	button->clicked.disconnect( *handler, &TestHandler::onClicked );

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 0 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 0u );
}

TYPED_TEST( DisconnectByTarget, RuntimePmfDiscriminatesByMethod )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto obj = std::make_shared< TwoMethods >();

	button->clicked.connect( *obj, &TwoMethods::onA );
	button->clicked.connect( *obj, &TwoMethods::onB );

	// same receiver, different method: only onA is removed
	button->clicked.disconnect( *obj, &TwoMethods::onA );

	button->click( 1, 1 );
	EXPECT_EQ( obj->a, 0 );
	EXPECT_EQ( obj->b, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( DisconnectByTarget, RuntimePmfDiscriminatesByReceiver )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto h1 = std::make_shared< TestHandler >();
	auto h2 = std::make_unique< TestHandler >();

	button->clicked.connect( *h1, &TestHandler::onClicked );
	button->clicked.connect( *h2, &TestHandler::onClicked );

	// same method, different receiver: only h1 is removed
	button->clicked.disconnect( *h1, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( h1->callCount, 0 );
	EXPECT_EQ( h2->callCount, 1 );
}

TYPED_TEST( DisconnectByTarget, RuntimePmfPartialArity )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto recv = std::make_shared< PartialReceiver >();

	button->clicked.connect( *recv, &PartialReceiver::onOne );  // drops 2nd int
	button->click( 5, 6 );
	EXPECT_EQ( recv->calls, 1 );

	button->clicked.disconnect( *recv, &PartialReceiver::onOne );

	recv->calls = 0;
	button->click( 7, 8 );
	EXPECT_EQ( recv->calls, 0 );
}

TYPED_TEST( DisconnectByTarget, RuntimePmfTrackedPairForm )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	PlainReceiver recv;         // not a Trackable
	pulsar::Trackable anchor;   // separate lifetime anchor

	button->clicked.connect( pulsar::Tracked{ recv, anchor }, &PlainReceiver::onClick );
	button->click( 1, 1 );
	EXPECT_EQ( recv.calls, 1 );

	// identity is receiver + method; the anchor is not part of it
	button->clicked.disconnect( recv, &PlainReceiver::onClick );

	recv.calls = 0;
	button->click( 2, 2 );
	EXPECT_EQ( recv.calls, 0 );
}

TYPED_TEST( DisconnectByTarget, RuntimePmfNoMatchIsNoOp )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto obj = std::make_shared< TwoMethods >();

	button->clicked.connect( *obj, &TwoMethods::onA );

	// a method that was never connected: disconnect is a no-op
	button->clicked.disconnect( *obj, &TwoMethods::onB );

	button->click( 1, 1 );
	EXPECT_EQ( obj->a, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

// ---------------------------------------------------------------------------
// Single-match across all four by-target disconnect forms
// ---------------------------------------------------------------------------

TYPED_TEST( DisconnectByTarget, SingleMatchRuntimePmf )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.connect( *handler, &TestHandler::onClicked );
	button->clicked.connect( *handler, &TestHandler::onClicked );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );

	button->clicked.disconnect( *handler, &TestHandler::onClicked );

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );  // one connection survives

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( DisconnectByTarget, SingleMatchNttpMethod )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	auto handler = std::make_shared< TestHandler >();

	button->clicked.template connect< &TestHandler::onClicked >( *handler );
	button->clicked.template connect< &TestHandler::onClicked >( *handler );

	button->click( 1, 1 );
	EXPECT_EQ( handler->callCount, 2 );

	button->clicked.template disconnect< &TestHandler::onClicked >( *handler );

	handler->reset();
	button->click( 2, 2 );
	EXPECT_EQ( handler->callCount, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( DisconnectByTarget, SingleMatchRuntimeFree )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_freeCallsA = 0;

	button->clicked.connectFree( freeA );
	button->clicked.connectFree( freeA );

	button->click( 1, 1 );
	EXPECT_EQ( _freeCallsA, 2 );

	button->clicked.disconnectFree( freeA );

	_freeCallsA = 0;
	button->click( 2, 2 );
	EXPECT_EQ( _freeCallsA, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}

TYPED_TEST( DisconnectByTarget, SingleMatchNttpFree )
{
	auto button = std::make_shared< TestButtonT< TypeParam > >();
	_freeCallsB = 0;

	button->clicked.template connectFree< &freeB >();
	button->clicked.template connectFree< &freeB >();

	button->click( 1, 1 );
	EXPECT_EQ( _freeCallsB, 2 );

	button->clicked.template disconnectFree< &freeB >();

	_freeCallsB = 0;
	button->click( 2, 2 );
	EXPECT_EQ( _freeCallsB, 1 );

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	EXPECT_EQ( inspector.getEventInfo().activeConnectionCount, 1u );
}
