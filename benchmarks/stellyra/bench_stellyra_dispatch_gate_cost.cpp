/**
 * @file bench_stellyra_dispatch_gate_cost.cpp
 * @brief Isolates the fixed per-emission cost of BasicEvent::operator()'s
 * sender-loop dispatch gate.
 *
 * The dispatch gate is the check that decides whether a trigger dispatches
 * synchronously on the calling thread or gets deferred to the sender's
 * EventLoop.  The dispatch gate is:
 *
 *   EventLoop* senderLoop = owner ? owner->eventLoop() : nullptr;
 *   if ( senderLoop
 *        && ! senderLoop->shouldDispatchDirectlyOnThread( platform::currentThreadId() ) )
 *   { ... defer ... }
 *
 * (see basic_event.h, BasicEvent::operator()). shouldDispatchDirectlyOnThread()
 * itself is:
 *
 *   ( hasDrainThread() && drainThread() == id ) || isDrainingOnThread( id )
 *
 * i.e. up to three atomic loads (_hasDrainThread, _drainThread,
 * _activeDrainThreadId) plus one platform::ThreadId comparison and one
 * currentThreadId() call (std::this_thread::get_id() in thread-safe mode).
 *
 * BM_NoSenderLoop_Baseline  - senderLoop == nullptr (BM_Emit_1Connection's
 *                             case: sender has no EventLoop attached at all).
 *                             Isolates the cost of the outer pointer check
 *                             alone; the gate itself is short-circuited and
 *                             never evaluated
 * BM_CurrentThreadId_Cost   - isolates platform::currentThreadId() alone
 * BM_DispatchGate_DirectHit - senderLoop set, calling thread IS the
 *                             registered drain thread (the common
 *                             "already on the right thread" case) -
 *                             shouldDispatchDirectlyOnThread() returns
 *                             true via the hasDrainThread() branch
 * BM_DispatchGate_Miss      - senderLoop set, calling thread is NOT the
 *                             drain thread and not draining - exercises
 *                             the full false-path through both branches
 *                             before returning false (the shape that
 *                             leads to an actual deferral)
 *
 * Build: same as the other isolated micro-benchmarks in this directory
 * (bench_stellyra_tls_cost.cpp's old build wiring in CMakeLists.txt).
 */

#include <kmac/stellyra/event_loop.h>
#include <kmac/stellyra/platform.h>

#include <benchmark/benchmark.h>

namespace stellyra = kmac::stellyra;

namespace
{
	// dummy owner pointer shape: BasicEvent::operator() reads
	// `this->_impl->owner ? this->_impl->owner->eventLoop() : nullptr`
	// before ever touching the loop; these benchmarks start one level in,
	// at "we already have a (possibly null) EventLoop*", since the owner
	// dereference itself is a plain, un-interesting pointer chase with no
	// platform-specific cost to isolate
	stellyra::EventLoop* const global_noSenderLoop = nullptr;
}

// ============================================================================
// BM_NoSenderLoop_Baseline
// ============================================================================

static void BM_NoSenderLoop_Baseline( benchmark::State& state )
{
	for ( auto _ : state )
	{
		stellyra::EventLoop* senderLoop = global_noSenderLoop;
		bool deferred = false;
		if ( senderLoop )
		{
			deferred = true;  // never reached; mirrors the short-circuit shape
		}
		benchmark::DoNotOptimize( deferred );
		benchmark::DoNotOptimize( senderLoop );
	}
}
BENCHMARK( BM_NoSenderLoop_Baseline );

// ============================================================================
// BM_CurrentThreadId_Cost
// ============================================================================

static void BM_CurrentThreadId_Cost( benchmark::State& state )
{
	for ( auto _ : state )
	{
		stellyra::platform::ThreadId id = stellyra::platform::currentThreadId();
		benchmark::DoNotOptimize( id );
	}
}
BENCHMARK( BM_CurrentThreadId_Cost );

// ============================================================================
// BM_DispatchGate_DirectHit
//
// Calling thread is registered as the drain thread - shouldDispatchDirectly-
// OnThread() returns true on the first (hasDrainThread()) branch without
// needing isDrainingOnThread()'s extra atomic load.
// ============================================================================

static void BM_DispatchGate_DirectHit( benchmark::State& state )
{
	stellyra::EventLoop loop;
	const stellyra::platform::ThreadId self = stellyra::platform::currentThreadId();
	loop.setDrainThread( self );

	for ( auto _ : state )
	{
		const stellyra::platform::ThreadId id = stellyra::platform::currentThreadId();
		const bool directDispatch = loop.shouldDispatchDirectlyOnThread( id );
		benchmark::DoNotOptimize( directDispatch );
	}
}
BENCHMARK( BM_DispatchGate_DirectHit );

// ============================================================================
// BM_DispatchGate_Miss
//
// No drain thread registered and no drain() in flight - both branches of
// shouldDispatchDirectlyOnThread() are evaluated and both return false,
// i.e. the full cost of the check on the path that leads to an actual
// deferral (post() + queue) rather than a direct call.
// ============================================================================

static void BM_DispatchGate_Miss( benchmark::State& state )
{
	stellyra::EventLoop loop;
	const stellyra::platform::ThreadId id = stellyra::platform::currentThreadId();

	for ( auto _ : state )
	{
		const bool directDispatch = loop.shouldDispatchDirectlyOnThread( id );
		benchmark::DoNotOptimize( directDispatch );
	}
}
BENCHMARK( BM_DispatchGate_Miss );

BENCHMARK_MAIN();
