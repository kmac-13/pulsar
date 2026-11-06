# Stellyra Best Practices Guide

## Overview

Stellyra is designed to make event-driven programming safe and efficient.  This guide shows you how to use the library correctly and avoid common pitfalls.

## Table of Contents

1. [Basic Usage Patterns](#basic-usage-patterns)
2. [Connection Management](#connection-management)
3. [Thread Safety](#thread-safety)
4. [Performance Tips](#performance-tips)
5. [Common Mistakes](#common-mistakes)
6. [Multi-Threaded Examples](#multi-threaded-examples)

---

## Basic Usage Patterns

### Pattern 1: Auto-Disconnect (Recommended for Most Cases)

**The simplest and most common pattern:**

```cpp
class Button : public stellyra::Trackable {
public:
	stellyra::Event< int, int > clicked { this };
};

class Handler : public stellyra::Trackable {
public:
	void onClicked( int x, int y ) {
		std::cout << "Clicked at: " << x << ", " << y << "\n";
	}
};

int main() {
	auto button = std::make_unique< Button >();
	auto handler = std::make_unique< Handler >();

	// just connect - no need to store Connection
	button->clicked.connect( *handler, &Handler::onClicked );

	button->clicked( 10, 20 );  // calls handler

	// when handler is destroyed, connection auto-disconnects
	handler.reset();

	button->clicked( 30, 40 );  // safe - no crash
}
```

**When to use:** 
- ✅ default choice for most scenarios
- ✅ when connection lifetime matches object lifetime
- ✅ when you don't need explicit disconnect control

**Benefits:**
- simplest code
- automatic cleanup
- no memory to manage
- thread-safe by design

---

### Pattern 2: Scoped Connections (For Explicit Lifetime Control)

**When you need connections to disconnect at specific scope:**

```cpp
void processData() {
	auto button = std::make_shared< Button >();
	auto handler = std::make_shared< Handler >();

	// create scoped connection
	stellyra::ScopedConnection scoped( button->clicked.connect( *handler, &Handler::onClicked ) );

	button->clicked( 1, 1 );  // handler called

	// auto-disconnects when leaving scope
}
```

**When to use:**
- ✅ temporary connections within a function
- ✅ conditional connection lifetime
- ✅ RAII-style resource management

**Example: Conditional Connection**
```cpp
private:
	stellyra::ScopedConnection _connection;

void Widget::setEnabled( bool enabled ) {
	if ( enabled ) {
		_connection = stellyra::ScopedConnection( _button->clicked.connect( *this, &Widget::onClicked ) );
	}
	else {
		_connection = {};  // disconnect by resetting
	}
}
```

---

### Pattern 3: Connection Groups (Multiple Related Connections)

**When you have many connections to manage together:**

```cpp
class Widget : public stellyra::Trackable {
private:
	stellyra::ConnectionGroup _connections;

public:
	Widget( std::shared_ptr< Button > button ) {
		// add multiple connections to group
		_connections += button->clicked.connect( *this, &Widget::onClick );
		_connections += button->pressed.connect( *this, &Widget::onPress );
		_connections += button->released.connect( *this, &Widget::onRelease );
	}

	void disable() {
		_connections.blockAll();
	}

	void enable() {
		_connections.unblockAll();
	}

	~Widget() {
		// all connections auto-disconnect
	}

private:
	void onClick( int x, int y ) { /* ... */ }
	void onPress( int x, int y ) { /* ... */ }
	void onRelease( int x, int y ) { /* ... */ }
};
```

**When to use:**
- ✅ multiple connections to manage as a unit
- ✅ need to block/unblock together
- ✅ want bulk disconnection

---

## Connection Management

### ✅ Recommended: Disconnect by Receiver

**The thread-safe way to disconnect:**

```cpp
// multiple threads can safely do this
event.disconnect( receiver );
```

This is the **preferred method** for multi-threaded disconnection because:
- thread-safe
- no Connection handle needed
- disconnects all connections to that receiver
- clear intent

### ✅ Acceptable: Store Connection in Single Thread

```cpp
class SetupClass {
private:
	stellyra::Connection _conn;  // only accessed from one thread

public:
	void setupConnection() {
		_conn = _event.connect( receiver, slot );
	}

	void teardownConnection() {
		_conn.disconnect();
	}
};
```

### ⚠️ Careful: Store in Shared Container

**If you must store connections accessed by multiple threads:**

```cpp
class MultiThreadedManager {
private:
	std::mutex _mutex;
	std::vector< stellyra::ConnectionGuard > _connections;

public:
	void addConnection( stellyra::Connection conn ) {
		std::lock_guard< std::mutex > lock( _mutex );
		// wrap in ConnectionGuard for thread safety
		_connections.emplace_back( std::move( conn ) );
	}

	void disconnectAll() {
		std::lock_guard< std::mutex > lock( _mutex );
		for ( auto& guard : _connections ) {
			guard.disconnect();
		}
	}
};
```

**Better alternative:** Don't store connections at all, use receivers:

```cpp
class MultiThreadedManager {
private:
	stellyra::Event< int > _event { this };
	std::vector< std::shared_ptr< Handler > > _handlers;

public:
	void addHandler( std::shared_ptr< Handler > handler ) {
		_event.connect( *handler, &Handler::process );
		_handlers.push_back( handler );
	}

	void removeHandler( std::shared_ptr< Handler > handler ) {
		// thread-safe disconnect
		_event.disconnect( *handler );

		auto it = std::find( _handlers.begin(), _handlers.end(), handler );
		if ( it != _handlers.end() ) {
			_handlers.erase( it );
		}
	}
};
```

---

## Thread Safety

### What IS Thread-Safe

✅ **Event dispatch** - Multiple threads can trigger concurrently
```cpp
// thread A
event.trigger( 1 );

// thread B (concurrent - SAFE)
event.trigger( 2 );
```

✅ **Connection creation** - Multiple threads can connect concurrently
```cpp
// thread A
event.connect( receiver1, handler1 );

// thread B (concurrent - SAFE)
event.connect( receiver2, handler2 );
```

✅ **Disconnect by receiver** - Multiple threads can disconnect concurrently
```cpp
// thread A
event.disconnect( receiver1 );

// thread B (concurrent - SAFE)
event.disconnect( receiver2 );
```

✅ **Connection methods** - Each method call is thread-safe
```cpp
Connection conn = event.connect( ... );

// thread A
if ( conn.isConnected() ) { ... }  // SAFE

// thread B
conn.block();  // SAFE
```

### What is NOT Thread-Safe

❌ **Connection object assignment**
```cpp
Connection conn;

// thread A
conn = event.connect( receiver1, handler1 );

// thread B (concurrent - UNSAFE!)
conn = event.connect( receiver2, handler2 );  // RACE CONDITION
```

### Thread Safety Rules

1. **Connection operations** (`disconnect()`, `isConnected()`, `block()`, `unblock()`) are individually thread-safe, even called concurrently on the same `Connection`
2. **Assigning a new value into the same `Connection` variable** from multiple threads is *not* thread-safe - that's a genuine data race on the handle's own storage, not on anything it points to
3. **Event operations** are fully thread-safe
4. **When in doubt**, use `event.disconnect(receiver)` instead of Connection handles

---

## Performance Tips

### Tip 1: Let Auto Resolve Connection Type (Default)

```cpp
// Auto (the default) - Direct when sender and receiver share an EventLoop,
// Deferred otherwise; re-resolves automatically if either side's loop
// changes later via setEventLoop()
event.connect( receiver, handler );

// forces synchronous dispatch unconditionally - even across loops, and
// even if the topology changes later; use only when you specifically need
// to override the receiver's thread affinity and know that's safe
event.connect( receiver, handler, stellyra::ConnectionType::Direct );

// forces deferral unconditionally - even when sender and receiver share a
// loop; use to break reentrancy/recursion or guarantee async delivery
event.connect( receiver, handler, stellyra::ConnectionType::Deferred );
```

`Auto` is the right default in nearly all cases - it gives you `Direct`’s zero-overhead dispatch whenever it’s actually safe (same loop) and falls back to `Deferred` automatically otherwise, without you having to track loop topology by hand or re-check it after a `setEventLoop()` call.  Use explicit `Direct` / `Deferred` only when you need to force one regardless of topology.

### Tip 2: Don't Store Connections You Don't Need

```cpp
// ❌ Unnecessary storage
std::vector< stellyra::Connection > connections;
for ( auto& receiver : receivers ) {
	connections.push_back( event.connect( receiver, &Receiver::handler ) );
}

// ✅ Just connect
for ( auto& receiver : receivers ) {
	event.connect( receiver, &Receiver::handler );
}
```

### Tip 3: Use ConnectionGroup for Bulk Operations

```cpp
// Verbose, but no different in cost
for ( auto& conn : connections ) {
	conn.disconnect();
}

// Same work, one line - and you can't forget a connection or leave the
// vector non-empty afterward
group.disconnectAll();
```

`ConnectionGroup::disconnectAll()` does the same per-connection loop internally - it isn’t a batched or single-lock operation like `EventStorage::disconnectAll()` (the whole-event version) is.  The win is encapsulation, not throughput.

### Tip 4: Avoid Rapid Connect/Disconnect Cycles

```cpp
// ❌ Inefficient
while ( running ) {
	auto conn = event.connect( receiver, handler );
	processEvent();
	conn.disconnect();
}

// ✅ Better: connect once, use blocking
auto conn = event.connect( receiver, handler );
while ( running ) {
	if ( shouldProcess ) {
		conn.unblock();
		processEvent();
		conn.block();
	}
}

// ✅ Best: use conditional connection
// conditional connections don't require maintaining a handle to the connection -
// just supply the predicate, which requires exact-arity matching against the event's 
// Args... (no partial-argument-matching path for predicates, ignore the payload here)
event.connect( receiver, handler, event.params( []( int ) { return shouldProcess; } ) );
```

---

## Predicate Timing for Conditional Connections

A predicate supplied via `ConnParams` (`event.params( pred )`, or `event.params( pred, context )`) bundles the condition with the handler.  Two independent things can go wrong with it: a captured reference outliving its scope (or, the opposite problem, a captured value outliving what it should), and the predicate reading stale state relative to when it's evaluated. These are unrelated hazards - fixing one doesn't fix the other.

### Hazard 1: Capture lifetime vs. ownership

The predicate is stored for the entire lifetime of the connection, not just for one call, so how you capture state into it matters:

- **By reference** - risks a dangling reference if the captured *variable* goes out of scope independently of anything the connection tracks.  This has nothing to do with `Receiver`'s own lifetime or `Trackable` - it's a plain local with no lifetime tracking at all:

```cpp
void setupConnection( std::shared_ptr< Receiver > receiver )
{
	bool bufferFull = checkBuffer();  // plain local, nothing tracks this

	// ❌ Unsafe: captures a local by reference, which dangles the moment this
	// function returns, regardless of what happens to `receiver` - Trackable
	// has nothing to do with `bufferFull`'s lifetime
	event.connect( *receiver, &Receiver::process,
		event.params( [ &bufferFull ]( int ) { return ! bufferFull; } ) );
}
```

- **By value (a `shared_ptr`)** - avoids the dangling-reference problem, but trades it for the opposite one: the captured copy is an *owning* reference held for as long as the connection exists.  If every other owner releases the object, it no longer gets destroyed on schedule - the connection's own captured copy keeps it alive, which can silently extend an object's lifetime well past when the rest of the program expects it gone.

- **By value (a `weak_ptr`)** - avoids both.  It's a value, not a reference, so it's independent of whatever scope the original variable lived in; and it doesn't participate in the strong refcount, so it doesn't keep the object alive either.  The cost is a null/expired check inside the predicate:

```cpp
// ✅ Recommended: doesn't dangle (captured by value) and doesn't extend
// lifetime (weak_ptr doesn't contribute to the refcount)
std::weak_ptr< Receiver > weakReceiver = receiver;
event.connect( *receiver, &Receiver::process,
	stellyra::ConnectionType::Deferred,
	event.params( [ weakReceiver ]( int ) {
		auto r = weakReceiver.lock();
		return r && ! r->bufferFull();
	} ) );
```

Capturing a `shared_ptr` by reference or by value can both be fine in narrower cases (by reference, if the variable provably outlives the connection; by value, if extending the object's lifetime for the connection's duration is actually the intended ownership model) - but `weak_ptr` is the one option that's safe by default without having to reason about either.

### Hazard 2: Context/staleness mismatch (Deferred connections only)

`PredicateContext` controls *when* the predicate runs:

- **`Receiver`** (the default) - evaluated at `trigger()` time for `Direct` connections, or re-evaluated at drain time, immediately before the handler runs, for `Deferred` connections.
- **`Sender`** - always evaluated inside `trigger()`, before the task is even posted.

For `Direct` connections there's no gap either way - `trigger()` and the handler run at the same instant on the same thread.  For `Deferred` connections, staleness can show up if the predicate's context isn't in sync with the context with which the predicate is executed:

- `Sender` context inspecting **receiver** state → stale, since receiver state may change between emission and whenever the handler eventually runs.
- `Receiver` context inspecting **sender** state → also stale, for the mirror reason: evaluated at dequeue, which is later than emission, so sender-side state captured then could already be outdated.
- Either context inspecting its *own* side's state is fine - no gap, because evaluation happens at exactly the moment that side's state is current.

```cpp
// ❌ Stale: Sender-context predicate inspecting RECEIVER state on a Deferred
// connection - evaluated at trigger() time, before the task is posted, so
// this can be out of date by the time the handler actually runs later.
event.connect( receiver, &Receiver::process,
	stellyra::ConnectionType::Deferred,
	event.params( [ weakReceiver ]( int ) {
		auto r = weakReceiver.lock();
		return r && ! r->bufferFull();
	}, stellyra::PredicateContext::Sender ) );

// ✅ Fix: use Receiver context (the default) for receiver-side state - it's
// re-evaluated at dequeue time, immediately before the handler runs, so it
// sees state exactly as current as the handler itself would.
event.connect( receiver, &Receiver::process,
	stellyra::ConnectionType::Deferred,
	event.params( [ weakReceiver ]( int ) {
		auto r = weakReceiver.lock();
		return r && ! r->bufferFull();
	}, stellyra::PredicateContext::Receiver ) );
```

The rule of thumb: prefer capturing a `weak_ptr` by value (hazard 1), and match `PredicateContext` to whichever side's state the predicate actually reads (hazard 2).

---

## Common Mistakes

### Mistake 1: Storing Temporary Receivers

```cpp
// ❌ WRONG: temporary receiver
void setupConnection() {
	auto receiver = std::make_shared< Receiver >();
	event.connect( *receiver, &Receiver::handler );
	// receiver destroyed here - connection invalid!
}
```

**Fix: Keep receiver alive**
```cpp
// ✅ CORRECT
class Manager {
private:
	std::shared_ptr< Receiver > _receiver;  // keep alive

public:
	void setupConnection() {
		_receiver = std::make_shared< Receiver >();
		event.connect( *_receiver, &Receiver::handler );
	}
};
```

### Mistake 2: Concurrent Connection Vector Access

```cpp
// ❌ WRONG: race condition
std::vector< stellyra::Connection > connections;

// thread A
connections[ 0 ].disconnect();

// thread B
connections[ 0 ] = event.connect( ... );  // RACE!
```

**Fix: Use proper synchronization or avoid storing**
```cpp
// ✅ Option 1: don't store connections
event.connect( receiver1, handler1 );
event.connect( receiver2, handler2 );
// ... later ...
event.disconnect( receiver1 );

// ✅ Option 2: use ConnectionGuard
std::vector< stellyra::ConnectionGuard > connections;

// ✅ Option 3: explicit protection with mutex (same protection provided by ConnectionGuard)
std::mutex _mutex;
std::vector< stellyra::Connection > connections;

void disconnect( size_t index ) {
	std::lock_guard< std::mutex > lock( _mutex );
	connections[ index ].disconnect();
}
```

### Mistake 3: Forgetting EventLoop for Deferred Connections

```cpp
// ❌ WRONG: receiver has no event loop
auto receiver = std::make_unique< Receiver >();
event.connect( *receiver, handler, stellyra::ConnectionType::Deferred );
// handler never called - no event loop!
```

**Fix: Set EventLoop**
```cpp
// ✅ CORRECT
stellyra::EventLoop loop;
stellyra::AutoDrainThread drainer( loop );  // drains loop automatically on a background thread

auto receiver = std::make_unique< Receiver >();
receiver->setEventLoop( &loop );

event.connect( *receiver, handler, stellyra::ConnectionType::Deferred );
// works - invocation is queued to the receiver's loop!
```

### Mistake 4: Passing Connection by Reference Across Threads

```cpp
// ❌ WRONG
void workerThread( stellyra::Connection& conn ) {
	conn.disconnect();  // races with other threads
}
```

**Fix: Use event.disconnect(receiver) or move Connection**
```cpp
// ✅ Option 1: use receiver
void workerThread( std::shared_ptr< Receiver > receiver ) {
	event.disconnect( handler );  // thread-safe
}

// ✅ Option 2: pass by value - this thread gets its own Connection
// instance (copied or moved in), so no other thread can race on it.
// Calling disconnect() on a copy is safe even if another copy of the
// same logical connection exists elsewhere.
void workerThread( stellyra::Connection conn ) {
	conn.disconnect();
}
```

---

## Multi-Threaded Examples

### Example 1: Producer-Consumer

```cpp
class Producer : public stellyra::Trackable {
public:
	stellyra::Event< std::string > dataReady { this };

	void produce( const std::string& data ) {
		dataReady( data );
	}
};

class Consumer : public stellyra::Trackable {
public:
	void consume( std::string data ) {
		// process in consumer's thread
		std::cout << "Processing: " << data << "\n";
	}
};

int main() {
	auto producer = std::make_unique< Producer >();
	auto consumer = std::make_unique< Consumer >();

	// consumer has its own event loop
	stellyra::EventLoop loop;
	stellyra::AutoDrainThread drainer( loop );
	consumer->setEventLoop( &loop );

	// auto connection - will be Deferred
	producer->dataReady.connect( *consumer, &Consumer::consume );

	// produce from main thread
	producer->produce( "Hello" );
	producer->produce( "World" );

	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	drainer.stop();  // unnecessary since AutoDrainThread calls stop on destruction
}
```

### Example 2: Thread-Safe Connection Management

```cpp
class ConnectionManager : public stellyra::Trackable {
private:
	std::mutex _mutex;
	std::vector< std::shared_ptr< Receiver > > _receivers;

public:
	stellyra::Event< int > event { this };

	void addHandler( std::shared_ptr< Receiver > receiver ) {
		// thread-safe connect
		event.connect( *receiver, &Receiver::process );

		std::lock_guard< std::mutex > lock( _mutex );
		_receivers.push_back( receiver );
	}

	void removeHandler( std::shared_ptr< Receiver > receiver ) {
		// thread-safe disconnect
		event.disconnect( *receiver );

		std::lock_guard< std::mutex > lock( _mutex );
		auto it = std::find( _receivers.begin(), _receivers.end(), receiver );
		if ( it != _receivers .end() ) {
			_receivers.erase( it );
		}
	}

	void emit( int value ) {
		// thread-safe emit
		event( value );
	}
};
```

### Example 3: Concurrent Emissions

```cpp
class Sender : public stellyra::Trackable {
public:
	stellyra::Event< int > dataReady { this };
};

class Receiver : public stellyra::Trackable {
public:
	void process( int value ) { /* ... */ }
};

// Note: workerThread takes the Sender itself, not a copy of its Event -
// BasicEvent (what Event<int> aliases) deletes its copy constructor, so
// std::make_shared<stellyra::Event<int>>(sender->dataReady) does not compile
void workerThread( std::shared_ptr< Sender > sender, int id ) {
	for ( int i = 0; i < 100; ++i ) {
		// safe: concurrent emissions on the same, shared Event
		sender->dataReady( id * 100 + i );
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
	}
}

int main() {
	auto sender = std::make_shared< Sender >();
	auto receiver = std::make_shared< Receiver >();

	sender->dataReady.connect( *receiver, &Receiver::process );

	// multiple threads triggering concurrently
	std::vector< std::thread > threads;
	for ( int i = 0; i < 4; ++i ) {
		threads.emplace_back( workerThread, sender, i );
	}

	for ( auto& t : threads ) {
		t.join();
	}
}
```

---

## Summary

### Golden Rules

1. **Prefer auto-disconnect** - don't store Connection handles unless you need them
2. **Use disconnect(receiver)** - thread-safe and simpler than Connection handles
3. **Connection objects are not thread-safe** - don't access from multiple threads
4. **Event operations are thread-safe** - emit and connect freely from any thread
5. **Keep receivers alive** - connection needs both sender and receiver to exist
6. **Use ScopedConnection for RAII** - automatic cleanup at scope exit
7. **Use ConnectionGroup for bulk management** - simpler than managing individual connections

### Quick Reference

| Pattern | Use When | Thread-Safe? |
|---------|----------|--------------|
| Auto-disconnect | Default choice | ✅ |
| ScopedConnection | Scoped lifetime | ✅ |
| ConnectionGroup | Multiple connections | ✅ |
| event.disconnect(receiver) | Multi-threaded | ✅ |
| Connection stored locally | Single thread | ✅ |
| Connection in shared container | Never (use ConnectionGuard) | ❌ |

**When in doubt: Don't store Connection handles, let auto-disconnect work!**
