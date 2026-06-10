#pragma once

// Prevent Windows macro pollution
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef Args
#undef Args
#endif

#include <gtest/gtest.h>

#ifdef emit
#undef emit
#endif

#ifdef Args
#undef Args
#endif

#include <kmac/pulsar_extras.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pulsar = kmac::pulsar;

// ---------------------------------------------------------------------------
// Global static functions for free-function connection tests
// ---------------------------------------------------------------------------

inline int _staticCallCount = 0;

inline void staticClickHandler( int, int )
{
	_staticCallCount++;
}

inline void anotherStaticHandler( int, int )
{
	// different handler - used to verify selective disconnection
}

// ---------------------------------------------------------------------------
// Shared test fixture classes
// ---------------------------------------------------------------------------

class TestButton : public pulsar::Object
{
public:
	pulsar::Event< int, int > clicked{ this };

	void click( int x, int y )
	{
		clicked( x, y );
	}
};

class TestHandler : public pulsar::Object
{
public:
	int callCount = 0;
	int lastX = 0;
	int lastY = 0;

	void onClicked( int x, int y )
	{
		callCount++;
		lastX = x;
		lastY = y;
	}

	void reset()
	{
		callCount = 0;
		lastX = 0;
		lastY = 0;
	}
};

class TestValidator : public pulsar::Object
{
public:
	bool checkValue( int value )
	{
		return value > 0;
	}
};

class DataSender : public pulsar::Object
{
public:
	pulsar::Event< int > dataReady{ this };

	void sendData( int value )
	{
		dataReady( value );
	}
};

class DataReceiver : public pulsar::Object
{
public:
	int callCount = 0;
	int lastValue = 0;
	std::vector< int > receivedValues;

	void processData( int value )
	{
		callCount++;
		lastValue = value;
		receivedValues.push_back( value );
	}

	void reset()
	{
		callCount = 0;
		lastValue = 0;
		receivedValues.clear();
	}
};

class RelayNode : public pulsar::Object
{
public:
	pulsar::Event< int > dataIn{ this };
	pulsar::Event< int > dataOut{ this };

	void setupRelay()
	{
		dataIn.connect( shared_from_this(), [ this ]( int value ) { dataOut( value ); } );
	}
};

class Checker : public pulsar::Object
{
public:
	bool expectedResult = true;
	int callCount = 0;

	bool check( const std::string& )
	{
		callCount++;
		return expectedResult;
	}
};

// ---------------------------------------------------------------------------
// Convenience: sleep in milliseconds (replaces QTest::qWait)
// ---------------------------------------------------------------------------

inline void msleep( int ms )
{
	std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
}
