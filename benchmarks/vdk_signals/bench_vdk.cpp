/**
 * @file bench_vdk.cpp
 * @brief vdk-signals benchmarks - ST/TS-shared scenarios only.
 *
 * Scenarios implemented:
 * 1.  Single-threaded emission, 1 connection
 * 2.  Single-threaded emission, N connections
 * 3.  Connect / disconnect throughput
 * 4.  Scoped receiver lifetime (vdk::context base auto-disconnects on
 *     destruction; explicit disconnect modelled for "safest usage")
 *
 * The genuinely concurrent scenarios (5, 6, 7a) live in five separate
 * files, one per family, always built thread-safe - see
 * bench_vdk_ts_crossthread.cpp, bench_vdk_ts_threadaffinity.cpp,
 * bench_vdk_ts_direct.cpp, bench_vdk_ts_contention.cpp, and
 * bench_vdk_ts_perreceiver.cpp.  Unlike nano/sigslot/rocket's single
 * combined "_ts_2" file, vdk's five concurrent families each need their
 * OWN separate process: running multiple different thread-creating
 * benchmarks sequentially in one process was found to hang reliably on
 * Windows/MinGW (see KNOWN_ISSUES.md for the full diagnosis - a
 * thread_local destructor issue in vdk's own channel handling, confirmed
 * against multiple independent MinGW-w64/GCC bug reports).  This is a
 * genuine, confirmed root cause, unlike the "pragmatic mitigation, unclear
 * mechanism" framing on the other libraries' splits.
 *
 * vdk-signals notes:
 * - Two-file distribution: signals.h + signals.cpp.
 *   CMake compiles signals.cpp into a small static library.
 * - Receivers inherit vdk::context; connections auto-sever on destruction.
 * - vdk::lite namespace provides a single-threaded variant with the same API.
 *
 * Two executables are produced by CMake from THIS file:
 *   bench_vdk_st   - vdk::lite namespace (no locking)
 *   bench_vdk_ts_1 - vdk namespace (thread-safe), same source file, same
 *                    scenarios, built with VDK_THREAD_SAFE to measure the
 *                    namespace's overhead on operations that don't need
 *                    real concurrency to be meaningful
 *
 * Five more executables, bench_vdk_ts_crossthread / _threadaffinity /
 * _direct / _contention / _perreceiver, are produced from their own
 * dedicated source files (always thread-safe, no ST variant).
 *
 * VDK_THREAD_SAFE preprocessor flag selects the namespace.
 */

#include "../common/benchmark_helpers.h"

#include <signals.h>

#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

using namespace bench;

#if defined( VDK_THREAD_SAFE ) && VDK_THREAD_SAFE
namespace vdk_ns = vdk;
#else
namespace vdk_ns = vdk::lite;
#endif

// ============================================================================
// Receiver
// ============================================================================

struct Receiver : public vdk_ns::context
{
	void onFired( bench::EventArg v ) { bench::global_sink.store( v, std::memory_order_relaxed ); }
};

// ============================================================================
// Scenario 1 - single-threaded emission, 1 connection
// ============================================================================

static void BM_Emit_1Connection( benchmark::State& state )
{
	vdk_ns::signal< void( bench::EventArg ) > sig;
	Receiver r;
	sig.connect( &r, &Receiver::onFired );

	for ( auto _ : state )
	{
		sig( bench::PAYLOAD );
	}
}
BENCHMARK( BM_Emit_1Connection );

// ============================================================================
// Scenario 2 - single-threaded emission, N connections
// ============================================================================

static void BM_Emit_NConnections( benchmark::State& state )
{
	const int n = static_cast< int >( state.range( 0 ) );

	vdk_ns::signal< void( bench::EventArg ) > sig;
	std::vector< Receiver > receivers( n );

	for ( auto& r : receivers )
	{
		sig.connect( &r, &Receiver::onFired );
	}

	for ( auto _ : state )
	{
		sig( bench::PAYLOAD );
	}
}
BENCHMARK( BM_Emit_NConnections )
	->Arg( 1 )
	->Arg( 10 )
	->Arg( 100 )
	->Arg( 1000 );

// ============================================================================
// Scenario 3 - connect / disconnect throughput
// ============================================================================

static void BM_ConnectDisconnect( benchmark::State& state )
{
	vdk_ns::signal< void( bench::EventArg ) > sig;

	for ( auto _ : state )
	{
		std::vector< Receiver > receivers( bench::CONNECT_DISCONNECT_BATCH );

		for ( auto& r : receivers )
		{
			sig.connect( &r, &Receiver::onFired );
		}

		sig( bench::PAYLOAD );

		// vdk::context destructor severs connections; clear() drives destruction.
		receivers.clear();
	}
}
BENCHMARK( BM_ConnectDisconnect );

// ============================================================================
// Scenario 4 - scoped receiver lifetime
//
// vdk::context destroys its connections in its destructor.  We rely on the
// destructor path here since vdk has no separate scoped_connection handle.
// ============================================================================

static void BM_ScopedReceiverLifetime( benchmark::State& state )
{
	vdk_ns::signal< void( bench::EventArg ) > sig;

	for ( auto _ : state )
	{
		{
			Receiver r;
			sig.connect( &r, &Receiver::onFired );
			sig( bench::PAYLOAD );
			// r destroyed here; vdk::context destructor severs the connection
		}
	}
}
BENCHMARK( BM_ScopedReceiverLifetime );

BENCHMARK_MAIN();
