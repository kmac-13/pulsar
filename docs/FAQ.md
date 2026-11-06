# Stellyra FAQ

---

### Why use an event library at all, and why Stellyra?

General benefits of an event/observer-style design, independent of threading model or dispatch type:

- **Loose coupling** - components communicate through events with no compile-time knowledge of each other
- **One-to-many broadcast** - a single emission reaches every connected receiver without the sender enumerating or knowing how many there are
- **Selective subscription** - a receiver only needs to care about the specific notifications it's interested in, rather than implementing an entire mandatory callback interface
- **Runtime flexibility** - handlers can be connected, disconnected, or swapped at runtime (e.g. for plugins or state-dependent behavior) without recompiling the producer
- **Decoupled testing** - a collaborator can be swapped for a test double by connecting a different handler, instead of modifying the production call site
- **Less boilerplate** - connecting a handler is a single call, rather than defining a dedicated listener interface, a registration method, and manual list management by hand

**A trade-off worth understanding, independent of which library you use:** loose coupling and direct, traceable calls pull in opposite directions - neither is a free win.  Events are naturally suited to *announcing* that something happened, to whoever happens to be listening; direct calls are naturally suited to *requesting* an immediate result from a specific, known collaborator.  Leaning on events for the latter turns execution flow into a maze of indirect calls that's hard to trace.  But leaning on direct calls where the real relationship should be a decoupled one produces the opposite failure: a tightly-coupled web of forced interdependencies that's just as hard to reason about - entanglement instead of indirection.  Which tool is right depends on the proper design of the code, not on a default to prefer.

What's specific to Stellyra:

- **Automatic receiver-lifetime safety** - `Trackable`/`Tracked<T>` disconnect every tracked connection before a receiver is destroyed, removing the dangling-callback bug class common to hand-rolled observer code
- **Per-instance thread-safety strategy** - the mutex strategy (`Event`/`SharedEvent`/`SingleThreadedEvent`/`ConcurrentEvent`) is a template parameter chosen per event instance, not a single library-wide compile flag, so one binary can mix all of them
- **Built-in deferred dispatch** - the `EventLoop`-aware `Direct`/`Deferred`/`Auto` connection model handles cross-thread marshalling without an external queue or hand-written posting code; a receiver with its own `EventLoop` has every `Auto`/`Deferred`-resolved handler call serialized onto that loop's drain thread, no matter how many senders or threads connect to it, which is the same guarantee Qt's queued connections give a receiver's thread affinity, so the receiver's own handler code needs no internal mutex to protect itself against concurrent calls (an explicitly `Direct`-declared connection from a sender outside that loop is the one exception - see "How Stellyra's, Qt's, and vdk-signals' thread models differ?" below.)
- **Bare-metal / zero-overhead mode** - `STELLYRA_THREAD_SAFE=0` strips all mutex/atomic overhead for single-core embedded targets without changing the API
- **Introspection** - `EventInspector` gives read-only, lock-respecting connection-table snapshots and Graphviz DOT dumps of a live event's wiring, for debugging connection graphs that have grown too tangled to trace by reading source

**Trade-offs to weigh against this:**

- Dispatch has overhead versus a direct call - Stellyra's fast dispatch path and `STELLYRA_THREAD_SAFE=0` minimize this, but it's never zero-cost
- Execution flow is less linear than an explicit call chain - harder to trace who runs in response to what - but that indirection is the necessary cost of genuine asynchronous dispatch, not a flaw to engineer away, and it’s the same indirection that discourages the forced, tightly-coupled interdependencies described above
- Subscription lifetime has to be managed - `Trackable`/`Tracked<T>` handle the common case automatically (see "Do I need to inherit from Trackable?" below), but the responsibility doesn't disappear entirely

The rest of this FAQ covers what's specific to Stellyra's own design - thread safety, lifetime tracking, deferred execution, and so on.

---

### Do I need to inherit from Trackable?

No - not for senders or receivers.

**Senders** can either inherit from `stellyra::Trackable` and pass this to the `Event` constructor (simplest), or pass the address of a `stellyra::Trackable` (or `stellyra::Anchor`) obtained some other way - a member of the sender or an instance supplied to the sender's constructor.

**Receivers** can either inherit from `stellyra::Trackable` (simplest) or pair a non-inheriting receiver with a standalone `stellyra::Trackable`/`Anchor` via `Tracked<T>{ receiver, tracker }`, passed to `connect`/`connectFree`/`connectLambda` in place of a bare receiver.  In both cases, the receiver must remain alive for as long as any event it's connected to might still be dispatching into it.  That can be arranged two ways: let the tracker's destruction handle it automatically (destroying the tracker runs `Trackable::~Trackable()`, which disconnects every connection that tracker was holding, so no further dispatch can reach the receiver afterward), or disconnect explicitly beforehand - via a stored `Connection`'s `disconnect()`/`ScopedConnection` or `event.disconnect( receiver )` - so there's nothing left to dispatch through by the time the receiver is destroyed.  Neither approach protects against a dispatch already in progress when destruction begins; avoiding that overlap is the caller's ordinary responsibility.  For an inheriting receiver, the tracker mechanism runs automatically as part of the receiver's own destruction.  For the non-inheriting `Tracked<T>` pattern, the standalone tracker and the receiver are two separate objects the caller must sequence deliberately - explicit disconnection removes that sequencing concern entirely.

See `trackable.h` and `BEST_PRACTICES.md` for full examples of both patterns.

---

### Can I use Stellyra in a single-threaded application?

Yes - and single-threaded deferred execution is a first-class use case, not just a supported edge case.  A manual `EventLoop` drained by the same thread that emits events gives you the architectural benefits of deferred execution without any threading complexity:

- **State machine safety** - handlers that emit further events do not re-enter the emitter; recursive transitions are prevented naturally
- **Reentrancy resistance** - deferred processing breaks recursive call chains that can cause subtle bugs in synchronous listener systems
- **Isolation from slow handlers** - the emitter returns immediately after posting to the queue; handler execution happens on the next drain
- **Burst absorption** - the queue smooths spikes without stalling the emitter

These four are specific to single-threaded deferred execution; they're in addition to the general event-driven-design benefits already covered under "Why use an event library at all, and why Stellyra?" above.

If you need even less overhead, set `STELLYRA_THREAD_SAFE=0` to replace all internal mutexes with zero-overhead no-ops.

If you don't need an `EventLoop` at all - no deferred execution, just immediate direct dispatch - you can use `Event` with no `EventLoop` assigned.  Handlers execute synchronously on the calling thread with no queue overhead.

---

### Can multiple threads emit the same event concurrently?

It depends on whether the sender has an EventLoop:

**Without an EventLoop** - emissions execute immediately on each calling thread.  The connection list snapshot is protected by a shared lock so multiple threads can emit concurrently, but if the sender has non-thread-safe state that handlers access, you are responsible for protecting it.

**With an EventLoop** - any emission from outside that loop's drain context is automatically deferred to the loop.  All emissions are then serialised to the loop's drain thread, protecting sender state without requiring external mutexes.

---

### Does SharedEvent or ConcurrentEvent still dispatch concurrently if it has an owner EventLoop?

No - once an owner `EventLoop` is assigned, both lose the thing that distinguishes them from a plain `Event`.

`BasicEvent::operator()` and `BasicConcurrentEvent::operator()` run the identical sender-loop check, and it's independent of `MutexType`: any trigger from a thread that isn't eligible to dispatch directly on the owner's loop has its entire trigger posted to that loop and runs later during `drain()`.  `EventLoop::drain()` guards itself with a CAS so only one thread can ever be draining a given loop at a time.  Put together, `dispatch()` is never called by more than one thread at a time once an owner loop exists - regardless of whether the event is `Event`, `SharedEvent`, `ConcurrentEvent`, etc.

- `SharedEvent`'s `SharedLock`-based concurrent dispatch becomes uncontended by construction - there's no second thread left to share the lock with; `connect()`/`disconnect()` still take the exclusive lock and still block against whatever's dispatching, same as for `Event`
- `ConcurrentEvent`'s lock-free `EpochDomain`/atomic-slot design goes unexercised for its actual purpose - there's nothing left to arbitrate without a second concurrent dispatching thread

Both types remain exactly what they are, API-wise, and still dispatch correctly from the loop's own drain thread - this isn't a bug, just a consequence of the sender-side deferral being independent of `MutexType`.  If genuinely concurrent dispatch from multiple threads is what you actually want, don't assign the event's owner an `EventLoop` - let triggers run directly on the calling thread instead.

---

### Can connections be made from any thread?

Yes.  All connection management operations (`connect`, `disconnect`, `block`, `unblock`) are fully thread-safe.

---

### What happens if a receiver is destroyed while an emission is in progress?

It depends on what "receiver" means for that connection - specifically, whether it has an associated Trackable:

**Receiver inherits Trackable** - nothing bad.  Destroying it runs `Trackable::~Trackable()` (via `disconnectAll()`), which marks every connection it was tracking as inactive on the sender's event, under that event's own mutex; `dispatch()` checks that same slot state under the same lock before invoking anything.  If the receiver has been destroyed, the slot reads as inactive and the invocation is silently skipped.  This guarantee is unconditional for a tracked receiver - it holds regardless of which thread destroys it.

**Receiver is paired with a standalone `Trackable`/`Anchor` via `Tracked<T>`** - same guarantee, by the same mechanism, as long as the standalone tracker is destroyed no later than the receiver (see "Do I need to inherit from Trackable?" above).  Destroying the external tracker is what runs `disconnectAll()`; the receiver itself doesn't need to do anything.

**Receiver has no associated `Trackable` at all** - e.g. connected via `connect( receiver, &SomeClass::method )` where SomeClass doesn't inherit `Trackable` and no `Tracked<T>` pairing was used.  `connect()`'s object/method overloads silently permit this: if T doesn't inherit `Trackable`, the internal tracker pointer is simply left nullptr and the connection is created fully untracked, with no diagnostic at connect time.  There is no protection in this case - the connection stays active with no way to detect the receiver's destruction, and triggering the event afterward dispatches into freed memory.  Always ensure a receiver passed by bare reference actually inherits `Trackable`, or use `Tracked<T>` explicitly.

---

### What happens if I trigger an Event with a Deferred connection and the receiver has no EventLoop?

The invocation is silently dropped.  Enable `STELLYRA_LOG_CONNECTION_ISSUES` to receive a warning when this occurs.

---

### How do I migrate from another event library?

See [`docs/MIGRATION.md`](MIGRATION.md) for a step-by-step guide on migrating to Stellyra from various libraries.

---

### What is the difference between stellyra.h and stellyra_extras.h?

`<kmac/stellyra.h>` provides the core event system: `Event`/`SharedEvent`/`SingleThreadedEvent`, `PrivateEvent`, `Trackable`, `EventLoop`, `Connection`/`ScopedConnection`/`BlockGuard`, and compile-time configuration.

`<kmac/stellyra_extras.h>` includes everything in `stellyra.h` plus extended functionality: `ConnectionGuard`, `ConnectionGroup`, `CombiningEvent` plus combiners, `ConcurrentEvent`, `EventInspector`, `RecordableEvent`, `Property`/`ReadOnlyProperty`/`ComputedProperty`/`ConstProperty`, and the optional `FastRecursiveMutex`/`SpinRecursiveMutex` mutex types.

Include `stellyra_extras.h` during development and debugging or if `ConcurrentEvent` is needed.  For production code that only needs the core event dispatch mechanism, `stellyra.h` is sufficient.

---

### What is the difference between Auto, Direct, and Deferred connections?

- **Auto (default)** - resolved at connect time and cached, re-resolved only when the sender's or receiver's `EventLoop` changes via `Trackable::setEventLoop()` (never resolved at trigger-time).  If sender and receiver share the same `EventLoop` (or neither has one), the connection behaves as `Direct`.  If they are on different `EventLoops`, it behaves as `Deferred`.
- **Direct** - handler executes synchronously on whatever thread is running the emission.  If the sender has an `EventLoop`, emissions from outside that loop are deferred to it first, so the handler ultimately runs on the sender's loop drain thread.  If the sender has no `EventLoop`, the handler runs immediately on the calling thread.
- **Deferred** - handler is posted to the receiver's `EventLoop` queue and executes asynchronously when that loop is next drained.

---

### How Stellyra's, Qt's, and vdk-signals' thread models differ?

Qt compares the **emitting thread** against the receiver's thread affinity.  For `QueuedConnection`, or `AutoConnection` where the threads differ, this is fine as Qt always defers the **slot** handling to the receiver's thread.  For `DirectConnection`, however, the slot is executed on the **emitting thread**, even if that thread differs from the signal object's thread affinity - which can be a problem if the slot object expects thread-safe behaviour by virtue of sharing the same thread affinity as the signal object.

Stellyra, on the other hand, compares the **sender's EventLoop** against the current drain context.  If the event is not being processed in the sender's loop, Stellyra defers the **entire emission** to that loop.  For a `Deferred` connection, or an `Auto` connection where the sender and receiver `EventLoop`s differ, Stellyra always defers the receiver's handler to the receiver's loop.  For a `Direct` connection, since emission is always processed in the sender's loop, the receiver's handler executes in that same loop - meaning receivers that share the sender's `EventLoop` can safely assume thread-consistent handler execution without additional synchronisation.

vdk-signals compares the emitting thread directly against the `context`'s home thread - the same per-connection, thread-vs-thread comparison Qt makes, rather than the loop-object comparison Stellyra makes.  If no execution type is given, a `context`-bound slot invoked from the same thread that constructed its `context` runs synchronously; otherwise the call is transferred into the `context`'s thread and invoked asynchronously.  Explicit `exec::sync` and `exec::async` overrides are available too, playing the same role as Qt's `DirectConnection`/`QueuedConnection`.

The structural difference from Stellyra is where thread affinity actually lives.  A vdk-signals `context`'s home is fixed to whichever OS thread constructed it, via an implicit, `thread_local` channel allocated on first use - there is exactly one such channel per OS thread, and `signals_execute()` (the free-function equivalent of `EventLoop::drain()`) always drains the calling thread's own channel, with no separate object to construct or name.  Stellyra's `EventLoop` is decoupled from OS-thread identity entirely - it's a value you construct and assign explicitly, so multiple independent `EventLoop`s can coexist on a single OS thread and be drained separately, the pattern neither vdk-signals nor Qt can express, since both tie affinity to the thread itself rather than to an independent, explicitly-managed loop object.
