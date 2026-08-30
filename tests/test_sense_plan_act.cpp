#include "test_helpers.hpp"

#include <kmac/pulsar/event_loop.h>

// Validates the "Differential Machine Control via Deferred Event Loops"
// pattern (see USE_CASE_DIFFERENTIAL_MACHINE_CONTROL.md): Sense reads
// hardware inputs unconditionally every tick and fires a deferred event only
// on an actual delta; Plan and Act then drain whatever that produced, which
// is nothing when the sensed value didn't change.
//
// Sense is intentionally not backed by its own EventLoop: hardware inputs
// are read unconditionally every tick - there's nothing to differentially
// skip about the read itself, since you always have to look before you know
// whether anything changed.  The efficiency gain this pattern is about comes
// entirely from Plan loop being skipped when Sense finds nothing changed and/or
// the Act loop being skipped if no changes have been generated from Planning,
// not from Sense itself being deferred.  So only Plan and Act are EventLoops
// here; Sense is a plain synchronous method.
//
// All draining here happens sequentially on one thread (no std::thread, no
// AutoDrainThread), and no handler reentrantly connects/disconnects during
// its own event's dispatch, so all three MutexType variants (Event,
// SharedEvent, SingleThreadedEvent) are safe.  Sensor/Planner each hold a
// single Args signature, so parametrizing on a fixed EventT alias (rather
// than MutexType + BasicEvent<...>) is enough here.

namespace {

template< typename EventT >
class Sensor : public pulsar::Trackable
{
private:
	int _last = -1;

public:
	EventT stateChanged{ this };

	void scan( int currentValue )
	{
		if ( currentValue != _last )
		{
			_last = currentValue;
			stateChanged( currentValue );
		}
	}
};

template< typename EventT >
class Planner : public pulsar::Trackable
{
public:
	EventT actuatorCommand{ this };
	int planCallCount = 0;

	void onStateChanged( int sensedValue )
	{
		++planCallCount;
		actuatorCommand( sensedValue * 2 );  // trivial control law
	}
};

class Actuator : public pulsar::Trackable
{
public:
	int lastCommand = -1;
	int actCallCount = 0;

	void onCommand( int cmd )
	{
		++actCallCount;
		lastCommand = cmd;
	}
};

template< typename EventT >
struct SensePlanActFixture
{
	pulsar::EventLoop planLoop;
	pulsar::EventLoop actLoop;

	Sensor< EventT > sensor;
	Planner< EventT > planner;
	Actuator actuator;

	// Test-only input: stands in for whatever register/memory-mapped I/O a
	// real sense() would read directly.  Not part of the pattern itself -
	// production sense() takes no arguments, since hardware doesn't need to
	// be told its own state.
	int nextSensorValue = 0;

	// Initialize the connections and associated EventLoops.
	SensePlanActFixture()
	{
		planner.setEventLoop( &planLoop );
		actuator.setEventLoop( &actLoop );

		sensor.stateChanged.connect( planner, &Planner< EventT >::onStateChanged, pulsar::ConnectionType::Deferred );
		planner.actuatorCommand.connect( actuator, &Actuator::onCommand, pulsar::ConnectionType::Deferred );
	}

	// SENSE phase.  Takes no arguments in production - reads hardware
	// registers/memory-mapped I/O directly.  Here it reads nextSensorValue
	// instead, purely so the test can control what a given tick "senses".
	// Fires stateChanged only on an actual delta from the last reading.
	void sense()
	{
		sensor.scan( nextSensorValue );
	}

	// One hardware tick - always takes no arguments, in tests or in
	// production.  SENSE always runs; PLAN and ACT then drain whatever it
	// produced, which is nothing when the sensed value didn't change.
	void tick()
	{
		sense();           // SENSE
		planLoop.drain();  // PLAN
		actLoop.drain();   // ACT
	}
};

} // namespace

template< typename EventT >
class SensePlanAct : public ::testing::Test {};

using EventTypes = ::testing::Types<
	pulsar::Event< int >,
	pulsar::SharedEvent< int >,
	pulsar::SingleThreadedEvent< int > >;
TYPED_TEST_SUITE( SensePlanAct, EventTypes );

TYPED_TEST( SensePlanAct, FullCascadeCompletesInOneTick )
{
	SensePlanActFixture< TypeParam > f;

	f.nextSensorValue = 100;
	f.tick();

	EXPECT_EQ( f.planner.planCallCount, 1 );
	EXPECT_EQ( f.actuator.actCallCount, 1 );
	EXPECT_EQ( f.actuator.lastCommand, 200 );  // 100 * 2
}

TYPED_TEST( SensePlanAct, ZeroWorkTickWhenValueUnchanged )
{
	SensePlanActFixture< TypeParam > f;

	f.nextSensorValue = 100;
	f.tick();
	EXPECT_EQ( f.planner.planCallCount, 1 );  // initial work performed
	EXPECT_EQ( f.actuator.actCallCount, 1 );  // initial work performed

	// same value - stateChanged does not fire
	f.tick();

	EXPECT_EQ( f.planner.planCallCount, 1 );  // unchanged
	EXPECT_EQ( f.actuator.actCallCount, 1 );  // unchanged
}

TYPED_TEST( SensePlanAct, SecondChangeCascadesCorrectly )
{
	SensePlanActFixture< TypeParam > f;

	f.nextSensorValue = 100;
	f.tick();

	f.nextSensorValue = 50;
	f.tick();

	EXPECT_EQ( f.planner.planCallCount, 2 );
	EXPECT_EQ( f.actuator.actCallCount, 2 );
	EXPECT_EQ( f.actuator.lastCommand, 100 );  // 50 * 2
}

TYPED_TEST( SensePlanAct, RepeatedZeroWorkTicksAreStable )
{
	SensePlanActFixture< TypeParam > f;

	f.nextSensorValue = 75;
	f.tick();

	for ( int i = 0; i < 5; ++i )
	{
		f.tick();  // nextSensorValue unchanged
	}

	EXPECT_EQ( f.planner.planCallCount, 1 );
	EXPECT_EQ( f.actuator.actCallCount, 1 );
}
