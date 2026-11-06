# Connection Thread Safety - Design Rationale

This document explains the thread safety model for `Connection` handles in Stellyra, the design alternatives that were considered, and the rationale for the decisions made.  For practical usage guidance, see `BEST_PRACTICES.md`.

---

## The Core Distinction

Stellyra has two distinct thread safety guarantees that must not be confused:

**Event operations are fully thread-safe:**
- `event.connect()` - any thread
- `event.disconnect( receiver )` - any thread
- `event( args )` / `event.trigger( args )` / `event.emit( args )` - any thread; genuinely concurrent dispatch is only guaranteed for `SharedEvent` (shared-locked, so concurrent triggers run in parallel against each other but block against connect/disconnect) and `ConcurrentEvent` (lock-free - dispatch never takes a lock at all); `Event`'s default `RecursiveMutex` serializes concurrent triggers rather than running them in parallel, and `SingleThreadedEvent` provides no synchronization at all and requires single-threaded use

**Connection is not thread-safe against concurrent reassignment:**
- `conn.disconnect()`, `conn.block()`, `conn.isConnected()`, etc. may be called concurrently from different threads on the same instance without restriction - each forwards into the shared, already-locked `EventImplBase`
- reassigning a `Connection` (`conn = event.connect(...)`, or clearing it) while another thread is calling any method on that same instance is a data race - assignment is the only thing that touches the handle’s own state

This distinction is intentional.  The library owns the connection state internally and protects it with a `shared_mutex`.  `Connection` is a lightweight handle - a `WeakPtr<EventImplBase>` plus a slot index and generation counter (24 bytes) - whose job is to give the caller a way to reference a specific connection for explicit control.

---

## The Real Misuse Pattern

### Storing Connection handles in shared containers

```cpp
std::vector< Connection > connections;

// thread A
connections[ 0 ].disconnect();

// thread B (concurrent - UNSAFE)
connections[ 0 ] = event.connect( ... );  // data race on same element
```

A `std::vector<Connection>` accessed from multiple threads requires external synchronisation on the *container access*, not just on the individual `Connection` operations.

**Resolution:** Don't store `Connection` handles when you don't need them - auto-disconnect via receiver lifetime handles the common case.  When explicit handle storage is required across threads, use `ConnectionGuard` or protect the container with a mutex.

---

## Design Alternatives Considered

### Making Connection move-only (copy-disabled)

**Considered but not adopted** - it would break common legitimate patterns: `std::vector<Connection>::push_back()` wanting copyable-or-movable elements (move-only works but requires explicit `std::move`), and ordinary reassignment like `conn = event.connect(...)`.

Disabling copy construction and copy assignment would prevent some accidental sharing patterns.  `Connection` is already effectively move-only in practice - copying a `Connection` gives two handles to the same underlying connection, which is rarely intentional.  The current design allows both copy and move.  The documentation and `ConnectionGuard` address the misuse cases without restricting the API.

### Making Connection non-assignable

**Considered but not adopted** - the restriction is too broad relative to the misuse it prevents: it would also block legitimate reassignment.

Deleting `operator=` would prevent `connections[i] = event.connect(...)` at compile time, but also prevent:

```cpp
Connection conn;
// ... later ...
conn = event.connect( receiver, slot );  // useful reassignment
```

### A fully thread-safe Connection type

**Considered but not adopted** - mutex overhead on every operation isn’t justified when the common case is a single `Connection` instance accessed by one thread at a time, even in an otherwise multithreaded program.

Wrapping every `Connection` operation in a mutex would make concurrent access safe at the cost of a mutex acquisition on every `isConnected()`, `block()`, etc. call.  The opt-in `ConnectionGuard` provides this for the likely-uncommon case where it is needed.

### Runtime detection in debug builds

**Considered but not adopted** - ineffective for the actual problem.

A `thread_local` last-caller check in `Connection::disconnect()` was considered to detect concurrent access in debug builds.  However, `thread_local` tracks the calling thread, not concurrent access - it would only detect the same thread calling `disconnect()` twice in sequence, not two different threads racing.

---

## The Adopted Solution: ConnectionGuard

`ConnectionGuard` is a mutex-protected wrapper around `Connection` for the cases where a connection handle genuinely needs to be shared across threads:

```cpp
// when you must share a connection handle across threads:
stellyra::ConnectionGuard guard( event.connect( receiver, slot ) );

// thread A - safe
guard.disconnect();

// thread B - safe
if ( guard.isConnected() ) { ... }
```

`ConnectionGuard` has mutex overhead and is intentionally not the default.  Most code should use one of the safer patterns that avoid storing handles at all:

```cpp
// preferred: auto-disconnect via receiver lifetime
event.connect( receiver, &Receiver::slot );
// no handle stored - disconnects automatically when receiver is destroyed

// preferred: disconnect by receiver (always thread-safe)
event.disconnect( receiver );

// preferred: RAII scoped lifetime
stellyra::ScopedConnection scoped( event.connect( receiver, slot ) );
```

---

## Summary of Decisions

| Decision | Choice | Rationale |
|---|---|---|
| **Connection copy semantics** | Allowed | Needed for container storage and ordinary reassignment |
| **Connection thread safety** | Not guaranteed | Overhead not justified for common single-threaded access case |
| **Thread-safe handle storage** | Opt-in via ConnectionGuard | Explicit rather than paying cost universally |
| **Thread-safe disconnect** | Always via event.disconnect(receiver) | No handle needed; works from any thread |
| **Debug-build detection** | Not implemented | thread_local approach was ineffective for the actual race |
