# Pulsar - Thread-Safe Event Library

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Header-Only](https://img.shields.io/badge/header--only-brightgreen.svg)]()
[![License: BSD-3](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE)

Pulsar is a modern C++17 header-only event library with type-safe events, automatic connection lifetime management, built-in thread safety, and asynchronous cross-thread dispatch with no external dependencies beyond the C++ standard library.

---

## Quick Start

### Installation

**Header-only** - copy or symlink the `libs/pulsar/include/` directory:

```bash
# core only
cp -r libs/pulsar/include/kmac /your/project/include/

# extras (optional)
cp -r libs/pulsar_extras/include/kmac /your/project/include/
```

**CMake:**

```cmake
add_subdirectory( pulsar )

# core only
target_link_libraries( your_target PRIVATE Pulsar::Pulsar )

# core + extras (ScopedConnection, CombiningEvent, EventInspector, RecordableEvent, etc.)
target_link_libraries( your_target PRIVATE Pulsar::Extras )
```

### Option 1: Inherit from Object (simplest)

```cpp
#include <kmac/pulsar.h>

namespace pulsar = kmac::pulsar;

class Button : public pulsar::Object
{
public:
	pulsar::Event< int, int > clicked{ this };

	void click( int x, int y ) { clicked( x, y ); }
};

class Handler : public pulsar::Object
{
public:
	void onClicked( int x, int y )
	{
		std::cout << "Clicked at (" << x << ", " << y << ")\n";
	}
};

auto button = std::make_shared< Button >();
auto handler = std::make_shared< Handler >();

button->clicked.connect( handler, &Handler::onClicked );
button->click( 10, 20 );  // prints: Clicked at (10, 20)

handler.reset();          // connection auto-disconnects
button->click( 30, 40 );  // safe - no crash, no call
```

### Option 2: Embed Object as member (non-invasive)

Classes that cannot or should not modify their inheritance hierarchy can embed `pulsar::Object` as a member instead.

**Sender** - embed `pulsar::Object` and pass its address to the Event constructor:

```cpp
class NetworkManager  // no inheritance required
{
	pulsar::Object _obj;
public:
	pulsar::Event< int > dataReceived { &_obj };

	void receive( int value ) { dataReceived( value ); }
};
```

**Receiver** - embed `std::shared_ptr<pulsar::Object>` as the lifetime anchor and pass it to `connect()` along with a lambda bridge:

```cpp
class ExistingClass  // cannot modify inheritance
{
	std::shared_ptr< pulsar::Object > _lifetime = std::make_shared< pulsar::Object >();
public:
	void onData( int value ) { /* ... */ }

	std::shared_ptr< pulsar::Object > pulsarObject() { return _lifetime; }
};

ExistingClass existing;
manager.dataReceived.connect(
	existing.pulsarObject(),
	[ &existing ]( int value ) { existing.onData( value ); } );
// connection severs automatically when _lifetime is destroyed
```

---

## Features

### Core Event System
- **Type-safe events** - compile-time signature checking; no runtime type errors
- **Multiple handlers** - any number of handlers per event, in any combination of member functions, lambdas, and free functions
- **Three trigger syntaxes** - `event(args)`, `event.trigger(args)`, `event.emit(args)` - all equivalent
- **Automatic lifetime management** - connections disconnect automatically when sender or receiver is destroyed; no dangling calls

### Connection Types
- **Direct** - handler executes synchronously in the sender's EventLoop drain context
- **Deferred** - handler is posted to the receiver's EventLoop and executes asynchronously
- **Auto** (default) - resolved at trigger-time: Direct if sender and receiver share an EventLoop, Deferred otherwise

### Advanced Connections
- **Single-shot** - `connectOnce()` auto-disconnects after the first invocation
- **Conditional** - `connectIf()` with an arbitrary predicate; handler skipped when condition is false
- **Priority ordering** - `connectWithPriority()` with integer priority; higher value executes first
- **Move-only captures** - lambda captures of `std::unique_ptr` and other move-only types are fully supported

### Thread Safety
- **Thread-safe by default** - all event, connection, and object operations are thread-safe
- **Sender thread affinity** - if a sender has an EventLoop and an emission occurs outside its drain context, the entire emission is automatically deferred to that loop
- **Cross-thread dispatch** - Deferred connections route handler execution to the receiver's EventLoop thread
- **Bare-metal / embedded** - set `PULSAR_THREAD_SAFE=0` to replace all mutexes with zero-overhead no-ops; set `PULSAR_ENABLE_THREAD=0` to remove `<thread>` dependency

### EventLoop
- **Auto-processed** - `EventLoop::makeAutoProcessed()` spawns a dedicated background thread
- **Manual-processed** - `EventLoop::makeManualProcessed()` + `processEvents()` driven by the caller's thread
- **Migration** - `setEventLoop()` moves an object between loops, pending events follow

### Connection Management
- **Connection handles** - `connect()` returns a `Connection` for manual `disconnect()`, `block()`, and `unblock()`
- **ScopedConnection** - RAII wrapper that disconnects when it goes out of scope (`extras`)
- **ConnectionGroup** - manages a set of connections across multiple events as a unit (`extras`)

### Encapsulation
- **PrivateEvent** - `PrivateEvent<Owner, Args...>` (`PEvent<Owner, Args...>` alias) allows external `connect()` but restricts triggering to the owner class; enforced at compile time via the type system

### Extended Features (`pulsar_extras.h`)
- **CombiningEvent** - collects return values from all handlers and combines them with a configurable combiner
- **10+ built-in combiners** - `LogicalAnd`, `LogicalOr`, `Sum`, `Product`, `Mean`, `Maximum`, `Minimum`, `CountTrue`, `First`, `FirstNonDefault`, `Last`, `LastNonDefault`
- **EventInspector** - runtime inspection of connection metadata, ASCII connection graph, and Graphviz DOT export (`toDotString()`)
- **RecordableEvent** - records emissions with timestamps; replay at original, adjusted, or variable speed; export to CSV; performance statistics

---

## Common Use Cases

- **GUI event handling** - button clicks, menu selections, property change notifications
- **Observer pattern** - decouple data models from their views
- **Producer-consumer** - worker threads posting results back to a UI or coordinator thread via Deferred connections
- **Event-driven state machines** - state enter/exit notifications with priority-ordered handlers
- **Validation pipelines** - `CombiningEvent` with `LogicalAnd` to require all validators to pass
- **Plugin / extension systems** - `PrivateEvent` exposes connection points without exposing trigger authority
- **Testing and debugging** - `RecordableEvent` records emission sequences for replay in unit tests or bug reproduction
- **Embedded / RTOS systems** - `PULSAR_THREAD_SAFE=0` + `PULSAR_ENABLE_THREAD=0` + manual EventLoop for bare-metal targets

---

## Feature Comparison

> For full details see [`docs/COMPREHENSIVE_LIBRARY_COMPARISON.md`](docs/COMPREHENSIVE_LIBRARY_COMPARISON.md).

| Feature | Pulsar | Qt | Boost.Signals2 | libsigc++ | nano-signal-slot | sigslot | vdk-signals | rocket | RxCPP | asio |
|---|---|---|---|---|---|---|---|---|---|---|
| **Header-only** | ✅ | ❌ | ✅ | ❌ | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ |
| **C++ standard** | 17 | 17 (Qt6) | 11 | 11 | 17 | 14 | 17 | 11 | 14 | 14 |
| **No preprocessor** | ✅ | ❌ MOC | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Thread-safe** | ✅ | ✅ | ✅ | ❌ | ⚠️ Policy | ✅ | ✅ | ⚠️ Policy | ✅ | ✅ |
| **Cross-thread dispatch** | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ⚠️ | ✅ | ✅ |
| **Event loops** | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ | ⚠️ | ✅ | ✅ |
| **Sender thread affinity** | ✅ Loop identity | ✅ OS thread | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ | N/A | N/A |
| **Auto connection type** | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ⚠️ | ❌ | N/A | N/A |
| **Single-shot** | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ? | ❌ | N/A | N/A |
| **Conditional connections** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ? | ❌ | ⚠️ | ❌ |
| **Priority ordering** | ✅ | ❌ | ✅ | ❌ | ❌ | ❌ | ? | ❌ | ❌ | ❌ |
| **Return value aggregation** | ✅ 11 combiners | ❌ | ✅ 2 built-in | ✅ custom | ❌ | ❌ | ? | ✅ | ✅ | ❌ |
| **Private event (encapsulation)** | ✅ Type-system | ✅ MOC convention | ❌ | ❌ | ❌ | ❌ | ? | ❌ | N/A | N/A |
| **Inheritance optional** | ✅ | ❌ QObject | ✅ | ✅ | ❌ | ✅ | ? | ❌ | ✅ | ✅ |
| **Bare-metal support** | ✅ | ❌ | ❌ | ❌ | ✅ | ✅ | ❌ | ? | ❌ | ⚠️ |
| **Inspection / debugging** | ✅ Full | ⚠️ Limited | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **Event recording / replay** | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| **License** | BSD-3 | LGPL/Commercial | Boost | LGPL | zlib | MIT | ? | Public domain | Apache-2 | Boost |

> **Note:** `?` indicates the library exists but detailed analysis has not yet been performed.
> RxCPP and asio use fundamentally different paradigms (reactive streams and async I/O
> respectively) and are included for reference only - they are not direct signal/slot competitors.

---

## When Not to Use Pulsar

- **Pure callback notification** - if an object just needs an `onChanged()` notifier with no threading or lifetime concerns, lighter libraries (Boost.Signals2, sigslot, nano-signal-slot) are simpler choices
- **Async I/O frameworks** - Pulsar does not replace Asio, libuv, or io_uring; it sits above them and coordinates the stateful objects that process I/O results
- **Deep bare-metal microcontrollers** - Cortex-M targets with minimal RAM are better served by static-array dispatchers; Pulsar's bare-metal mode targets soft real-time Linux-based embedded systems

See [`docs/USE_CASES.md`](docs/USE_CASES.md) for a detailed breakdown of strong fits,
reasonable fits with caveats, and antipatterns.

---

## Benchmarks

> Formal benchmarks have not yet been conducted.  Results will be published here once available.

The benchmark suite (see `benchmarks/` once available) will measure:

- single-threaded direct emission - 1, 10, 100, 1000 connections
- cross-thread deferred emission
- connect/disconnect throughput
- concurrent emission from multiple threads
- sender deferral overhead vs no-loop baseline

Each library is benchmarked in a separate binary to avoid cross-library contamination of allocator state and cache behaviour.

---

## Documentation

| Document | Contents |
|---|---|
| [`docs/FAQ.md`](docs/FAQ.md) | Frequently asked questions |
| [`docs/FEATURES.md`](docs/FEATURES.md) | Full feature reference with code examples |
| [`docs/BEST_PRACTICES.md`](docs/BEST_PRACTICES.md) | Usage patterns and common mistakes |
| [`docs/MIGRATION.md`](docs/MIGRATION.md) | Migrating from Qt, Boost, libsigc++, nano, sigslot |
| [`docs/COMPREHENSIVE_LIBRARY_COMPARISON.md`](docs/COMPREHENSIVE_LIBRARY_COMPARISON.md) | Detailed feature comparison across libraries |
| [`docs/CONNECTION_THREAD_SAFETY.md`](docs/CONNECTION_THREAD_SAFETY.md) | Thread safety design rationale for Connection handles |
| [`docs/USE_CASES.md`](docs/USE_CASES.md) | Architectural use cases and where Pulsar is not the right tool |

---

## Requirements

- C++17 or later
- No external dependencies
- Tested: GCC 7+, Clang 5+, MSVC 2017+, Apple Clang 10+

---

## License

BSD 3-Clause - see [LICENSE](LICENSE) for details.
