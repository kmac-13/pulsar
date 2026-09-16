#include "test_helpers.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Trackable::trackConnection() vs Trackable::disconnectAll() race.
//
// trackable.h documents an invariant: _mutex is always released before any
// call that may acquire an EventImpl mutex (isHandlerConnected(), called by
// trackConnection()'s prune step, is exactly such a call).  The safe pattern
// - take a COPY of _trackedConnections under _mutex, do the EventImpl calls
// unlocked, then reacquire _mutex - must never leave the real list
// observably empty except during disconnectAll()'s own drain.  A version
// that instead swaps the real list out (leaving it briefly, genuinely
// empty) opens a window where a concurrent disconnectAll() sees nothing to
// disconnect, and a connection added around that window survives the call
// entirely - permanently un-disconnected, since it was also never
// re-inserted into what disconnectAll() drained.
//
// This test connects many new connections to a single shared receiver from
// one thread while another thread concurrently calls disconnectAll() on
// that same receiver, then runs one final *sequential* disconnectAll() and
// checks that every connection created during the race is now actually
// disconnected.  A connection that survives the final sweep was dropped
// from the tracked list without ever being disconnected - the failure mode
// this test exists to catch.
// ---------------------------------------------------------------------------

namespace {

class RaceReceiver : public pulsar::Trackable
{
public:
	void onFire() { fireCount.fetch_add( 1, std::memory_order_relaxed ); }
	std::atomic< int > fireCount{ 0 };
};

} // namespace

template< typename MutexType >
class TrackConnectionRace : public ::testing::Test {};

using TrackConnectionRaceMutexTypes = ::testing::Types<
	pulsar::platform::RecursiveMutex,
	pulsar::platform::SharedMutex >;
TYPED_TEST_SUITE( TrackConnectionRace, TrackConnectionRaceMutexTypes );

TYPED_TEST( TrackConnectionRace, ConnectionsSurviveConcurrentDisconnectAll )
{
	constexpr int TRIALS = 50;
	constexpr int CONNECTIONS_PER_TRIAL = 200;

	for ( int trial = 0; trial < TRIALS; ++trial )
	{
		RaceReceiver receiver;

		// senders must outlive the trial so connection state can be
		// queried once both threads have finished
		std::vector< std::unique_ptr< pulsar::BasicEvent< TypeParam > > > senders( CONNECTIONS_PER_TRIAL );
		std::vector< pulsar::Connection > connections( CONNECTIONS_PER_TRIAL );

		std::atomic< int > connected{ 0 };

		std::thread connector( [ & ]() {
			for ( int i = 0; i < CONNECTIONS_PER_TRIAL; ++i )
			{
				senders[ i ] = std::make_unique< pulsar::BasicEvent< TypeParam > >();
				connections[ i ] = senders[ i ]->connect( receiver, &RaceReceiver::onFire );
				connected.fetch_add( 1, std::memory_order_relaxed );
			}
		} );

		std::thread disconnector( [ & ]() {
			// give the connector a head start so there are already some
			// pre-existing tracked connections by the time disconnectAll()
			// runs - widens trackConnection()'s prune loop, the same
			// condition that originally exposed the race
			while ( connected.load( std::memory_order_relaxed ) < CONNECTIONS_PER_TRIAL / 4 )
			{
				std::this_thread::yield();
			}
			receiver.disconnectAll();
		} );

		connector.join();
		disconnector.join();

		// final, sequential sweep - anything still connected here was
		// dropped from the tracked list without ever being disconnected
		receiver.disconnectAll();

		for ( int i = 0; i < CONNECTIONS_PER_TRIAL; ++i )
		{
			EXPECT_FALSE( connections[ i ].isConnected() )
				<< "trial " << trial << ", connection " << i
				<< " survived disconnectAll() - dropped from the tracked list without being disconnected";
		}
	}
}
