/**
 * @file bench_boost.cpp
 * @brief Boost.Signals2 benchmark scaffold.
 *
 * STATUS: Commented out.  Boost.Signals2 is excluded from the default build
 * due to the size of the Boost dependency.  The CMake target is present but
 * disabled; see benchmarks/boost_signals2/CMakeLists.txt.
 *
 * To enable:
 * 1. Install Boost (Windows: vcpkg install boost-signals2, or download from
 *    boost.org and add to CMAKE_PREFIX_PATH).
 * 2. Uncomment the bench_boost target in boost_signals2/CMakeLists.txt.
 * 3. Uncomment the implementation below.
 *
 * Performance note:
 *   sigslot (palacaze) was designed as a Boost.Signals2 replacement and has
 *   broadly comparable single-threaded performance with lower overhead in
 *   the common cases.  Where Boost.Signals2 results are absent, sigslot_st
 *   results serve as an approximate lower bound for the Boost figure.
 *   The key Boost.Signals2 differentiators not captured by sigslot are:
 *     - track_foreign(): tracks arbitrary shared_ptr, not just library types
 *     - combiner support: custom return-value combiners
 *     - signal::num_slots() and similar introspection
 *
 * Scenarios that would be implemented (matching all other libraries):
 * 1.  Single-threaded emission, 1 connection
 * 2.  Single-threaded emission, N connections
 * 3.  Connect / disconnect throughput
 * 4.  Scoped receiver lifetime (track() + shared_ptr)
 * Scenarios 5, 6, 7a, 7b: N/A - no built-in event loop or deferred dispatch.
 */

// #include "../common/benchmark_helpers.h"
//
// #include <boost/signals2.hpp>
//
// #include <benchmark/benchmark.h>
//
// #include <memory>
// #include <vector>
//
// using namespace bench;
//
// using Signal = boost::signals2::signal< void( EventArg ) >;
//
// struct Receiver
// {
// 	void onFired( EventArg v ) { global_sink = v; }
// };

// static void BM_Emit_1Connection( benchmark::State& state )
// {
// 	Signal sig;
// 	auto r = std::make_shared< Receiver >();
// 	sig.connect( boost::bind( &Receiver::onFired, r, boost::placeholders::_1 ) );

// 	for ( auto _ : state )
// 	{
// 		sig( PAYLOAD );
// 	}
// }
// BENCHMARK( BM_Emit_1Connection );

// static void BM_Emit_NConnections( benchmark::State& state )
// {
// 	const int n = static_cast< int >( state.range( 0 ) );
// 	Signal sig;
// 	std::vector< std::shared_ptr< Receiver > > receivers;
// 	receivers.reserve( n );
// 	for ( int i = 0; i < n; ++i )
// 	{
// 		auto r = std::make_shared< Receiver >();
// 		sig.connect( boost::bind( &Receiver::onFired, r, boost::placeholders::_1 ) );
// 		receivers.push_back( std::move( r ) );
// 	}
// 	for ( auto _ : state )
// 	{
// 		sig( PAYLOAD );
// 	}
// }
// BENCHMARK( BM_Emit_NConnections )->Arg(1)->Arg(10)->Arg(100)->Arg(1000);

// static void BM_ConnectDisconnect( benchmark::State& state )
// {
// 	Signal sig;
// 	for ( auto _ : state )
// 	{
// 		std::vector< boost::signals2::connection > handles;
// 		handles.reserve( CONNECT_DISCONNECT_BATCH );
// 		std::vector< std::shared_ptr< Receiver > > receivers;
// 		receivers.reserve( CONNECT_DISCONNECT_BATCH );
// 		for ( int i = 0; i < CONNECT_DISCONNECT_BATCH; ++i )
// 		{
// 			auto r = std::make_shared< Receiver >();
// 			handles.push_back( sig.connect( boost::bind( &Receiver::onFired, r, boost::placeholders::_1 ) ) );
// 			receivers.push_back( std::move( r ) );
// 		}
// 		sig( PAYLOAD );
// 		for ( auto& c : handles ) { c.disconnect(); }
// 	}
// }
// BENCHMARK( BM_ConnectDisconnect );

// static void BM_ScopedReceiverLifetime( benchmark::State& state )
// {
// 	Signal sig;
// 	for ( auto _ : state )
// 	{
// 		{
// 			auto r = std::make_shared< Receiver >();
// 			// track() holds a weak_ptr; connection is silently skipped when
// 			// the shared_ptr expires.
// 			sig.connect(
// 				boost::signals2::signal< void(EventArg) >::slot_type(
// 					boost::bind( &Receiver::onFired, r.get(), boost::placeholders::_1 )
// 				).track_foreign( r )
// 			);
// 			sig( PAYLOAD );
// 		}
// 		// r destroyed; Boost skips dead slot on next emission (lazy cleanup)
// 	}
// }
// BENCHMARK( BM_ScopedReceiverLifetime );

// BENCHMARK_MAIN();

// Placeholder so the translation unit is not empty when commented out.
int main() { return 0; }
