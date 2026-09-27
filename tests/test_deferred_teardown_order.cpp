#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Deferred Teardown Order
//
// Verifies a specific Pulsar correctness property: destroying Receiver
// objects BEFORE the AutoDrainThread/EventLoop they're connected through
// (the "backwards" declaration order deliberately kept in
// BM_ConcurrentEmission_PerReceiver and BM_ReceiverDeferral_Serialised in
// the benchmark suite) does not cause a use-after-free, even when
// Deferred commands are still queued and unprocessed at the moment
// destruction begins.
//
// The claimed mechanism:
// - emitting through a Deferred connection posts a Task capturing only
//   the slot INDEX and GENERATION, not a resolved/bound callable
//   (basic_event_impl.h, ResolvedConnectionType::Deferred case)
// - BasicEventImpl::invokeDeferred() checks isSlotLive(index, generation)
//   before touching anything else, and returns immediately if the slot is
//   no longer live
// - Trackable::~Trackable() calls disconnectAll(), which calls
//   BasicEventImpl::disconnectHandler(index, generation) for every tracked
//   connection
// - disconnectHandler() and invokeDeferred() both take the SAME
//   BasicEventImpl mutex for their entire duration, so a receiver's
//   disconnection and any in-flight deferred invocation of its handler
//   are properly serialized against each other
//
// Parametrized across Event (RecursiveMutex) and SharedEvent
// (SharedMutex) - the mechanism above is structural to BasicEvent
// itself, not specific to one mutex type, so it should hold for both.
// SingleThreadedEvent is deliberately excluded: this test is inherently
// multi-threaded, and NullMutex provides no synchronization by design.
//
// This test does NOT assert that every emission is processed.  Events
// still queued but not yet drained at the moment a Receiver is destroyed
// are EXPECTED to be silently dropped by the generation check - that's
// the correct, intended behaviour, not a bug.  What this test actually
// checks is the absence of a use-after-free (run under AddressSanitizer)
// and the absence of a data race (run under ThreadSanitizer), across
// many trials, with emitter threads properly joined before any
// destruction begins.
//
// STRESS TEST, not a fast unit test: defaults are sized for a quick CI
// pass on SharedEvent's worst case (std::shared_mutex has repeatedly
// measured far more expensive per-emission than std::recursive_mutex on
// MinGW/Windows specifically).  Two independent options scale up for full
// sanitizer validation: PULSAR_STRESS_TRIALS (trial count, for statistical
// power across independent setup/teardown timing) and
// PULSAR_STRESS_EMISSIONS_PER_THREAD (per-trial volume).
// ---------------------------------------------------------------------------

#include <kmac/pulsar/auto_drain_thread.h>

#include <atomic>
#include <cstdlib>
#include <list>
#include <thread>
#include <vector>

namespace
{

int stressTrialCount()
{
	if ( const char* env = std::getenv( "PULSAR_STRESS_TRIALS" ) )
	{
		const int n = std::atoi( env );
		if ( n > 0 )
		{
			return n;
		}
	}
	return 20;  // fast-CI default; set PULSAR_STRESS_TRIALS to scale up
}

int stressEmissionsPerThread()
{
	if ( const char* env = std::getenv( "PULSAR_STRESS_EMISSIONS_PER_THREAD" ) )
	{
		const int n = std::atoi( env );
		if ( n > 0 )
		{
			return n;
		}
	}

	// deliberately small for the fast-CI default - SharedEvent
	// (std::shared_mutex) has repeatedly measured far more expensive
	// per-emission than Event (std::recursive_mutex) on MinGW/Windows
	// specifically (see this project's benchmark suite findings), so
	// this default is sized for SharedEvent's worst case, not Event's
	return 2000;
}

constexpr int RECEIVERS_PER_TRIAL = 6;
constexpr int EMITTER_THREADS = 4;

struct Receiver : pulsar::Trackable
{
	// actually touches member state on every invocation, so a
	// use-after-free would be a real, detectable write to freed memory,
	// not a no-op call through a dangling-but-unused this pointer
	std::atomic< long long > total{ 0 };

	void onFired( int v )
	{
		total.fetch_add( v, std::memory_order_relaxed );
	}
};

} // namespace

template< typename EventType >
class DeferredTeardownOrderTest : public ::testing::Test
{
};

// see integration note 3 above - replace with the project's existing
// thread-safe type alias if one already covers Event + SharedEvent
using DeferredTeardownOrderEventTypes = ::testing::Types< pulsar::Event< int >, pulsar::SharedEvent< int > >;

TYPED_TEST_SUITE( DeferredTeardownOrderTest, DeferredTeardownOrderEventTypes );

TYPED_TEST( DeferredTeardownOrderTest, SurvivesReceiverDestroyedBeforeDrainer )
{
	const int trials = stressTrialCount();
	const int emissionsPerThread = stressEmissionsPerThread();
	long long totalAcrossAllTrials = 0;

	for ( int trial = 0; trial < trials; ++trial )
	{
		SCOPED_TRACE( testing::Message() << "trial " << trial << "/" << trials );

		TypeParam sender;

		// deliberately backwards: receivers (declared last) destructs
		// FIRST, before drainers' final-drain destructor runs. This is
		// the exact property under test, matching the ordering
		// deliberately kept in the benchmark suite
		std::list< pulsar::EventLoop > loops;
		std::list< pulsar::AutoDrainThread > drainers;
		std::list< Receiver > receivers;

		for ( int i = 0; i < RECEIVERS_PER_TRIAL; ++i )
		{
			loops.emplace_back();
			drainers.emplace_back( loops.back() );
			receivers.emplace_back();
			receivers.back().setEventLoop( &loops.back() );
			sender.template connect< &Receiver::onFired >( receivers.back(), 0, pulsar::ConnectionType::Deferred );
		}

		std::vector< std::thread > emitters;
		emitters.reserve( EMITTER_THREADS );
		for ( int t = 0; t < EMITTER_THREADS; ++t )
		{
			emitters.emplace_back( [ &sender, emissionsPerThread ]()
			{
				for ( int i = 0; i < emissionsPerThread; ++i )
				{
					sender( 1 );
				}
			} );
		}

		// properly joined: nothing is still running against `sender` once this returns
		for ( auto& t : emitters )
		{
			t.join();
		}

		// no wait for the drain threads to catch up: destruction begins
		// immediately, on purpose, to maximize the chance that some
		// emitted events are still queued (or an invokeDeferred() call
		// still in-flight) at the exact moment receivers starts being
		// destroyed; receivers, drainers, loops, and sender all
		// destruct here, in that order (reverse of declaration)
		for ( auto& r : receivers )
		{
			totalAcrossAllTrials += r.total.load( std::memory_order_relaxed );
		}
	}

	// sanity check only, not a correctness assertion on exact counts:
	// confirms the mechanism isn't silently a complete no-op (which
	// would trivially "pass" the absence-of-crash check above for the
	// wrong reason) - late-arriving emissions being dropped is expected
	// and correct, so this deliberately does not check for the full
	// theoretical maximum (trials * threads * emissions_per_thread)
	EXPECT_GT( totalAcrossAllTrials, 0 )
		<< "no handler invocations were observed across any trial - "
		<< "either Deferred dispatch isn't working at all, or something "
		<< "about this test's setup is wrong, not the teardown-order "
		<< "property this test exists to check";
}
