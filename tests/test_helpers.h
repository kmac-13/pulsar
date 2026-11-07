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

#include <kmac/stellyra_extras.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace stellyra = kmac::stellyra;

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

class TestButton : public stellyra::Trackable
{
public:
	stellyra::Event< int, int > clicked { this };

	void click( int x, int y )
	{
		clicked( x, y );
	}
};

class TestHandler : public stellyra::Trackable
{
public:
	// SharedEvent dispatches concurrently across emitting threads by
	// design (see ThreadSafety's SharedMutex case), so these must survive
	// concurrent writes from onClicked() running on more than one thread
	// at once (a plain int here would be a data race)
	std::atomic< int > callCount{ 0 };
	std::atomic< int > lastX{ 0 };
	std::atomic< int > lastY{ 0 };

	void onClicked( int x, int y )
	{
		callCount.fetch_add( 1, std::memory_order_relaxed );
		lastX.store( x, std::memory_order_relaxed );
		lastY.store( y, std::memory_order_relaxed );
	}

	void reset()
	{
		callCount.store( 0, std::memory_order_relaxed );
		lastX.store( 0, std::memory_order_relaxed );
		lastY.store( 0, std::memory_order_relaxed );
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
class TestButtonT : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int, int > clicked { this };

	void click( int x, int y )
	{
		clicked( x, y );
	}
};

template< typename MutexType >
class DataSenderT : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int > dataReady { this };

	void sendData( int value )
	{
		dataReady( value );
	}
};

template< typename MutexType >
class RelayNodeT : public stellyra::Trackable
{
public:
	stellyra::BasicEvent< MutexType, int > dataIn { this };
	stellyra::BasicEvent< MutexType, int > dataOut { this };

	void setupRelay()
	{
		dataIn.connectLambda( *this, [ this ]( int value ) { dataOut( value ); } );
	}
};

class TestValidator : public stellyra::Trackable
{
public:
	bool checkValue( int value )
	{
		return value > 0;
	}
};

class DataSender : public stellyra::Trackable
{
public:
	stellyra::Event< int > dataReady { this };

	void sendData( int value )
	{
		dataReady( value );
	}
};

class DataReceiver : public stellyra::Trackable
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

class RelayNode : public stellyra::Trackable
{
public:
	stellyra::Event< int > dataIn { this };
	stellyra::Event< int > dataOut { this };

	void setupRelay()
	{
		dataIn.connectLambda( *this, [ this ]( int value ) { dataOut( value ); } );
	}
};

class Checker : public stellyra::Trackable
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
