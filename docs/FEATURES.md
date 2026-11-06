# Stellyra Library - Feature Summary

## Feature List

* **events** - type-safe, templated events with automatic connection management; mutex type is chosen per-instance via a template parameter
* **lifetime tracking (`Trackable`/`Anchor`)** - connections auto-disconnect when an owning or tracked object is destroyed
* **non-invasive tracking (`Tracked<T>`)** - pair a receiver that can't or shouldn't inherit `Trackable` with a standalone `Trackable`/`Anchor`, getting the same automatic lifetime safety without changing its inheritance hierarchy
* **connections** - handles for manual control over a single event-to-handler link (disconnect, block/unblock)
* **connection types (`Direct`/`Deferred`/`Auto`)** - synchronous same-thread execution, deferred execution on the receiver's EventLoop, or automatic choice between the two (resolved at connect time and `EventLoop` update time)
* **single-shot connections** - auto-disconnect after the first trigger
* **conditional connections** - predicate-gated handler execution
* **priority connections** - control handler execution order
* **partial-argument matching** - a `connect`/`connectFree` handler can take only a leading prefix of the event's argument types, with the trailing arguments dropped automatically
* **multiple callable types** - member functions, lambdas (captured or move-only), free functions (including with no receiver object to track), and functors, all without `std::function` overhead
* **event loops (`EventLoop`/`AutoDrainThread`)** - thread-safe task queue, drained manually or by a dedicated background thread
* **sender thread affinity** - triggers from other threads automatically deferred to protect sender state without a hand-written mutex
* **event loop migration** - move an object, and its pending events, to a different EventLoop at runtime
* **serialized or concurrent events** - a std::shared_mutex-backed event (`SharedEvent`) or a lock-free, `EpochDomain`-backed alternative (`ConcurrentEvent`) for genuinely concurrent dispatch from multiple threads at once, rather than one thread at a time behind an exclusive lock (`Event`)
* **properties (`Property`/`ReadOnlyProperty`/`ComputedProperty`/`ConstProperty`)** - reactive value wrappers with a built-in `changed` event, for values that are read-only externally, computed from another value, or immutable
* **scoped connections (`ScopedConnection`)** - RAII auto-disconnect
* **blocking and RAII block guard** - temporarily disable a single connection (`block()`/`unblock()`), or pause an entire event for a scope via the RAII `BlockGuard` wrapper
* **connection grouping (`ConnectionGroup`)** - manage multiple connections as a single disconnect-together unit
* **result aggregation (`CombiningEvent` + combiners)** - aggregate handler return values (`LogicalAnd`/`Or`, `Sum`, `Average`, `Product`, `Maximum`/`Minimum`, `First`/`Last`, `CountTrue`, etc.)
* **restricted triggering (`PrivateEvent`/`PEvent`)** - restrict triggering to the owning/friend class while still allowing external connections
* **recording/playback (`RecordableEvent`)** - record emissions with timestamps; replay them at original, scaled, or immediate speed; timing statistics and CSV export
* **introspection (`EventInspector`)** - read-only inspection of an event's live connections, for debugging
* **event forwarding (`forwardTo`, `forwardFrom`)** - relay one event's emissions into another
* **bare-metal/zero-overhead mode (`STELLYRA_THREAD_SAFE`)** - library-wide compile switch for bare-metal/single-threaded builds, removing mutex/atomic overhead entirely
* **thread-safe connection management** - connect/disconnect/block/unblock are safe to call from any thread
* **compile-time type safety** - handler signature mismatches caught at compile time with readable static-assert messages

## Core Features

### Event System
**Type-safe events with automatic connection management**
```cpp
stellyra::Event< int, std::string > dataReady { trackable };
```
Events notify multiple handlers when triggered.  Connections automatically clean up when objects are destroyed, preventing dangling pointers.

### Concurrent Events
**Lock-free dispatch for genuinely parallel triggers**
```cpp
stellyra::ConcurrentEvent< int > sensorReading { this };
stellyra::SingleThreadedConcurrentEvent< int > sensorReadingST { this };  // same model, no atomics
```
`SharedEvent` already supports genuinely concurrent dispatch from multiple threads via a shared lock on a `std::shared_mutex` - unlike `Event`'s plain recursive mutex, which serializes dispatch to one thread at a time.  `ConcurrentEvent` (in extras) goes further: its dispatch path never touches a mutex at all, relying entirely on a lock-free `EpochDomain` instead, so it avoids both the shared lock's acquisition overhead and the blocking that happens when `connect()`/`disconnect()` hold `SharedEvent`'s exclusive lock while a dispatch is in flight.  That lock-free benefit also disappears once the event is given an owner `EventLoop` - see the FAQ entry "Does SharedEvent or ConcurrentEvent still dispatch concurrently if it has an owner EventLoop?" for why.

### Trackable Class
**Foundation for lifetime management**
```cpp
// option 1: inherit (simplest)
class AppClass : public stellyra::Trackable {
	stellyra::Event< int, std::string > dataReady { this };
};

// option 2: pair with a standalone Trackable/Anchor (non-invasive - no inheritance required)
class AppClass {
	stellyra::Anchor _lifetime;  // standalone tracker; pass Tracked{ *this, _lifetime } when connecting
	stellyra::Event< int, std::string > dataReady { &_lifetime };
};
```
Inheriting `stellyra::Trackable` is the simplest path and gives the cleanest connect syntax.  Pairing a non-inheriting receiver with a standalone `stellyra::Trackable` (alias `stellyra::Anchor`) via `Tracked<T>{ receiver, tracker }` is fully supported for classes that cannot - or should not - modify their inheritance hierarchy.  Both approaches provide the same automatic connection cleanup when the tracker is destroyed.

### Connections
**Handles for managing event-to-handler links**
```cpp
stellyra::Connection conn = event.connect( receiver, &Receiver::handler );
```
Store connections for manual control (disconnect, block/unblock).  Auto-disconnect when objects destroyed.

---

## Connection Types

Resolved at connection-time, and re-resolved whenever sender or receiver EventLoop changes via setEventLoop():

- **Direct** always resolves to Direct.
- **Deferred** resolves to Deferred if the receiver has an EventLoop; otherwise to None.
- **Auto** resolves to Direct if sender and receiver share the same EventLoop (including the case where both are null); otherwise to Deferred if the receiver has a valid EventLoop; otherwise to None (this last case requires the sender to have a valid EventLoop too - if neither side has one, that’s the shared-EventLoop case above, which resolves to Direct).

`None` is not a connection type you can request - there is no ConnectionType::None.  It’s the resolved outcome when Deferred or Auto can’t find a usable receiver loop.  See below for what this means in practice.

### Direct Connection
**Immediate execution in sender's thread**
```cpp
event.connect( receiver, slot, stellyra::ConnectionType::Direct );
```
Handler executes synchronously during trigger, in the same thread.

### Deferred Connection
**Asynchronous execution in receiver's thread**
```cpp
event.connect( receiver, slot, stellyra::ConnectionType::Deferred );
```
Handler execution deferred to receiver's event loop, runs asynchronously.  If receiver has no event loop, this resolves to `None` (see below).

### Auto Connection
**Default, automatically chooses Direct, Deferred, or None based on EventLoop identity**
```cpp
event.connect( receiver, slot, stellyra::ConnectionType::Auto );  // default, recommended
```
Resolves to Direct if sender and receiver share an EventLoop, Deferred if the receiver has a different one, or None if the receiver has none.  Best for most use cases.

### None (Resolved Outcome, Not Requestable)
**Connection stays live but is silently skipped**

A connection resolves to `None` when `Deferred` or `Auto` can’t find a valid receiver EventLoop to dispatch into.  The connection is not disconnected - it stays in place, and its handler is simply skipped on every trigger() until the topology changes.  Resolution is re-checked automatically the next time either side (sender, receiver) calls setEventLoop(), so giving the receiver an EventLoop later reactivates the connection with no need to reconnect.

---

## Advanced Connections

### Single-Shot Connections
**Auto-disconnect after first trigger**
```cpp
event.connect( receiver, &Receiver::oneTimeHandler, event.params().once() );
```
Handler called once, then connection automatically removed.  Perfect for initialization or one-time events.

### Conditional Connections
**Execute only when predicate returns true**
```cpp
event.connect( receiver, slot, event.params( []( int x ) { return x > 0; } ) );
```
Handler called only if condition passes.  Great for filtering or validation when such logic should remain outside of handler code.

### Priority Connections
**Control execution order with integer priorities**
```cpp
event.connect( receiver, slot, event.params().prio( 100 ) );  // higher = earlier
```
Handlers execute in priority order (highest first).  Useful for validation pipelines.

### Free Function Connections
**Connect without a receiver object**
```cpp
event.connectFree( []( int x ) { std::cout << x; } );
```
For stateless handlers that don't need object lifetime tracking.

---

## Event Loops

### EventLoop
**Thread-safe task queue, manually or automatically drained**
```cpp
// manual: caller drives processing (analogous to
// QCoreApplication::exec() - the calling thread blocks and drives the loop)
stellyra::EventLoop loop;
while ( running )
{
	loop.drain();
}

// automatic: a dedicated background thread wraps the loop and drains it
// whenever work is posted (analogous to QThread::exec())
stellyra::EventLoop autoLoop;
stellyra::AutoDrainThread drainer( autoLoop );  // starts the drain thread; stop() or its destructor joins it
```
Processes deferred events from the calling thread on demand (`drain()`) or from a dedicated background thread that wakes automatically when work is posted (`AutoDrainThread`).  Unlike Qt's `QCoreApplication::exec()`, Stellyra's loops have no OS-level event integration - they drain only Stellyra's internal task queue.

A manual EventLoop in a **single-threaded** application still provides real architectural benefits beyond threading: deferred execution prevents handler reentrancy, isolates the emitter from slow handlers, absorbs event bursts, and makes state machines safer by flattening recursive call chains.  See `USE_CASES.md` for detailed discussions.

### Sender Thread Affinity
**Automatic deferral to sender's thread**
```cpp
sender->setEventLoop( &loop );
// events triggered from other threads automatically deferred to loop
```
Protects sender state without requiring the sender to add its own mutex.  Events triggered from any thread other than the loop’s drain thread are deferred into the loop’s queue instead of running immediately on the calling thread. (For senders with no event loop, synchronous event triggers from other threads are still possible.)

### Event Loop Migration
**Move objects between event loops**
```cpp
object->setEventLoop( &newLoop );
```
Transfer object to a different EventLoop, including pending events.  (Events already claimed by the old loop’s current drain batch finish there - migratePendingTo() only ever operates on the queue, never on that batch.  Anything still queued does migrate, though if the old loop is being kept continuously busy, it may have already swept a given task into its current batch before migration gets a turn at the queue.)

---

## RAII Management

### ScopedConnection
**Auto-disconnect when going out of scope**
```cpp
{
	auto conn = event.connect( receiver, slot ).scoped();
	// auto-disconnects here
}
```
RAII wrapper for connections.  Ensures cleanup in exception-safe way.

### BlockGuard
**Temporarily block an event for a scope**
```cpp
{
	auto guard = event.blockGuard();  // event is now blocked
	event( 42 );                      // dropped while blocked
	// guard destroyed here - event automatically unblocked
}
```
RAII wrapper for block()/unblock().  Blocks the whole event - not a single connection - for the guard’s lifetime, with exception-safe automatic unblocking even if the scope exits early.

### ConnectionGroup
**Manage multiple connections as a unit**
```cpp
stellyra::ConnectionGroup connections;
connections += event1.connect( ... );
connections += event2.connect( ... );
// all disconnect together when destroyed
```
Convenient for managing related connections, especially in destructors.

---

## Return Value Handling

### CombiningEvent
**Aggregate return values from all handlers**
```cpp
stellyra::CombiningEvent< bool, stellyra::Combiners::LogicalAnd<>, int > validate { this };
```
Collects return values from handlers and combines them with specified logic.

### Built-in Combiners
**Pre-made aggregation strategies**
- `LogicalAnd` - all must return true
- `LogicalOr` - at least one must return true
- `Sum` - sum all values
- `Mean` (`Average`) - average of all values
- `Product` - multiply all values together
- `Maximum` / `Minimum` - get max/min value
- `First` / `Last` - return only the first/last handler’s value (based on connection order)
- `FirstNonDefault` / `LastNonDefault` - first/last non-default value (based on connection order)
- `CountTrue` - count how many returned true

---

## Encapsulation

### PrivateEvent (PEvent)
**Restrict event triggering to friend class**
```cpp
class Button : public stellyra::Trackable {
public:
	stellyra::PrivateEvent< Button, int > clicked { this };  // only instances of Button can trigger
};
```
Compile-time access control.  External code can connect but not trigger.

---

### Reactive Properties
**Reactive values with a built-in `changed` event**
```cpp
stellyra::Property< float > volume { this, 1.0f };
volume = 0.5f;       // fires changed(0.5f) only if the value actually differs
volume.changed.connect( this, &MyClass::onVolumeChanged );
```
A small family of reactive wrappers - `Property<T>` (readable and writable by anyone), `ReadOnlyProperty<Owner, T>` (writable only by `Owner`), `ComputedProperty`/`ComputedPropertyFn` (value derived from another property via a member function or lambda), and `ConstProperty<T>` (immutable; `changed` exists but never fires) - all share the same read interface (implicit conversion, `get()`, `changed`), so code consuming a property doesn't need to know which variant it's reading.  `changed` only fires when the new value actually differs; `setForce()` overrides that for in-place-mutated containers where `operator==` can't tell, and `setQuiet()` sets a value without firing `changed` at all.

---

## Event Recording and Replay

### RecordableEvent
**Event with built-in recording and statistics**
```cpp
stellyra::RecordableEvent< int > event { this };
```
Extended Event that can record emissions and provide performance statistics.  Use for debugging/testing; use base Event for production.

### Start/Stop Recording
**Capture event triggers with timestamps**
```cpp
event.recorder().startRecording();       // record all
event.recorder().startRecording( 100 );  // keep last 100
event.recorder().stopRecording();
```
Records event triggers with timing information.  Optional limit keeps only recent N recordings.

### Pause/Resume Recording
**Temporarily halt recording**
```cpp
event.recorder().pauseRecording();
event.recorder().resumeRecording();
```
Pause and resume without clearing recorded data.

### Replay
**Re-trigger recorded events**
```cpp
event.recorder().replay();  // re-trigger all recorded events immediately
```
Triggers all recorded events in sequence.  Useful for reproducing bugs or testing.

### Replay With Timing
**Replay with original timing preserved**
```cpp
event.recorder().replayWithTiming();
```
Sleeps between emissions to match original timing.  Great for simulating real-world scenarios.

### Replay With Speed
**Replay at different speeds**
```cpp
event.recorder().replayWithSpeed( 2.0 );   // 2x faster
event.recorder().replayWithSpeed( 0.5 );   // half speed
```
Speed up or slow down replay for testing different timing conditions.

### Get Recordings
**Access recorded data**
```cpp
auto recordings = event.recorder().getRecordings();
for ( const auto& rec : recordings ) {
	// rec.args - tuple of arguments
	// rec.timestamp - absolute time
	// rec.relativeTime - time since recording started
}
```
Access raw recording data for analysis or export.

### Timing Statistics
**Analyze event trigger patterns**
```cpp
auto stats = event.recorder().getTimingStats();
// stats.count - number of event triggers
// stats.minInterval - shortest time between event triggers
// stats.maxInterval - longest time between event triggers
// stats.avgInterval - average time between event triggers
// stats.totalDuration - total recording duration
```
Get statistical analysis of event timing.

### Export to CSV
**Save recordings for offline analysis**
```cpp
event.recorder().exportToCSV( "events.csv" );
```
Export all recordings to CSV file for analysis in Excel or other tools.

### Dump Recordings
**Human-readable output**
```cpp
event.recorder().dumpRecordings( std::cout );
```
Print formatted recording information.

### Performance Statistics
**Track emission performance**
```cpp
event.enableStatistics();
// ... emit events ...
auto stats = event.getStatistics();
// stats.emissionCount - total emissions
// stats.avgEmitTimeNs - average time per emission
// stats.connectionCount - current connections
```
Monitor how many times event was emitted and average execution time.

---

## Inspection & Debugging

### EventInspector
**Query event state and connections**
```cpp
stellyra::EventInspector inspector( event );
size_t count = inspector.connectionCount();
```
Get detailed information about connections, receivers, types, and states.

### Connection Information
**Detailed connection metadata**
```cpp
auto info = inspector.getConnectionInfo();
// returns receiver type, connection type, blocked status, etc.
```
View all connection details for debugging.

### Dump Methods
**Human-readable output**
```cpp
inspector.dumpConnections( std::cout );
inspector.dumpConnectionGraph( std::cout );
```
Print formatted connection information or visual graph.

---

## Triggering Methods

### Three Equivalent Syntaxes
**operator() / trigger() / emit()**
```cpp
event( 42 );          // function-call style (recommended)
event.trigger( 42 );  // alternative
event.emit( 42 );     // for those comfortable with Qt, libsigc++
```
All three do the same thing.  Choose your preferred style.

---

## Additional Features

### Event Forwarding
**Relay emissions to another event**
```cpp
event1.forwardTo( event2 );    // event1 emissions trigger event2
event2.forwardFrom( event1 );  // event1 emissions trigger event2 for cases where event2 is a PrivateEvent
```
Chain events together.  Useful for proxy patterns.

### Connection Blocking
**Temporarily disable without disconnecting**
```cpp
conn.block();    // disable
conn.unblock();  // re-enable
```
Pause handler execution without losing connection.  Can be used with `BlockGuard` (see above).

### Multiple Callable Types
**Support for various handler types**
- member function pointers: `&Class::method`
- lambdas: `[]( int x ) { }`
- lambdas with captures: `[ & ]( int x ) { }`
- move-only captures: `[ ptr = std::move( ptr ) ]() { }`
- free functions: `&freeFunction`
- functors: `FunctorClass{}`

All supported without `std::function` overhead.

---

## Platform Abstractions

### STELLYRA_THREAD_SAFE
**Library-wide thread safety switch**
```cmake
# Disable for single-threaded / bare-metal targets
target_compile_definitions( app PRIVATE STELLYRA_THREAD_SAFE=0 )
```
When enabled (the default), all internal mutexes use `std::mutex`, `std::recursive_mutex`, or `std::shared_mutex` depending on the type (`Trackable` and `EventLoop` use a plain mutex; the default `Event` alias uses a recursive mutex; `SharedEvent` uses a shared mutex for concurrent reads).  When disabled, they resolve to zero-overhead no-op stubs and `<mutex>` / `<shared_mutex>` are not included, allowing compilation on bare-metal targets where those headers are unavailable.  Affects `Event`, `Trackable`, `EventLoop`, `CombiningEvent`, and `ConnectionGuard` uniformly - it controls whether the whole library compiles with real synchronization primitives at all, not which per-event locking strategy is chosen (that is a separate, per-instance choice - see `BasicEvent<MutexType, Args...>` and its `Event`/`SharedEvent`/`SingleThreadedEvent` aliases).

### platform::Mutex / platform::SharedMutex
**Portable mutex types used internally**
```cpp
// resolves to std::mutex / std::shared_mutex when STELLYRA_THREAD_SAFE=1
// resolves to zero-size no-op structs when STELLYRA_THREAD_SAFE=0
stellyra::platform::Mutex m;
stellyra::platform::SharedMutex sm;
```
Defined in `platform.h`.  Follows the same pattern as Nova's `platform::Atomic`, `platform::Array`, etc.  Additional mutex types not used by the library but provided for variety/example purposes can be found in extras' `mutex_types.h`.

---

## Performance Features

### Minimal Dispatch Overhead
**Template-based design minimises dispatch overhead**
Handlers are stored as their concrete types via templates, avoiding `std::function` type erasure and virtual dispatch on the hot path.  The compiler can often inline handler calls in optimised builds.  Actual performance relative to other libraries depends on workload and build flags - see the benchmarks directory for measured comparisons.

### Lock-Free Operations
**Minimize contention**
Atomic operations where possible.  Efficient mutex usage only where needed.

---

## Thread Safety Features

### Thread-Safe Connection Management
**Connect/disconnect from any thread**
All connection operations are thread-safe.

### Automatic Serialization
**Sender thread affinity**
When sender has event loop, events automatically serialized to sender's thread.

### Cross-Thread Communication
**Safe message passing between threads**
Deferred connections enable safe async communication.

---

## Type Safety

### Compile-Time Checking
**Signature mismatches caught at compile time**
Template-based system ensures type safety.

### Clear Error Messages
**Helpful static asserts**
Readable error messages when types don't match.
