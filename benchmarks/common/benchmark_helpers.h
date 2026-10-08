#ifndef BENCHMARK_HELPERS_H
#define BENCHMARK_HELPERS_H

/**
 * @file benchmark_helpers.h
 * @brief Shared scenario constants, payload types, and utilities used by all
 * per-library benchmark executables.
 *
 * Each library implements the same set of scenarios so results are directly
 * comparable.  The constants here ensure every benchmark uses identical
 * workloads.  Full descriptions of what each scenario tests live in
 * SCENARIOS.md; this list is just the numbering reference.
 *
 * Scenarios
 * ---------
 * 1.   Single-threaded emission, 1 connection
 * 2.   Single-threaded emission, N connections  (see CONNECTION_COUNTS)
 * 2a.  Single-threaded emission, 1 Auto connection (Stellyra, Qt only)
 * 2b.  Single-threaded emission, N Auto connections (Stellyra, Qt only)
 * 2c.  Single-threaded emission, 1 connection, syntax comparison
 * 2d.  Single-threaded emission, N connections, syntax comparison
 * 3.   Connect / disconnect throughput (connect N, emit, disconnect N)
 * 4.   Scoped receiver lifetime:
 *          construct receiver -> connect -> emit -> [disconnect if manual] -> destroy
 * 5.   Cross-thread deferred emission (Stellyra TS, Qt, vdk only)
 * 6.   Thread-affinity forwarding (Stellyra TS, Qt, vdk only)
 * 7a.  Concurrent emission, per-receiver queue (libraries with TS emission)
 * 7b.  Concurrent emission, serialised dispatch (Stellyra TS, Qt forwarding only)
 * 8a.  Disconnect-by-target, linear scan (Stellyra only)
 * 8b.  Bulk disconnect via tracked-connection batch
 * 9.   Contention scaling (fixed receiver count, thread count 1 -> 32)
 *
 * Scenarios 5-7 are implemented only by libraries that natively support the
 * feature; all others mark them N/A in the results table.  Scenario 9 is
 * implemented only by libraries with some form of thread-safe emission.
 */

#include <atomic>
#include <cstdint>

namespace bench {

// ---------------------------------------------------------------------------
// Connection count sweep used in scenario 2
// ---------------------------------------------------------------------------

/// Connection counts for the N-connection sweep (scenario 2).
constexpr int CONNECTION_COUNTS[] = { 1, 10, 100, 1000 };

// ---------------------------------------------------------------------------
// Emission iteration counts
//
// Google Benchmark drives its own iteration loop via state.KeepRunning() /
// state.range(); these constants are used for fixed-count inner loops where
// we need to control the workload size ourselves (e.g. scenario 3 connect/
// disconnect batch).
// ---------------------------------------------------------------------------

/// Number of connect/disconnect cycles in one benchmark iteration (scenario 3).
constexpr int CONNECT_DISCONNECT_BATCH = 1000;

/// Number of emit calls per cross-thread scenario iteration (scenarios 5-7).
constexpr int CROSS_THREAD_EMISSIONS = 1000;

/// Number of emitting threads for concurrent scenarios (scenarios 7a/7b).
constexpr int CONCURRENT_THREAD_COUNT = 4;

/// Fixed receiver count for the contention-scaling benchmarks (scenario 9) -
/// deliberately NOT varied alongside thread count there, so thread count is
/// the only independent variable when isolating how each library's
/// thread-safety mechanism scales under genuine concurrent contention.
constexpr int CONTENTION_RECEIVER_COUNT = 4;

// ---------------------------------------------------------------------------
// Payload types
//
// Using a concrete int payload rather than void so the compiler cannot elide
// the argument passing.  A single int is representative of the common case
// and keeps per-library handler boilerplate minimal.
// ---------------------------------------------------------------------------

/// Event argument type used in all single-argument scenarios.
using EventArg = int;

/// Sentinel payload value emitted in all benchmarks.
constexpr EventArg PAYLOAD = 42;

// ---------------------------------------------------------------------------
// Sink
//
// A volatile sink prevents the compiler from optimising away handler bodies.
// Each benchmark handler should write to this.
// ---------------------------------------------------------------------------

/// Write a value here inside every handler to prevent dead-code elimination.
/// std::atomic<int> rather than plain volatile int: several benchmarks (the
/// "_Parallel" / "SharedEvent" concurrent variants across Stellyra, nod,
/// rocket, and the Qt Direct-connection variant) genuinely allow multiple
/// threads to invoke handlers concurrently with no serialisation of their
/// own - an unsynchronised volatile write from those threads is a data
/// race. memory_order_relaxed at each call site keeps this to a plain
/// store on x86 (no fence), so it doesn't distort the many single-threaded
/// benchmarks that also write here.
inline std::atomic< int > global_sink{ 0 };

} // namespace bench

#endif // BENCHMARK_HELPERS_H
