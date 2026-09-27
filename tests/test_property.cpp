#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Property tests
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

class Config : public pulsar::Trackable
{
public:
	pulsar::Property< int > retries { this, 3 };
	pulsar::Property< std::string > label { this, std::string( "default" ) };
};

class Motor : public pulsar::Trackable
{
public:
	pulsar::ReadOnlyProperty< Motor, int > rpm { this, 0 };

	void setRpm( int v ) { rpm = v; }
	void setRpmForce( int v ) { rpm.setForce( v ); }
	void setRpmQuiet( int v ) { rpm.setQuiet( v ); }
};

struct NonComparable
{
	int data = 0;
	// no operator==
};

class SpecialSensor : public pulsar::Trackable
{
public:
	pulsar::Property< NonComparable > raw { this };
};

class Rectangle : public pulsar::Trackable
{
public:
	pulsar::Property< int > width { this, 0 };
	pulsar::Property< int > height { this, 0 };

	// ComputedPropertyFn erases the compute callable's type into
	// Callable<T()>, so any lambda works directly - no need to name a
	// functor type for the member declaration
	pulsar::ComputedPropertyFn< Rectangle, int > area {
		this, [ this ]() { return width.get() * height.get(); } };

	Rectangle()
	{
		width.changed.connectLambda( *this, [ this ]( const int& ) { area.invalidate(); } );
		height.changed.connectLambda( *this, [ this ]( const int& ) { area.invalidate(); } );
	}

	// expose invalidation for testing
	void recomputeArea()
	{
		area.invalidate();
	}
};

class Badge : public pulsar::Trackable
{
public:
	pulsar::ConstProperty< std::string > typeName { this, std::string( "Badge" ) };
};

class IntObserver : public pulsar::Trackable
{
public:
	int callCount = 0;
	int lastValue = -1;

	void on( const int& v )
	{
		callCount++;
		lastValue = v;
	}

	void reset()
	{
		callCount = 0;
		lastValue = -1;
	}
};

// ---------------------------------------------------------------------------
// Property<T>
// ---------------------------------------------------------------------------

TEST( Property, InitialValue )
{
	Config cfg;
	EXPECT_EQ( cfg.retries.get(), 3 );
	EXPECT_EQ( cfg.label.get(), "default" );
}

// Tests get()/set() explicitly.
TEST( Property, SetFiresChanged )
{
	Config cfg;
	IntObserver obs;

	cfg.retries.changed.connect( obs, &IntObserver::on );

	cfg.retries.set( 10 );
	EXPECT_EQ( obs.callCount, 1 );
	EXPECT_EQ( obs.lastValue, 10 );
	EXPECT_EQ( cfg.retries.get(), 10 );
}

// Tests assignment/implicit cast.
TEST( Property, AssignmentOperatorFiresChanged )
{
	Config cfg;
	IntObserver obs;

	cfg.retries.changed.connect( obs, &IntObserver::on );
	cfg.retries = 7;

	EXPECT_EQ( obs.callCount, 1 );
	EXPECT_EQ( obs.lastValue, 7 );
}

TEST( Property, SameValueSkipped )
{
	Config cfg;
	IntObserver obs;

	cfg.retries.changed.connect( obs, &IntObserver::on );
	cfg.retries.set( 5 );
	cfg.retries.set( 5 );  // no-op

	EXPECT_EQ( obs.callCount, 1 );
}

TEST( Property, SetForceFiredEvenIfSame )
{
	Config cfg;
	IntObserver obs;

	cfg.retries.changed.connect( obs, &IntObserver::on );
	cfg.retries.set( 5 );
	cfg.retries.setForce( 5 );  // unconditional

	EXPECT_EQ( obs.callCount, 2 );
}

TEST( Property, SetQuietNoEmit )
{
	Config cfg;
	IntObserver obs;

	cfg.retries.changed.connect( obs, &IntObserver::on );
	cfg.retries.setQuiet( 99 );  // does not trigger

	EXPECT_EQ( obs.callCount, 0 );
	EXPECT_EQ( cfg.retries.get(), 99 );
}

TEST( Property, ImplicitConversionToRef )
{
	Config cfg;
	cfg.retries.setQuiet( 42 );

	const int& v = cfg.retries;
	EXPECT_EQ( v, 42 );
}

TEST( Property, GetReturnsCurrentValue )
{
	Config cfg;
	cfg.label.setQuiet( std::string( "hello" ) );

	EXPECT_EQ( cfg.label.get(), "hello" );
	EXPECT_EQ( cfg.label.get().size(), 5u );
}

TEST( Property, MultipleObservers )
{
	Config cfg;
	IntObserver obs1;
	IntObserver obs2;

	cfg.retries.changed.connect( obs1, &IntObserver::on );
	cfg.retries.changed.connect( obs2, &IntObserver::on );
	cfg.retries = 8;

	EXPECT_EQ( obs1.callCount, 1 );
	EXPECT_EQ( obs2.callCount, 1 );
}

TEST( Property, DisconnectStopsNotifications )
{
	Config cfg;
	IntObserver obs;

	auto conn = cfg.retries.changed.connect( obs, &IntObserver::on );
	cfg.retries = 1;
	EXPECT_EQ( obs.callCount, 1 );

	conn.disconnect();
	cfg.retries = 2;
	EXPECT_EQ( obs.callCount, 1 );
}

TEST( Property, ObserverDestroyedAutoDisconnects )
{
	Config cfg;
	{
		IntObserver obs;
		cfg.retries.changed.connect( obs, &IntObserver::on );
		cfg.retries = 1;
	}

	// should not crash
	cfg.retries = 2;
}

TEST( Property, NonComparableTypeAlwaysFires )
{
	SpecialSensor sensor;
	pulsar::Trackable obs;

	int callCount = 0;
	sensor.raw.changed.connectLambda( obs, [ &callCount ]( const NonComparable& ) {
		callCount++;
	} );

	// no operator==, so every set() fires
	sensor.raw.set( NonComparable{ 1 } );
	sensor.raw.set( NonComparable{ 1 } );

	EXPECT_EQ( callCount, 2 );
}

TEST( Property, StringProperty )
{
	Config cfg;
	pulsar::Trackable obs;

	std::string received;
	cfg.label.changed.connectLambda( obs, [ &received ]( const std::string& v ) {
		received = v;
	} );

	cfg.label = std::string( "hello" );
	EXPECT_EQ( received, "hello" );

	cfg.label = std::string( "hello" );  // same - no fire
	EXPECT_EQ( received, "hello" );

	cfg.label = std::string( "world" );
	EXPECT_EQ( received, "world" );
}

// ---------------------------------------------------------------------------
// ReadOnlyProperty<Owner, T>
// ---------------------------------------------------------------------------

TEST( ReadOnlyProperty, SetViaOwnerFiresChanged )
{
	Motor motor;
	IntObserver obs;

	motor.rpm.changed.connect( obs, &IntObserver::on );
	motor.setRpm( 3000 );

	EXPECT_EQ( obs.callCount, 1 );
	EXPECT_EQ( obs.lastValue, 3000 );
	EXPECT_EQ( motor.rpm.get(), 3000 );
}

TEST( ReadOnlyProperty, ReadAccessIsPublic )
{
	Motor motor;
	motor.setRpmQuiet( 1500 );

	EXPECT_EQ( motor.rpm.get(), 1500 );

	const int& v = motor.rpm;
	EXPECT_EQ( v, 1500 );
}

TEST( ReadOnlyProperty, SetQuietNoEmit )
{
	Motor motor;
	IntObserver obs;

	motor.rpm.changed.connect( obs, &IntObserver::on );
	motor.setRpmQuiet( 500 );

	EXPECT_EQ( obs.callCount, 0 );
	EXPECT_EQ( motor.rpm.get(), 500 );
}

TEST( ReadOnlyProperty, SetForceUnconditional )
{
	Motor motor;
	IntObserver obs;

	motor.rpm.changed.connect( obs, &IntObserver::on );
	motor.setRpm( 1000 );
	motor.setRpmForce( 1000 );  // same value but force

	EXPECT_EQ( obs.callCount, 2 );
}

TEST( ReadOnlyProperty, SameValueSkipped )
{
	Motor motor;
	IntObserver obs;

	motor.rpm.changed.connect( obs, &IntObserver::on );
	motor.setRpm( 2000 );
	motor.setRpm( 2000 );

	EXPECT_EQ( obs.callCount, 1 );
}

TEST( ReadOnlyProperty, ConnectionsArePublic )
{
	Motor motor;
	IntObserver obs;

	auto conn = motor.rpm.changed.connect( obs, &IntObserver::on );
	EXPECT_TRUE( conn.isConnected() );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );
}

// ---------------------------------------------------------------------------
// ComputedPropertyFn<Owner, T>
// ---------------------------------------------------------------------------

TEST( ComputedPropertyFn, InitialValueFromCompute )
{
	// simple case: no deps, just verifies construction evaluates fn
	pulsar::Trackable obj;

	pulsar::ComputedPropertyFn< pulsar::Trackable, int > prop( &obj, []() { return 21 * 2; } );
	EXPECT_EQ( prop.get(), 42 );
}

TEST( ComputedPropertyFn, InvalidateFiresChangedWithNewValue )
{
	Rectangle rect;
	IntObserver obs;

	rect.area.changed.connectLambda( obs, [ &obs ]( int v ) { obs.on( v ); } );

	rect.width = 4;   // triggers area.invalidate() via width.changed connection

	EXPECT_EQ( obs.callCount, 1 );
	EXPECT_EQ( obs.lastValue, 0 );   // 4 * 0
	EXPECT_EQ( rect.area.get(), 0 );

	rect.height = 5;  // 4 * 5

	EXPECT_EQ( obs.callCount, 2 );
	EXPECT_EQ( obs.lastValue, 20 );
	EXPECT_EQ( rect.area.get(), 20 );
}

TEST( ComputedPropertyFn, CacheReturnedBetweenInvalidations )
{
	Rectangle rect;

	rect.width = 3;
	rect.height = 4;
	// area cached as 12

	int area1 = rect.area.get();
	int area2 = rect.area.get();
	EXPECT_EQ( area1, 12 );
	EXPECT_EQ( area2, 12 );
}

TEST( ComputedPropertyFn, ImplicitConversion )
{
	Rectangle rect;
	rect.width = 6;
	rect.height = 7;

	int v = rect.area;
	EXPECT_EQ( v, 42 );
}

TEST( ComputedPropertyFn, ManualInvalidate )
{
	Rectangle rect;
	IntObserver obs;

	rect.area.changed.connectLambda( obs, [ &obs ]( int v ) { obs.on( v ); } );

	// manually poke without changing a dep
	rect.recomputeArea();

	EXPECT_EQ( obs.callCount, 1 );
	EXPECT_EQ( obs.lastValue, 0 );
}

// ---------------------------------------------------------------------------
// ConstProperty<T>
// ---------------------------------------------------------------------------

TEST( ConstProperty, ReturnsConstructedValue )
{
	Badge badge;
	EXPECT_EQ( badge.typeName.get(), "Badge" );
}

TEST( ConstProperty, ImplicitConversion )
{
	Badge badge;
	const std::string& name = badge.typeName;
	EXPECT_EQ( name, "Badge" );
}

TEST( ConstProperty, ChangedNeverFires )
{
	Badge badge;
	pulsar::Trackable obs;

	int callCount = 0;
	badge.typeName.changed.connectLambda( obs, [ &callCount ]( const std::string& ) {
		callCount++;
	} );

	// nothing can fire it - just verify connect/observe compiles and nothing crashes
	EXPECT_EQ( callCount, 0 );
}

TEST( ConstProperty, GetReturnsValue )
{
	Badge badge;
	EXPECT_EQ( badge.typeName.get().size(), 5u );  // "Badge"
}

// ---------------------------------------------------------------------------
// ROProperty alias
// ---------------------------------------------------------------------------

TEST( ROPropertyAlias, IsReadOnlyProperty )
{
	// verify the alias compiles and behaves identically
	class Foo : public pulsar::Trackable
	{
	public:
		pulsar::ROProperty< Foo, int > val { this, 7 };
		void set( int v ) { val = v; }
	};

	Foo foo;
	EXPECT_EQ( foo.val.get(), 7 );

	foo.set( 99 );
	EXPECT_EQ( foo.val.get(), 99 );
}
