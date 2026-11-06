# Known Issues & Limitations

This document is an engineering reference - confirmed toolchain/harness bugs and their fixes, verified correctness properties, and the current library's known limitations - for whoever next touches the benchmark suite or the `AutoDrainThread`/`EventLoop`/mutex-type areas of Stellyra itself.  It is separate from `CHOOSING_A_LIBRARY.md`, which compares libraries for someone choosing between them.

Each entry notes its **confidence level** where applicable - some findings were directly reproduced and verified under a sanitizer; others are the best explanation the evidence supports, applied as a defensive fix, without a fully confirmed mechanism.  Both kinds are noted honestly as which they are.

---

## Toolchain / benchmark-harness findings (Windows/MinGW)

These are findings about the benchmark suite's toolchain and runtime environment, not about Stellyra's own API - they're kept here because they're non-obvious enough that a future "cleanup" pass could easily reintroduce them without this context.

### 1. `std::this_thread::yield()` cannot cross CPU cores on Windows

**TL;DR:** Spin-wait benchmarks hung once thread count reached the machine's core count; fixed by falling back to a real blocking wait after a short spin.

**Symptom:** benchmarks using a spin-then-wait pattern hung specifically once thread count met or exceeded the machine's core count (16 threads on a 16-core machine), across multiple *unrelated* libraries (Stellyra's `SharedEvent`, Qt's zero-locking `DirectConnection`) - ruling out either library's own dispatch mechanism as the cause and pointing at the shared benchmark harness instead.

**Root cause - confirmed directly against Microsoft's own documentation:**
`std::this_thread::yield()` maps to `SwitchToThread()` on Windows, which is documented as: *"Yielding is limited to the processor that is executing the calling thread.  The operating system will not switch execution to another processor, even if that processor is idle."*  Once thread count exceeds core count, a thread spinning on `yield()` while waiting for a different thread on a *different* core can spin indefinitely without making progress.

**Fix:** every spin-wait pattern in the benchmark suite (`Barrier`, `QtBenchBarrier`, `SpinBarrier` in nano/sigslot/rocket/nod/vdk) does a short bounded pure-spin phase, then falls back to a real blocking wait (`condition_variable`, later hardened further - see #2 below).

**Confidence:** high.  Reproduced directly: a 64-thread stress test on a sandbox with 1 real core hung indefinitely with pure `yield()`-spinning and passed cleanly (including under ThreadSanitizer) once the fallback was added.

**Where it applies:** any future benchmark spinning on an atomic condition across more threads than the target machine has cores needs this same fallback, not just the classes listed above.

### 2. `condition_variable::wait(lock, predicate)` - a real or apparent lost wakeup under MinGW

**TL;DR:** A barrier wait occasionally hung despite its condition already being true; fixed by polling with a timed `wait_for()` loop instead of an unbounded `wait(lock, predicate)`.

**Symptom:** a barrier wait hung with a thread stuck inside `arrive()` despite the generation counter it was waiting on having *definitely* already changed - proven because every other participant in the same barrier round had already been released, which is only possible if the stuck thread's own arrival had already been counted.

**Root cause:** not conclusively proven.  `wait(lock, predicate)` is specifically designed to be immune to lost wakeups by rechecking the predicate before actually blocking, so this shouldn't be reproducible by the textbook mechanism.  This project has one other confirmed, independent MinGW-specific standard-library thread-lifecycle bug (#3 below), so a second one in a different primitive isn't implausible, but it isn't confirmed the same way that one was.

**Fix:** replaced the unbounded `wait(lock, predicate)` with `wait_for()` in a loop, rechecking the condition on a timer (5ms) regardless of whether a notification ever arrives - a defensive, self-healing mitigation (even a fully lost `notify_all()` can no longer cause an indefinite hang) rather than a fix targeting a proven root cause.

**Confidence:** medium.  The fix resolved the reported hang in practice (confirmed: the same benchmark that previously hung now runs to completion), which is real evidence for the lost-wakeup theory, but the exact mechanism inside MinGW's `condition_variable` implementation was never traced to source the way #3 was.

**Applied to:** `Barrier` (Stellyra, Qt) and every `SpinBarrier` copy
(nano, sigslot, rocket, nod, vdk).

### 3. `thread_local` destructors are unreliable under MinGW's POSIX threading model

**TL;DR:** A benchmark hung when multiple thread-creating test families ran in the same process, due to MinGW's unreliable `thread_local` destructors; fixed by running each family as its own process, after tracing the cause to MinGW’s unreliable handling of thread_local destructors (affecting any type with a non-trivial one, not something specific to vdk).

**Symptom:** vdk's thread-safe benchmark (`bench_vdk_ts.exe`) hung reliably when multiple *different* thread-creating benchmark families ran sequentially in one process, but never hung when any single family ran alone (even under 10 repetitions of its own thread creation/teardown cycle).  (A similar issue was encountered during work on the Nova logging library's benchmarks and/or unit tests.)

**Root cause - confirmed against multiple independent MinGW-w64/GCC bug reports spanning 2014–2024:** vdk allocates a `thread_local` channel object on first use per OS thread (block-scope, non-trivial destructor calling `close()`).  Under MinGW's POSIX threading model (`emutls`), block-scope `thread_local` objects with non-trivial destructors have a long-documented history of the destructor running against already-invalidated (or already-freed) TLS storage, or not running at all, depending on GCC version and thread-detach path.

**Fix:** rather than running every vdk_ts benchmark in one process, `run_benchmarks.sh` runs each thread-creating benchmark family as its own separate process invocation; `compare.py` merges the resulting JSON fragments before building the report.

**Confidence:** high for the general mechanism (well-documented, cross-referenced against an independently-diagnosed analogous case elsewhere in this project's logging benchmarks); the *specific* hang was resolved by the fix, consistent with the diagnosis.

**Note:** this is a known class of MinGW-w64 bug, not specific to vdk - independently documented (e.g. MinGW-w64 bug tracker, libstdc++ mailing list discussion from MinGW-w64 contributor LIU Hao).  For a `thread_local` type you control (unlike vdk's third-party `channel`), giving it a trivial destructor sidesteps the same root cause entirely, rather than working around it from outside.

### 4. `std::shared_mutex` maps to a slow `pthread_rwlock_t` translation layer on MinGW

**TL;DR:** `SharedEvent` is 6-9x slower than `Event` under contention on MinGW, due to a slow `std::shared_mutex` translation layer on that toolchain - not a bug, but a practical reason to avoid `SharedEvent` for contended workloads there.

**Finding, still true today, not a bug to fix:** `SharedEvent` (backed by `std::shared_mutex`) is 6–9x slower than `Event` (backed by `std::recursive_mutex`) for concurrent parallel dispatch on MinGW GCC 13.1.0, with a *super-linear* contention-scaling curve - roughly 5–7x slower at every thread-count doubling.

**Root cause:** `std::shared_mutex` → `pthread_rwlock_t` → winpthreads' SRWLOCK translation layer on this toolchain.  Reader-writer lock implementations are independently well known to degrade badly under many simultaneous readers; this appears to be a severe case of that on MinGW specifically, not present to the same degree on Linux/glibc.

**Confidence:** high for the relative comparison (directly measured, repeatedly); the exact internal mechanism of *why* winpthreads' rwlock degrades this severely wasn't traced further than identifying the translation layer as the responsible component.

**Practical implication:** `SharedEvent` is not a safe default choice for genuinely contended, concurrent-heavy workloads on MinGW/Windows, despite being the "obviously correct" choice on paper for allowing parallel reads.  `BasicConcurrentEvent` (in `stellyra_extras`, see `ARCHITECTURE.md`) is a lock-free alternative worth benchmarking on this toolchain for that use case, since it never takes a shared/exclusive lock during dispatch at all.

---

## Benchmark harness bugs (fixed)

### 5. Shutdown race: checking `running` before `arrive()`, not after

**TL;DR:** Shutdown sequences intermittently hung because a thread could skip its final `arrive()` call; fixed by calling `arrive()` unconditionally every round.

**Symptom:** intermittent hangs in shutdown sequences, probability increasing with thread count.

**Root cause - confirmed by direct, minimal reproduction:** every emitter-thread loop checked `while (running.load())` *before* calling `arrive()`.  If `running` flipped to `false` in the exact window between a thread finishing one barrier round and re-checking that outer condition, it would exit *without* calling `arrive()` for the final round - leaving whoever else was waiting on that barrier stuck forever.

**Fix:** changed every affected loop from `while (running.load()) { arrive(); ... }` to `for (;;) { arrive(); if (!running.load()) break; ... }` - `arrive()` is now called unconditionally every round, with `running` checked only *after* it returns.

**Confidence:** high - directly reproduced, fixed, and reproduction confirmed clean afterward (10 repeated runs, plus ThreadSanitizer).

### 6. `spinWait()`/`qtSpinWait()` - same `yield()` limitation, different shape

**TL;DR:** A separate spin-wait function hung for the same reason as #1, in a shape with no reusable object to own a mutex/condition variable; fixed with a short spin followed by `sleep_for()` polling.

**Symptom:** a separate hang from #1's `Barrier` fix, because this is a *different* primitive: a standalone function waiting on a plain `std::atomic<int>&` completion counter, not a reusable barrier object.

**Fix:** short bounded pure-spin phase, then `std::this_thread::sleep_for()` in a loop rather than a `condition_variable` - this function takes a plain atomic reference with no persistent object to own a mutex/cv, and `sleep_for()` uses a real timer wait, not subject to `SwitchToThread()`'s same-core limitation.

**Confidence:** the general fix is sound and low-risk regardless of exact mechanism (see the caveat under #1).

---

## Verified correctness properties

### 7. Receiver destroyed before its `AutoDrainThread`/`EventLoop` - safe by design

**TL;DR:** Destroying a receiver before its `EventLoop`/`AutoDrainThread` is safe by design, not by luck - verified clean under AddressSanitizer and ThreadSanitizer across millions of emissions.

A receiver whose declaration order places it *after* its `EventLoop` and `AutoDrainThread` (so it is destroyed *before* either, during normal stack unwind) is safe, by design, not by accident:

- `BasicEventImpl::invokeDeferred()` (and `BasicConcurrentEventImpl`'s equivalent) checks the slot's liveness/generation before touching anything else, and is a silent no-op if the slot is no longer live
- `Trackable::~Trackable()` calls `disconnectAll()`, which disconnects every tracked connection
- Disconnection and any in-flight deferred invocation of the same slot are serialized against each other (via the event's own mutex, for `BasicEvent`; via the epoch guard, for `BasicConcurrentEvent`) - regardless of destruction order

**Verification:** a properly-synchronized, parametrized test (across `Event` and `SharedEvent`) deliberately keeps this "backwards" declaration order, joins all emitter threads before destruction begins, and destroys everything immediately afterward with no wait for queues to drain - clean under both AddressSanitizer and ThreadSanitizer across millions of emissions, for both mutex types.

**Confidence:** high.  This ordering is deliberately exercised by `BM_ConcurrentEmission_PerReceiver` and `BM_ReceiverDeferral_Serialised` as a standing demonstration of this guarantee, not something being "worked around."

---

## Known limitations

These are current, intentional constraints of the design - not bugs - worth knowing before relying on a workaround for one of them.

- **`SharedEvent` does not support reentrant trigger/connect/disconnect.**
  `Event`'s default recursive mutex allows a handler to legitimately connect, disconnect, or re-trigger the same event from within its own invocation; `SharedEvent`'s shared mutex does not support this, and doing it anyway can deadlock.
- **Single-shot connections (`ConnParams::once()`) are unavailable on `SharedEvent`.**
  Enforced by a `static_assert` at the call site: a single-shot handler disconnecting itself while a shared lock is held would deadlock for the same reason as above.
- **`CombiningEvent` is Direct-only and non-reentrant.**
  There is no Deferred variant (the combined return value must be available synchronously), and its internal mutex is a plain, non-recursive mutex - connecting, disconnecting, or re-triggering from inside a connected handler will deadlock.
- **Predicates require exact-arity matching.**
  Unlike handlers (which support partial-argument matching via `createPartial`), a predicate passed to `ConnParams`/`.when()` must accept the event's full `Args...` list; there is no partial-predicate path, NTTP or otherwise.
- **A single event supports at most 65,535 simultaneously connected handlers.**
  `GenData`'s slot index is a `uint16_t` to keep it packed into 4 bytes; this is a high-water mark (freed slots are reused before the handler array grows past a prior peak), not a lifetime-cumulative count.  Exceeding it is a hard assertion failure (`STELLYRA_ASSERT_ALWAYS`), not silent corruption.  If more capacity is ever needed, `forwardTo()` can fan a single source out across more than one event.
- **`BasicConcurrentEvent` is eventually consistent, not linearizable, against topology changes.**
  A concurrent `updateSenderLoop()` / `updateHandlerLoop()` call (from `Trackable::setEventLoop()`) can race a live `dispatch()`: each entry's resolved connection type is read atomically (never torn), but which entries in one dispatch pass see the old vs new topology is not deterministic.  This is a deliberate, documented trade for lock-free dispatch - `BasicEvent`'s equivalent update always runs under the same mutex `dispatch()` holds, so it has no such window.
- **`BasicConcurrentEvent`'s concurrent reader-slot pool is fixed at compile time**
  (`STELLYRA_MAX_CONCURRENT_TRIGGERS`, default 32) across *all* concurrently-active `dispatch()` calls on a process, including reentrant nesting - not per-connection or per-event.  Exceeding it is a hard assertion failure; raise the define if a real workload's concurrent-dispatch depth needs more.
- **Untracked connections have no automatic lifetime safety.**
  A `connectFree()`/`connectLambda()` call made without a `Tracked<T>` or explicit `Trackable&` tracker is not auto-disconnected by anything; the caller is responsible for disconnecting before any captured state (for a lambda) or referenced object (for a free function closing over a pointer) becomes invalid.
- **`EventInspector` cannot report a connected receiver's address or concrete type**
  Only whether a connection is a "method" or "function/lambda" connection (`EventFlags::hasOwner()`) - `Callable` does not expose its bound object pointer generically.  Per-connection receiver identity would need a dedicated metadata field added to `HandlerEntry`.
- **`BasicRecordableEvent` always carries a larger memory footprint than `BasicEvent`**
  Whether or not recording or statistics are actually enabled - intended for development/debugging/testing, not as a drop-in replacement for `Event` in production code where per-event size matters.
- **`platform::Atomic<T>` in single-threaded/bare-metal mode (`STELLYRA_THREAD_SAFE=0`) is `volatile`-based, not lock-free-atomic**
  It is only safe on single-core targets (the only reordering concern there is compiler reordering, e.g. an ISR vs. the main loop).  A multi-core bare-metal or RTOS target needs a custom `platform::Atomic<T>` supplied via `STELLYRA_CUSTOM_ATOMIC` (examples for ARM Cortex-M LDREX/STREX and FreeRTOS critical sections are documented at the bottom of `platform.h`).
- **`STELLYRA_ENABLE_THREAD` is a wired-up CMake option with no effect in the library itself.**
  The root `CMakeLists.txt` defines an `option()` for it and, when turned off, applies `target_compile_definitions( Stellyra INTERFACE STELLYRA_ENABLE_THREAD=0 )` to the `Stellyra` target - so a consumer really can set it, and it really does define that macro.  But no header anywhere in `libs/stellyra` or `libs/stellyra_extras` checks that macro with `#ifdef`/`#if` (confirmed by grepping the full include tree; the only other occurrence is a stale mention in a comment table in `stellyra.h`).  Toggling it currently changes nothing about `EventLoop`, `AutoDrainThread`, or `<thread>` inclusion - that behavior is controlled entirely by `STELLYRA_THREAD_SAFE`.  Either the option should be wired into the headers to do what its description promises, or it should be removed from `CMakeLists.txt` as dead configuration surface.

---

## Library structure notes

### `CombiningEvent` and other extras live in `stellyra_extras`, not the core library

`CombiningEvent` (return-value collection/combining across handlers), `EventRecorder`/`BasicRecordableEvent`, `ConnectionGroup`, and the optional mutex types all live in `libs/stellyra_extras/`, not `libs/stellyra/`.  **Lesson for future documentation or comparison work on this project:** "does Stellyra have X" needs checking against `stellyra_extras` as well as the core library - several real features live there by design, not as an oversight, and a check that only looks in `libs/stellyra/` will report a false negative.

### `stellyra_extras/mutex_types.h` - `FastRecursiveMutex` and `SpinRecursiveMutex`

Both are experimental, atomic-owner-plus-depth-counter reentrant mutex types, not used by any shipped `Event` alias - provided so a given toolchain/target can be benchmarked against `std::recursive_mutex`.  `SpinRecursiveMutex`spins (bounded pure-spin, then yields) instead of falling back to a blocking `std::mutex`.  Both are verified under a single-threaded reentrancy test and a multi-thread/high-emission-count stress test (clean under ThreadSanitizer, Linux/libstdc++); neither has had long-duration soak testing or testing on Windows/MinGW at time of writing.
