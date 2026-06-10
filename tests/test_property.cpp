#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Property tests
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

class Config : public pulsar::Object
{
public:
	pulsar::Property< int > retries { this, 3 };
	pulsar::Property< std::string > label { this, std::string( "default" ) };
};

class Motor : public pulsar::Object
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

class SpecialSensor : public pulsar::Object
{
public:
	pulsar::Property< NonComparable > raw { this };
};

class Rectangle : public pulsar::Object
{
public:
	pulsar::Property< int > width { this, 0 };
	pulsar::Property< int > height { this, 0 };

	// callable struct used as the ComputeFn - avoids naming a lambda type,
	// which requires C++20 (in C++20 codebases a capturing lambda works directly)
	struct AreaCompute
	{
		Rectangle* self;
		int operator()() const { return self->width.get() * self->height.get(); }
	};

	pulsar::ComputedProperty< Rectangle, AreaCompute > area { this, AreaCompute{ this } };

	// expose invalidation for testing
	void recomputeArea()
	{
		area.invalidate();
	}

	// shared_from_this() is invalid during construction; wire deps after make_shared
	static std::shared_ptr< Rectangle > create()
	{
		auto r = std::make_shared< Rectangle >();
		r->width.changed.connect(
			r->shared_from_this(),
			[ raw = r.get() ]( const int& ) { raw->area.invalidate(); } );
		r->height.changed.connect(
			r->shared_from_this(),
			[ raw = r.get() ]( const int& ) { raw->area.invalidate(); } );
		return r;
	}
};

class Badge : public pulsar::Object
{
public:
	pulsar::ConstProperty< std::string > typeName { this, std::string( "Badge" ) };
};

class IntObserver : public pulsar::Object
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
	auto cfg = std::make_shared< Config >();
	EXPECT_EQ( cfg->retries.get(), 3 );
	EXPECT_EQ( cfg->label.get(), "default" );
}

TEST( Property, SetFiresChanged )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries.set( 10 );

	EXPECT_EQ( obs->callCount, 1 );
	EXPECT_EQ( obs->lastValue, 10 );
	EXPECT_EQ( cfg->retries.get(), 10 );
}

TEST( Property, AssignmentOperatorFiresChanged )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries = 7;

	EXPECT_EQ( obs->callCount, 1 );
	EXPECT_EQ( obs->lastValue, 7 );
}

TEST( Property, SameValueSkipped )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries.set( 5 );
	cfg->retries.set( 5 );  // no-op

	EXPECT_EQ( obs->callCount, 1 );
}

TEST( Property, SetForceFiredEvenIfSame )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries.set( 5 );
	cfg->retries.setForce( 5 );  // unconditional

	EXPECT_EQ( obs->callCount, 2 );
}

TEST( Property, SetQuietNoEmit )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries.setQuiet( 99 );

	EXPECT_EQ( obs->callCount, 0 );
	EXPECT_EQ( cfg->retries.get(), 99 );
}

TEST( Property, ImplicitConversionToRef )
{
	auto cfg = std::make_shared< Config >();
	cfg->retries.setQuiet( 42 );

	const int& v = cfg->retries;
	EXPECT_EQ( v, 42 );
}

TEST( Property, GetReturnsCurrentValue )
{
	auto cfg = std::make_shared< Config >();
	cfg->label.setQuiet( std::string( "hello" ) );

	EXPECT_EQ( cfg->label.get(), "hello" );
	EXPECT_EQ( cfg->label.get().size(), 5u );
}

TEST( Property, MultipleObservers )
{
	auto cfg = std::make_shared< Config >();
	auto obs1 = std::make_shared< IntObserver >();
	auto obs2 = std::make_shared< IntObserver >();

	cfg->retries.changed.connect( obs1, &IntObserver::on );
	cfg->retries.changed.connect( obs2, &IntObserver::on );
	cfg->retries = 8;

	EXPECT_EQ( obs1->callCount, 1 );
	EXPECT_EQ( obs2->callCount, 1 );
}

TEST( Property, DisconnectStopsNotifications )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< IntObserver >();

	auto conn = cfg->retries.changed.connect( obs, &IntObserver::on );
	cfg->retries = 1;
	EXPECT_EQ( obs->callCount, 1 );

	conn.disconnect();
	cfg->retries = 2;
	EXPECT_EQ( obs->callCount, 1 );
}

TEST( Property, ObserverDestroyedAutoDisconnects )
{
	auto cfg = std::make_shared< Config >();
	{
		auto obs = std::make_shared< IntObserver >();
		cfg->retries.changed.connect( obs, &IntObserver::on );
		cfg->retries = 1;
	}
	// should not crash
	cfg->retries = 2;
}

TEST( Property, NonComparableTypeAlwaysFires )
{
	auto sensor = std::make_shared< SpecialSensor >();
	auto obs = std::make_shared< pulsar::Object >();

	int callCount = 0;
	sensor->raw.changed.connect( obs, [ &callCount ]( const NonComparable& ) {
		callCount++;
	} );

	// no operator==, so every set() fires
	sensor->raw.set( NonComparable{ 1 } );
	sensor->raw.set( NonComparable{ 1 } );

	EXPECT_EQ( callCount, 2 );
}

TEST( Property, StringProperty )
{
	auto cfg = std::make_shared< Config >();
	auto obs = std::make_shared< pulsar::Object >();

	std::string received;
	cfg->label.changed.connect( obs, [ &received ]( const std::string& v ) {
		received = v;
	} );

	cfg->label = std::string( "hello" );
	EXPECT_EQ( received, "hello" );

	cfg->label = std::string( "hello" );  // same - no fire
	EXPECT_EQ( received, "hello" );

	cfg->label = std::string( "world" );
	EXPECT_EQ( received, "world" );
}

// ---------------------------------------------------------------------------
// ReadOnlyProperty<Owner, T>
// ---------------------------------------------------------------------------

TEST( ReadOnlyProperty, SetViaOwnerFiresChanged )
{
	auto motor = std::make_shared< Motor >();
	auto obs = std::make_shared< IntObserver >();

	motor->rpm.changed.connect( obs, &IntObserver::on );
	motor->setRpm( 3000 );

	EXPECT_EQ( obs->callCount, 1 );
	EXPECT_EQ( obs->lastValue, 3000 );
	EXPECT_EQ( motor->rpm.get(), 3000 );
}

TEST( ReadOnlyProperty, ReadAccessIsPublic )
{
	auto motor = std::make_shared< Motor >();
	motor->setRpmQuiet( 1500 );

	EXPECT_EQ( motor->rpm.get(), 1500 );

	const int& v = motor->rpm;
	EXPECT_EQ( v, 1500 );
}

TEST( ReadOnlyProperty, SetQuietNoEmit )
{
	auto motor = std::make_shared< Motor >();
	auto obs = std::make_shared< IntObserver >();

	motor->rpm.changed.connect( obs, &IntObserver::on );
	motor->setRpmQuiet( 500 );

	EXPECT_EQ( obs->callCount, 0 );
	EXPECT_EQ( motor->rpm.get(), 500 );
}

TEST( ReadOnlyProperty, SetForceUnconditional )
{
	auto motor = std::make_shared< Motor >();
	auto obs = std::make_shared< IntObserver >();

	motor->rpm.changed.connect( obs, &IntObserver::on );
	motor->setRpm( 1000 );
	motor->setRpmForce( 1000 );  // same value but force

	EXPECT_EQ( obs->callCount, 2 );
}

TEST( ReadOnlyProperty, SameValueSkipped )
{
	auto motor = std::make_shared< Motor >();
	auto obs = std::make_shared< IntObserver >();

	motor->rpm.changed.connect( obs, &IntObserver::on );
	motor->setRpm( 2000 );
	motor->setRpm( 2000 );

	EXPECT_EQ( obs->callCount, 1 );
}

TEST( ReadOnlyProperty, ConnectionsArePublic )
{
	auto motor = std::make_shared< Motor >();
	auto obs = std::make_shared< IntObserver >();

	auto conn = motor->rpm.changed.connect( obs, &IntObserver::on );
	EXPECT_TRUE( conn.isConnected() );

	conn.disconnect();
	EXPECT_FALSE( conn.isConnected() );
}

// ---------------------------------------------------------------------------
// ComputedProperty<Owner, Fn, T>
// ---------------------------------------------------------------------------

TEST( ComputedProperty, InitialValueFromCompute )
{
	// simple case: no deps, just verifies construction evaluates fn
	auto obj = std::make_shared< pulsar::Object >();

	struct DoubleCompute
	{
		int operator()() const { return 21 * 2; }
	};

	pulsar::ComputedProperty< pulsar::Object, DoubleCompute > prop( obj.get(), DoubleCompute{} );
	EXPECT_EQ( prop.get(), 42 );
}

TEST( ComputedProperty, InvalidateFiresChangedWithNewValue )
{
	auto rect = Rectangle::create();
	auto obs = std::make_shared< IntObserver >();

	rect->area.changed.connect( obs, &IntObserver::on );

	rect->width = 4;   // triggers area.invalidate() via width.changed connection

	EXPECT_EQ( obs->callCount, 1 );
	EXPECT_EQ( obs->lastValue, 0 );   // 4 * 0
	EXPECT_EQ( rect->area.get(), 0 );

	rect->height = 5;  // 4 * 5

	EXPECT_EQ( obs->callCount, 2 );
	EXPECT_EQ( obs->lastValue, 20 );
	EXPECT_EQ( rect->area.get(), 20 );
}

TEST( ComputedProperty, CacheReturnedBetweenInvalidations )
{
	auto rect = Rectangle::create();

	rect->width  = 3;
	rect->height = 4;
	// area cached as 12

	int area1 = rect->area.get();
	int area2 = rect->area.get();
	EXPECT_EQ( area1, 12 );
	EXPECT_EQ( area2, 12 );
}

TEST( ComputedProperty, ImplicitConversion )
{
	auto rect = Rectangle::create();
	rect->width  = 6;
	rect->height = 7;

	const int& v = rect->area;
	EXPECT_EQ( v, 42 );
}

TEST( ComputedProperty, ManualInvalidate )
{
	auto rect = Rectangle::create();
	auto obs = std::make_shared< IntObserver >();

	rect->area.changed.connect( obs, &IntObserver::on );

	// manually poke without changing a dep
	rect->recomputeArea();

	EXPECT_EQ( obs->callCount, 1 );
	EXPECT_EQ( obs->lastValue, 0 );
}

// ---------------------------------------------------------------------------
// ConstProperty<T>
// ---------------------------------------------------------------------------

TEST( ConstProperty, ReturnsConstructedValue )
{
	auto badge = std::make_shared< Badge >();
	EXPECT_EQ( badge->typeName.get(), "Badge" );
}

TEST( ConstProperty, ImplicitConversion )
{
	auto badge = std::make_shared< Badge >();
	const std::string& name = badge->typeName;
	EXPECT_EQ( name, "Badge" );
}

TEST( ConstProperty, ChangedNeverFires )
{
	auto badge = std::make_shared< Badge >();
	auto obs = std::make_shared< pulsar::Object >();

	int callCount = 0;
	badge->typeName.changed.connect( obs, [ &callCount ]( const std::string& ) {
		callCount++;
	} );

	// nothing can fire it - just verify connect/observe compiles and nothing crashes
	EXPECT_EQ( callCount, 0 );
}

TEST( ConstProperty, GetReturnsValue )
{
	auto badge = std::make_shared< Badge >();
	EXPECT_EQ( badge->typeName.get().size(), 5u );  // "Badge"
}

// ---------------------------------------------------------------------------
// ROProperty alias
// ---------------------------------------------------------------------------

TEST( ROPropertyAlias, IsReadOnlyProperty )
{
	// verify the alias compiles and behaves identically
	class Foo : public pulsar::Object
	{
	public:
		pulsar::ROProperty< Foo, int > val { this, 7 };
		void set( int v ) { val = v; }
	};

	auto foo = std::make_shared< Foo >();
	EXPECT_EQ( foo->val.get(), 7 );

	foo->set( 99 );
	EXPECT_EQ( foo->val.get(), 99 );
}
