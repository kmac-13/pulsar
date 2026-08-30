#pragma once

// prevent Windows macro pollution
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

class TestButton : public pulsar::Trackable
{
public:
	pulsar::Event< int, int > clicked { this };

	void click( int x, int y )
	{
		clicked( x, y );
	}
};

class TestHandler : public pulsar::Trackable
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

// ---------------------------------------------------------------------------
// MutexType-templated variants of the fixtures above, for tests that are
// parametrized across Event/SharedEvent/SingleThreadedEvent (i.e.
// BasicEvent<RecursiveMutex,...>/BasicEvent<SharedMutex,...>/
// BasicEvent<NullMutex,...>).  Kept separate from the untemplated
// TestButton/DataSender/RelayNode above (rather than templating those in
// place) so files not yet converted keep compiling unchanged.
// ---------------------------------------------------------------------------

template< typename MutexType >
class TestButtonT : public pulsar::Trackable
{
public:
	pulsar::BasicEvent< MutexType, int, int > clicked { this };

	void click( int x, int y )
	{
		clicked( x, y );
	}
};

template< typename MutexType >
class DataSenderT : public pulsar::Trackable
{
public:
	pulsar::BasicEvent< MutexType, int > dataReady { this };

	void sendData( int value )
	{
		dataReady( value );
	}
};

template< typename MutexType >
class RelayNodeT : public pulsar::Trackable
{
public:
	pulsar::BasicEvent< MutexType, int > dataIn { this };
	pulsar::BasicEvent< MutexType, int > dataOut { this };

	void setupRelay()
	{
		dataIn.connectLambda( *this, [ this ]( int value ) { dataOut( value ); } );
	}
};

class TestValidator : public pulsar::Trackable
{
public:
	bool checkValue( int value )
	{
		return value > 0;
	}
};

class DataSender : public pulsar::Trackable
{
public:
	pulsar::Event< int > dataReady { this };

	void sendData( int value )
	{
		dataReady( value );
	}
};

class DataReceiver : public pulsar::Trackable
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

class RelayNode : public pulsar::Trackable
{
public:
	pulsar::Event< int > dataIn { this };
	pulsar::Event< int > dataOut { this };

	void setupRelay()
	{
		dataIn.connectLambda( *this, [ this ]( int value ) { dataOut( value ); } );
	}
};

class Checker : public pulsar::Trackable
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
