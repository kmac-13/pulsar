# Stellyra Architecture

This document describes how Stellyra is built today.  It is not a design history or a migration guide - it describes the current source, file by file, so that anyone changing the library (or reviewing it, including for SOUP purposes) can find where a given behavior lives.

Stellyra is header-only, C++17, and split into two targets:

- **`stellyra`** (`libs/stellyra/include/kmac/stellyra/`) - the core library: events, connections, dispatch, lifetime tracking, event loops
- **`stellyra_extras`** (`libs/stellyra_extras/include/kmac/stellyra/`) - optional, separately-included features built on top of the core: a lock-free concurrent event, return-value-combining events, reactive properties, recording/replay, introspection, and a couple of alternative mutex types; nothing in `stellyra_extras` is required to use the core library, and nothing in the core library depends on `stellyra_extras`

## Events: `BasicEvent<MutexType, Args...>`

The public event type is `BasicEvent<MutexType, Args...>` (`basic_event.h`), aliased as:

- `Event<Args...>` - `platform::RecursiveMutex` (the default)
- `SharedEvent<Args...>` - `platform::SharedMutex` (concurrent dispatch, exclusive connect/disconnect)
- `SingleThreadedEvent<Args...>` - `platform::NullMutex` (zero locking overhead; caller guarantees single-threaded use)

The mutex strategy is a template parameter, so it is chosen per event instance, not via a single library-wide compile-time flag - a binary can mix `Event`, `SharedEvent`, and `SingleThreadedEvent` members freely.

`BasicEvent` itself is thin: it inherits connection management from `EventStorage` (below) and adds `operator()` / `trigger()` / `emit()` (all identical) to dispatch to connected handlers.

`PrivateEvent<FriendType, Args...>` / `PEvent` (`private_event.h`) wraps `BasicEvent` with private inheritance plus `friend FriendType`, so only `FriendType` can trigger it while every connect/disconnect/block method stays public.  `BasicRecordableEvent` / `RecordableEvent` (in `stellyra_extras`, see below) is the same idea with recording and
statistics bolted on.

### Dispatch deferral (sender side)

`BasicEvent::operator()` checks whether the event's owner (a `Trackable*`) has an `EventLoop`.  If it does, and the calling thread is not that loop's designated thread (via `EventLoop::shouldDispatchDirectlyOnThread()`), the *entire* dispatch - not just individual connections - is posted to that loop and runs whenever that loop is next drained.  Otherwise, dispatch happens synchronously on the calling thread.  This check is cheap when there is no
loop at all (the common case): one pointer read, no capture, no lock.

## Connections and storage

### `EventStorage<ImplT, MutexType, Args...>` (`event_storage.h`)

The base class every event type inherits for its connect/disconnect/block surface.  It is parameterized on `ImplT` (the actual storage/dispatch engine - `BasicEventImpl` for `BasicEvent`, `BasicConcurrentEventImpl` for
`BasicConcurrentEvent`, etc.) so the same connect-side API works across multiple backing implementations.

Connections are grouped into three named buckets, each with NTTP (compile-time) and runtime-pointer overloads:

- **`connect`** - object/method connections (member function pointer, bound to a receiver object)
- **`connectFree`** - free-function connections (including non-capturing lambdas)
- **`connectLambda`** - functor/lambda connections, either a raw functor (wrapped internally) or a pre-built `Callable`

Each bucket accepts an optional `Tracked<T>{ receiver, tracker }` for a receiver that does not itself inherit `Trackable` (see below), and an optional `ConnParams` for priority/predicate/single-shot (see below).  `forwardTo()` connects one event directly to another with matching signature.  `disconnect()` / `disconnectFree()` remove by receiver, by method/function identity, or via a `Trackable&`.  `block()` / `unblock()` /
`blockGuard()` suspend dispatch for the whole event.

### Stable-slot storage: `HandlerEntry<Args...>` (`handler_entry.h`)

Each connection occupies one slot in a `std::vector<HandlerEntry<Args...>>` owned by `BasicEventImpl` - the impl behind `Event`, `SharedEvent`, and `SingleThreadedEvent`.  `BasicConcurrentEventImpl` (backing `ConcurrentEvent`) uses a different, atomically-swapped `Entry`/`SlotArray` design instead; see the `BasicConcurrentEvent` / `ConcurrentEvent` section below.  A disconnected slot becomes an inactive hole, tracked by a free list, and is reused by a later `connect()` rather than the vector being compacted.  `HandlerEntry` is 48 bytes on 64-bit and holds the handler `Callable`, a `Trackable` migration tag, `EventFlags` (see below), declared+resolved `ConnectionTypes`, a priority, a generation counter, and the connection's `EventLoop*` (for Deferred dispatch).

Because slots never move, a `Connection` handle (next) can refer to one directly by index - there is no separate heap-allocated "connection object" distinct from the `HandlerEntry` itself (beyond whatever the `Callable` it holds needs - see below).

### `Connection` (`connection.h`)

A `Connection` is a 24-byte value: `WeakPtr<EventImplBase>` + a `uint32_t` slot index + a `uint16_t` generation. `isConnected()` resolves the weak pointer and compares the stored generation against the slot's current one - each time a slot is freed, its generation is bumped, so a `Connection` to a disconnected-and-reused slot reads as not connected rather than addressing the wrong handler.  `GenData` (`gen_data.h`) is the same (index, generation) pair used internally for bulk operations.

`ScopedConnection` wraps a `Connection` and disconnects on destruction (move-only).  `BlockGuard` is a type-erased RAII block guard (blocks on construction, unblocks on destruction) - deliberately not templated, so it can be returned from `blockGuard()` regardless of the event's `Args...`.

### `Callable<ReturnType(Args...)>` (`callable.h`)

The type-erased callable stored in every `HandlerEntry` (and used as the predicate type too).  A flat `(void* obj, stub function pointer, optional destructor function pointer)` triple - no virtual dispatch, and no heap allocation except when wrapping an owning functor (`create(F&&)`, typically a capturing lambda), which is heap-allocated via `new` and freed in `Callable`'s destructor or on overwrite. Free functions and pointer-to-member-functions are non-owning and allocation-free.  `createPartial<Method>(obj)` / `createPartial<Func>()` support connecting a handler that takes *fewer* arguments than the event provides - the trailing arguments are dropped at the call site, at zero runtime cost (`MethodArity`/`FunctionArity` resolve this at compile time in `event_detail.h`).  This is how partial-arity method/free-function connections work; lambdas are expected to just omit trailing parameters themselves.

### `ConnParams<MutexType, Args...>` (`conn_params.h`)

The single bundle for a connection's optional settings: priority (`.prio()`), a conditional predicate (constructor or `.when()`, with a `PredicateContext` of `Sender` or `Receiver`), and single-shot (`.once()`).  This approach helps avoid `connect` overload explosion, incluing having separate `connectIf()`/`connectOnce()`/`connectWithPriority()` entry points - any combination is one `ConnParams` value passed to the normal `connect()`/`connectFree()`/`connectLambda()` overloads.

Priority is a simple integer, with higher-priority connections being evaluated before lower-priority connections.  Note that Deferred connections with higher priority are still deferred and potentially executed later than Direct connections with lower priority.

`.once()` is a `static_assert` failure on `SharedEvent` (a single-shot handler disconnecting itself while a shared lock is held would deadlock); the assertion only fires when `.once()` is actually called, not merely when a `ConnParams` of that `MutexType` is instantiated.

`PredicateContext::Sender` evaluates the predicate at emission time, before a Deferred connection's task is even posted - always synchronously inside `trigger()`.  `PredicateContext::Receiver` (the default) means the predicate is evaluated immediately before the handler runs - synchronously inside `trigger()` for a Direct connection, or on the receiver's loop at drain time for a Deferred one.  

### Connection type resolution (`connection_type.h`, `event_detail.h`)

A connection declares `ConnectionType::Direct`, `Deferred`, or `Auto` (default) at connect time.  This resolves to a `ResolvedConnectionType` of `Direct`, `Deferred`, or `None` (topology doesn't support either - the connection stays live but is silently skipped) via `detail::resolveConnectionType()`:

- `Direct` always resolves `Direct`
- `Deferred` resolves `Deferred` if the receiver has an `EventLoop`, else `None`
- `Auto` resolves `Direct` if sender and receiver loops match (including both loops being null), `Deferred` if the receiver has a non-null loop and the sender has a loop different from receiver (including a null one), else `None`

Resolution is cached in the `HandlerEntry` at connect time and re-run whenever either side's loop changes via `Trackable::setEventLoop()`.

## Dispatch (`basic_event_impl.h`)

`BasicEventImpl<MutexType, Args...>` is the storage/dispatch engine behind `BasicEvent`.  It owns the `handlers` vector, a free list (`nextFree`), an optional priority-ordering index (`order`, built and maintained only while
at least one non-default-priority connection exists - the common all-default-priority case pays nothing for this), and whole-event block depth.

`dispatch(Args...)` has two paths:

- **Fast path**, used when every connection is Direct with no predicates (`nonDirectCount == 0 && predicateCount == 0`): a single lock, a straight walk over active slots, no branching per connection
- **Slow path**, used otherwise: per-handler predicate evaluation (in the right context), resolved-type dispatch (Direct now, or post to the receiver’s loop for Deferred), and single-shot cleanup immediately after that handler returns — a same-thread call into `disconnectHandler()`, which takes the exclusive lock - this is exactly why `.once()` is rejected on `SharedEvent` (see `ConnParams` above): dispatch there holds only a shared lock, so that immediate disconnect call would deadlock against itself

Argument capture for Deferred posting is lazy: the `shared_ptr<tuple<...>>` used to carry arguments across the loop boundary is only allocated once the first Deferred connection is actually encountered during a given dispatch (`ensureCapture()`), not unconditionally.

**Reentrancy**: a handler may legitimately connect to, disconnect from, or re-trigger the *same* event it is being called from (this is why `Event`'s default mutex is recursive).  A `triggerDepth` counter plus a `hadReentrantActivity` flag defer slot-reuse and order-rebuild bookkeeping while a reentrant call is in progress, reconciling once the outermost dispatch call unwinds (`reconcileAfterTrigger()`).  This is **not** supported on `SharedEvent` - reentering while holding a shared lock can deadlock.

## Lifetime tracking: `Trackable` (`trackable.h`)

`Trackable` replaces inheriting-and-nothing-else: a class derives from it to get (a) automatic disconnection of every connection where it is the receiver, when it is destroyed, and (b) an `EventLoop` association (`setEventLoop()`)
that both re-resolves its own events' sender-side topology and every tracked receiver-side connection.

For a receiver that cannot or should not inherit `Trackable`, `Tracked<T>` (in `event_storage.h`) is a two-field aggregate - `{ T& receiver, Trackable& tracker }` - passed to any `connect`/`connectFree`/`connectLambda` overload in place of a bare receiver reference, so a separate, explicitly-owned `Trackable` (aliased as `Anchor`) does the tracking instead.  This is the one non-inheriting receiver pattern in the current design.

Every `Trackable` has one `migrationTag`, shared by every connection where it is the receiver, so when it moves to a different `EventLoop`, all of its pending Deferred tasks migrate together in a single call (`EventLoop::migratePendingTo()`), regardless of which event or connection produced each one.

## `EventLoop` and `AutoDrainThread` (`event_loop.h`, `auto_drain_thread.h`)

`EventLoop` is a manually-drained task queue (`Callable<void()>` tasks).  `post()` enqueues; `drain()` processes everything queued, including tasks posted during the drain itself.  `shouldDispatchDirectlyOnThread()` is the gate `BasicEvent::operator()` (and friends) use to decide whether to dispatch now or defer to the loop - true if the calling thread is the loop's registered drain thread, or is, right now, the thread actively draining it.  `AutoDrainThread` is an optional helper that spawns a dedicated background thread which sleeps until `post()` wakes it, drains, and sleeps again - no polling.

Note that `EventLoop` supports a single listener to be notified when a task has been posted to the loop.  This is how `AutoDrainThread` gets notified in order to drain the loop, and is how using code can implement auto-draining without the use of `AutoDrainThread`.

## `platform.h`: thread-safety abstraction

All mutex and atomic types used throughout the library go through `kmac::stellyra::platform` aliases (`Mutex`, `RecursiveMutex`, `SharedMutex`, `Atomic<T>`, `SharedPtr<T>`, `WeakPtr<T>`, `ThreadId`).  When `STELLYRA_THREAD_SAFE=0` (opt-in, for bare-metal/single-core targets), these resolve to zero-overhead no-op stubs and `<mutex>`/`<shared_mutex>`/ `<atomic>` are not even included; `platform::Atomic<T>` falls back to a `volatile`-based implementation (single-core safe only, with a documented hook - `STELLYRA_CUSTOM_ATOMIC` - for supplying a real multi-core atomic on such a target).  `NullMutex` (used by `SingleThreadedEvent`) is always a no-op regardless of this flag.

## `stellyra_extras`

### `BasicConcurrentEvent` / `ConcurrentEvent` (`basic_concurrent_event*.h`)

A second, lock-free primary event type.  Each slot is a `std::atomic<Entry*>`, reclaimed through `EpochDomain` (below) rather than protected by a lock - `dispatch()` never takes the event's `mutex` at all; that mutex guards only connect/disconnect/priority-rebuild.  This gives `BasicConcurrentEvent` genuinely concurrent, non-serialized dispatch across threads, at the cost of being only *eventually consistent* (not linearizable) against a concurrent `updateSenderLoop()`/`updateHandlerLoop()` call - a documented, deliberate trade for lock-free dispatch.  Supports the same Direct/Deferred/Auto connection model, predicates, priority, and single-shot as `BasicEvent`; does not support reentrant dispatch the way `Event`'s recursive mutex does (there is no mutex held during dispatch to reenter in the first place, but the slot array can still be mutated concurrently with an in-flight dispatch by design - see the file's own documentation for the consistency model).

### `EpochDomain` (`epoch_domain.h`)

A small epoch-based-reclamation (EBR) utility backing `BasicConcurrentEvent`: readers announce "active" for the duration of a whole `dispatch()` call (not per-object), and a writer's `retire()` defers freeing until no reader could still be inside a critical section that started before the retired object was removed.  Reader-slot pool size is fixed at compile time via `STELLYRA_MAX_CONCURRENT_TRIGGERS` (default 32); exceeding it fails loudly via STELLYRA_ASSERT_ALWAYS (never compiled out by NDEBUG) rather than corrupting anything.

### `CombiningEvent<Combiner, ReturnType, Args...>` (`combining_event.h`, `combiners.h`)

Fires all connected handlers synchronously and reduces their return values through a `Combiner` functor (`Combiners::LogicalAnd`, `LogicalOr`, `Sum`, `Product`, `Mean`/`Average`, `Maximum`, `Minimum`, `CountTrue`, `First`, `FirstNonDefault`, `Last`, `LastNonDefault`).  Always Direct - there is no Deferred variant, since the combined result must be available synchronously - and its mutex is non-recursive, so reentrant connect/disconnect/trigger from inside a handler is not supported.

### Properties (`property.h`)

Property types:

- `Property<T>`: read/write by anyone
- `ReadOnlyProperty<Owner, T>`: read externally, write only by `Owner`
- `ComputedProperty<Owner, T>`: derived via an `Owner` member function
- `ComputedPropertyFn<Owner, T>`: derived via a capturing lambda, using one `Callable<T()>`
- `ConstProperty<T>`: immutable; `changed` exists for API uniformity but never fires

All expose the same read surface (implicit conversion, `get()`, a `changed` event) so calling code doesn't need to know which variant it's reading.

### Introspection: `EventInspector<MutexType, Args...>` (`event_inspector.h`, `inspection_info.h`)

Read-only, lock-respecting inspection of a live event's connection table - per-connection snapshots (`ConnectionInfo`), aggregate stats (`EventInfo`), and human-readable or Graphviz DOT dumps (`toDot()`/`toDotString()`, renderable with `dot -Tpng` or any DOT viewer).  Works on `PrivateEvent` too (declared a friend specifically so it can inspect past the private trigger methods).  Per-connection receiver address/type is not available - only "method" vs. "function/lambda" (`EventFlags::hasOwner()`), since `Callable` does not expose its bound object pointer generically.

### Recording and replay: `EventRecorder<Args...>` / `BasicRecordableEvent` (`event_recorder.h`, `basic_recordable_event.h`)

`EventRecorder` captures a bounded or unbounded sequence of emissions (arguments + timestamps) with start/pause/resume/stop, replay (immediate, original-timing, or speed-scaled), range replay, timing statistics, and CSV export.  `BasicRecordableEvent` / `RecordableEvent` wires a recorder (and optional cumulative emission-time statistics) around the same dispatch path as `BasicEvent`, wiring the recorder's replay target back through its own `operator()`.  Both are opt-in and add memory/per-emission overhead relative to plain `Event` - intended for development/debugging/testing/simulation, not as the default production event type.

### `ConnectionGroup` / `ConnectionGuard` (`connection_group.h`, `connection_guard.h`)

`ConnectionGroup` owns a `std::vector<Connection>` so a set of connections can be handled as a unit (e.g. when swapping a model) without destroying the owning object; move-only, `+=` to add.  `ConnectionGuard` wraps a single `Connection` behind a mutex for cases where the handle itself (not just the underlying connection) is shared and mutated across threads, serializing `isConnected()`, `disconnect()`, and `block()`/`unblock()`/`isBlocked()` — the same surface `Connection` itself has, made safe for concurrent access to one shared handle.

### Optional mutex types (`mutex_types.h`)

`FastRecursiveMutex` and `SpinRecursiveMutex`: experimental, not used by any shipped `Event` alias, provided for benchmarking/comparison against `std::recursive_mutex` on a given target.  Both use the same atomic-owner + depth-counter reentrancy design; `SpinRecursiveMutex` spins (bounded-then-yield) instead of blocking on an inner `std::mutex`.

## Other core headers

- `event_flags.h` - `EventFlags` (active/predicate/single-shot/owner/blocked bits) and `ConnectionTypes` (declared + resolved type), both packed into one byte each, underlying `HandlerEntry`
- `event_impl_base.h` - `EventImplBase`, the type-erased base every event's impl object inherits, giving `Connection`/`Trackable` a common interface independent of `Args...`, plus a globally unique per-instance `id`
- `private_event.h` - see above
- `stellyra_fwd.h` - forward declarations only, for headers that need type names without full definitions
- `config.h` - logging tags and the optional Nova logging integration (`STELLYRA_USE_NOVA`), plus the `STELLYRA_LOG_CONNECTION_ISSUES` compile flag for connection-misconfiguration warnings (off by default, zero
  overhead when off)
- `version.h` - compile-time version macros/constants
- `stellyra_assert.h` - `STELLYRA_ASSERT_ALWAYS`, a hard assertion that is never compiled out by `NDEBUG` (unlike plain `assert`), for conditions indicating genuine API misuse with no well-defined way to continue
