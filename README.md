# Stellyra - Thread-Safe Event Library

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Header-Only](https://img.shields.io/badge/header--only-brightgreen.svg)]()
[![License: BSD-3](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)

Stellyra is a modern C++17 header-only event library with type-safe events, automatic connection lifetime management, built-in thread safety, and asynchronous cross-thread dispatch with no external dependencies beyond the C++ standard library.

---

## Quick Start

### Installation

**Header-only** - copy or symlink the `libs/stellyra/include/` directory:

```bash
# core only
cp -r libs/stellyra/include/kmac /your/project/include/

# extras (optional)
cp -r libs/stellyra_extras/include/kmac /your/project/include/
```

**CMake:**

```cmake
add_subdirectory( stellyra )

# core only
target_link_libraries( your_target PRIVATE Stellyra::Stellyra )

# core + extras (CombiningEvent, EventInspector, RecordableEvent, etc.)
target_link_libraries( your_target PRIVATE Stellyra::Extras )
```

### Option 1: Inherit from Trackable (simplest)

```cpp
#include <kmac/stellyra.h>

namespace stellyra = kmac::stellyra;

class Button : public stellyra::Trackable
{
public:
	stellyra::Event< int, int > clicked{ this };

	void click( int x, int y ) { clicked( x, y ); }
};

class Handler : public stellyra::Trackable
{
public:
	void onClicked( int x, int y )
	{
		std::cout << "Clicked at (" << x << ", " << y << ")\n";
	}
};

Button button;
auto handler = std::make_unique< Handler >();

button.clicked.connect( *handler, &Handler::onClicked );
button.click( 10, 20 );  // prints: Clicked at (10, 20)

handler.reset();         // connection auto-disconnects
button.click( 30, 40 );  // safe - no crash, no call
```

### Option 2: Embed Trackable as member (non-invasive)

Classes that cannot or should not modify their inheritance hierarchy can embed `stellyra::Trackable` (aliased as `stellyra::Anchor`) as a member instead.

**Sender** - embed `stellyra::Trackable` and pass its address to the Event constructor:

```cpp
class NetworkManager  // no inheritance required
{
	stellyra::Trackable _obj;
public:
	stellyra::Event< int > dataReceived { &_obj };

	void receive( int value ) { dataReceived( value ); }
};
```

**Receiver** - embed a `stellyra::Anchor` as the lifetime tracker and pass it to `connect()` paired with the receiver via `Tracked<T>`:

```cpp
class ExistingClass : public ExistingBaseClass  // cannot modify inheritance
{
	stellyra::Anchor _lifetime;
public:
	void onData( int value ) { /* ... */ }

	stellyra::Anchor& stellyraAnchor() { return _lifetime; }
};

ExistingClass existing;
manager.dataReceived.connect(
	stellyra::Tracked{ existing, existing.stellyraAnchor() },
	&ExistingClass::onData );
// connection severs automatically when existing (and its Anchor) is destroyed
```

### Option 3: External ownership (receiver type can't be touched at all)

Some receivers can't be touched at all - third-party library types, generated code, anything outside your control.  Unlike Option 2, this doesn't require the receiver to expose so much as an accessor: the `Trackable` lives entirely outside the receiver, owned by whichever code performs the `connect()` call.

```cpp
class SetupClass : public stellyra::Trackable  // or embeds its own stellyra::Anchor (Trackable)
{
	stellyra::Trackable _lifetime;  // if SetupClass doesn't itself inherit Trackable
public:
	void init( ThirdPartyLibClass* libInst, EventClass* events )
	{
		// ThirdPartyLibClass is a type defined in a third-party library that can't be modified
		// EventClass contains at least one event
		events->valueChanged.connect( { *libInst, _lifetime }, &ThirdPartyLibClass::setValue );
	}
};
```

**This ties the connection's safety to `SetupClass`'s lifetime, not `ThirdPartyLibClass`'s.**  The connection auto-disconnects when `_lifetime` (i.e. `SetupClass`) is destroyed - not when `libInst` is.  If `libInst` can be destroyed or become invalid while `SetupClass` is still alive, the caller is responsible for disconnecting first; Stellyra has no way to observe `ThirdPartyLibClass`'s lifetime since `ThirdPartyLibClass` was never given a tracker of its own.  Use this pattern only when the registering code's lifetime is known to bound (or exceed) the receiver's.

---

## Features

### Core Event System
- **Type-safe events** - compile-time signature checking; no runtime type errors
- **Multiple handlers** - any number of handlers per event, in any combination of member functions, lambdas, and free functions
- **Per-instance thread-safety strategy** - choose the mutex strategy per event via a type alias: `Event` (recursive mutex), `SharedEvent` (shared mutex, concurrent dispatch), or `SingleThreadedEvent` (zero-overhead no-op mutex)
- **Pluggable mutex type** - those three aliases cover the common cases, but `BasicEvent<MutexType, Args...>` accepts any type satisfying the standard `Lockable`/`SharedLockable` requirements, so a custom mutex can be supplied directly - `stellyra_extras/mutex_types.h`'s experimental `FastRecursiveMutex`/`SpinRecursiveMutex` are exactly this, provided for benchmarking a given toolchain against `std::recursive_mutex`
- **Three trigger syntaxes** - `event(args)`, `event.trigger(args)`, `event.emit(args)` - all equivalent
- **Automatic lifetime management** - connections disconnect automatically when sender or receiver is destroyed; no dangling calls
- **Partial argument matching** - a member function handler can omit trailing arguments it doesn't need; trailing args are silently dropped at compile time with no runtime cost

### Connection Types
- **Direct** - handler executes synchronously in the event's trigger context
- **Deferred** - handler is posted to the receiver's `EventLoop` for asynchronous execution
- **Auto** (default) - resolved at connection-time and upon change of receiver's `EventLoop`: `Direct` if sender and receiver share an `EventLoop`, `Deferred` otherwise

### Advanced Connections
- **Single-shot** - `.once()` on `ConnParams` (passed to `connect()`/`connectFree()`/`connectLambda()`) auto-disconnects after the first invocation
- **Conditional** - a predicate on `ConnParams` (constructor or `.when()`) with an arbitrary predicate; handler skipped when condition is false
- **Priority ordering** - an integer priority value on `ConnParams` (constructor or `.prio()`); higher values execute before lower values
- **Move-only captures** - lambda captures of `std::unique_ptr` and other move-only types are fully supported

### Thread Safety
- **Thread-safe by default** - all event, connection, and trackable operations are thread-safe
- **Sender thread affinity** - if a sender has an `EventLoop` and an emission occurs outside its drain context, the entire emission is automatically deferred to that loop
- **Cross-thread dispatch** - a handler never runs on the wrong thread by accident: using a `Deferred` (or `Auto`-resolved) connection guarantees execution happens on the receiver’s own `EventLoop` thread, not whatever thread the sender happened to trigger from
- **Bare-metal / embedded** - set `STELLYRA_THREAD_SAFE=0` to replace all mutexes with zero-overhead no-ops

### EventLoop
- **Manual** - `EventLoop` + `drain()` driven by the caller's thread
- **Automatic** - `AutoDrainThread` wraps an `EventLoop` with a dedicated background thread that wakes and drains whenever work is posted
- **Migration** - `setEventLoop()` moves an object between loops, pending events follow

### Connection Management
- **Connection handles** - `connect()` returns a `Connection` for manual `disconnect()`, `block()`, and `unblock()`
- **ScopedConnection** - RAII wrapper that disconnects when it goes out of scope

### Encapsulation
- **PrivateEvent** - `PrivateEvent<Owner, Args...>` (`PEvent<Owner, Args...>` alias) allows external `connect()` but restricts triggering to the owner class; enforced at compile time via the type system

### Extended Features (`stellyra_extras.h`)
- **ConnectionGroup** - manages a set of connections across multiple events as a unit
- **ConcurrentEvent** - a lock-free, `EpochDomain`-backed alternative to `Event`/`SharedEvent`, letting multiple threads trigger genuinely in parallel with no shared or exclusive lock in the dispatch path at all
- **CombiningEvent** - collects return values from all handlers and combines them with a configurable combiner
- **10+ built-in combiners** - `LogicalAnd`, `LogicalOr`, `Sum`, `Product`, `Mean` (`Average`), `Maximum`, `Minimum`, `CountTrue`, `First`, `FirstNonDefault`, `Last`, `LastNonDefault`
- **EventInspector** - runtime inspection of connection metadata, ASCII connection graph, and Graphviz DOT export (`toDotString()`)
- **RecordableEvent** - records emissions with timestamps; replay at original, adjusted, or variable speed; export to CSV; performance statistics
- **Properties** - `Property<T>` (read/write by anyone), `ReadOnlyProperty<Owner, T>` (read externally, write only by Owner), `ComputedProperty<Owner, T>` / `ComputedPropertyFn<Owner, T>` (derived from a member function or lambda), and `ConstProperty<T>`  (immutable) - all expose the same read surface (implicit conversion, get(), a changed event) so calling code doesn’t need to know which variant it’s holding

---

## Common Use Cases

- **GUI event handling** - button clicks, menu selections, property change notifications
- **Observer pattern** - decouple data models from their views
- **Producer-consumer** - worker threads posting results back to a UI or coordinator thread via Deferred connections
- **Event-driven state machines** - state enter/exit notifications with priority-ordered handlers
- **Validation pipelines** - `CombiningEvent` with `LogicalAnd` to require all validators to pass
- **Plugin / extension systems** - `PrivateEvent` exposes connection points without exposing trigger authority
- **Testing and debugging** - `RecordableEvent` records emission sequences for replay in unit tests or bug reproduction
- **Embedded / RTOS systems** - `STELLYRA_THREAD_SAFE=0` + manual `EventLoop` for bare-metal targets

---

## Comparing Stellyra to Other Libraries

This README is deliberately scoped to what Stellyra itself provides.  For how it compares against other C++ event/signal libraries, and when one of them is the better fit, see [`docs/CHOOSING_A_LIBRARY.md`](docs/CHOOSING_A_LIBRARY.md).

---

## When Not to Use Stellyra

- **Pure callback notification** - if an object just needs an onChanged() notifier with no threading or lifetime concerns, a library with a smaller synchronous-only API - sigslot, nano-signal-slot, or Boost.Signals2’s core signal/slot mechanism (setting aside the size of Boost as a whole) - is a simpler choice
- **Async I/O frameworks** - libraries such as Asio, libuv, and io_uring solve a different problem: they’re I/O reactors that wait on sockets, timers, file descriptors, etc, at the OS level and tell your code when an I/O operation has completed; Stellyra doesn’t support any of that, and isn’t a substitute for it, but Stellyra can be used alongside these types of libraries by having an I/O completion handler trigger a Stellyra event that the rest of the application can handle synchronously or asynchronously - complementary, not competing libraries
- **Deep bare-metal microcontrollers** - Cortex-M targets with minimal RAM are better served by static-array dispatchers; Stellyra’s bare-metal mode (`STELLYRA_THREAD_SAFE=0`) is built for exactly this kind of target - it compiles without `<mutex>`/`<atomic>` - but its `std::vector<HandlerEntry>`-based connection storage still allocates on the heap, which is the actual reason Cortex-M-class targets are better served by static-array dispatchers

See [`docs/USE_CASES.md`](docs/USE_CASES.md) for a detailed breakdown of strong fits, reasonable fits with caveats, and antipatterns.

---

## Benchmarks

A full benchmark suite exists and has been run against every library in the comparison table above (Qt, Boost.Signals2, libsigc++, nano-signal-slot, sigslot, vdk-signals, rocket, nod).  It measures:

- single-threaded direct emission - 1, 10, 100, 1000 connections
- cross-thread deferred emission
- connect/disconnect throughput
- concurrent emission from multiple threads, at increasing contention
- scoped (RAII) receiver lifetime teardown
- sender deferral overhead vs no-loop baseline

Each library is benchmarked in separate binaries to avoid cross-library contamination of allocator state and cache behaviour.  See [`benchmarks/SCENARIOS.md`](benchmarks/SCENARIOS.md) for what each scenario measures.  `benchmarks/BENCHMARK_RESULTS.md` is not committed - results are only meaningful for the machine that produced them - so run the suite yourself (see [`benchmarks/RUNNING.md`](benchmarks/RUNNING.md)) to generate current numbers.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How the library is put together - events, connections, dispatch, lifetime tracking, EventLoop, and the `stellyra_extras` components |
| [`docs/FAQ.md`](docs/FAQ.md) | Frequently asked questions |
| [`docs/FEATURES.md`](docs/FEATURES.md) | Full feature reference with code examples |
| [`docs/BEST_PRACTICES.md`](docs/BEST_PRACTICES.md) | Usage patterns and common mistakes |
| [`docs/MIGRATION.md`](docs/MIGRATION.md) | Migrating from Qt, Boost, libsigc++, nano, sigslot |
| [`docs/NAMING.md`](docs/NAMING.md) | Naming conventions used throughout the library |
| [`docs/CHOOSING_A_LIBRARY.md`](docs/CHOOSING_A_LIBRARY.md) | When each alternative library is a better fit than Stellyra, and vice versa |
| [`docs/CONNECTION_THREAD_SAFETY.md`](docs/CONNECTION_THREAD_SAFETY.md) | Thread safety design rationale for Connection handles |
| [`docs/USE_CASES.md`](docs/USE_CASES.md) | Architectural use cases and where Stellyra is not the right tool |
| [`docs/USE_CASE_DIFFERENTIAL_MACHINE_CONTROL.md`](docs/USE_CASE_DIFFERENTIAL_MACHINE_CONTROL.md) | Worked use case: a differential/machine-control event pipeline |
| [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) | Current limitations, toolchain findings, and verified correctness properties |
| [`docs/TODO.md`](docs/TODO.md) | Open work and features under consideration |

---

## Requirements

- C++17 or later
- No external dependencies
- Should support (not independently verified in this project - TODO(CI): confirm once CI is wired up): GCC 7+, Clang 5+, MSVC 2017+, Apple Clang

---

## License

BSD 3-Clause - see [LICENSE](LICENSE) for details.
