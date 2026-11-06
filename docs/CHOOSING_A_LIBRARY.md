# Choosing Stellyra vs. Other Event/Signal Libraries

This document summarizes when each alternative library is a better fit than Stellyra, and vice versa.  See `BENCHMARK_RESULTS.md` for current benchmark numbers and `compare.py`'s Feature Checklist for a side-by-side capability table - this document covers qualitative tradeoffs rather than specific figures, since those go stale as soon as benchmarks are re-run.

Stellyra's unique features aren't repeated in every per-library section below; each section links to the full list at the end instead.  Likewise, one consideration applies to every alternative and isn't repeated per library either: if your team already has hands-on production experience with a given library and your requirements don't demand something Stellyra provides that the library in question doesn't, that familiarity alone might be enough reason to stick with what you have experience with.

### Maintenance activity

Verified directly from each project's own repository (`git log -1` on a fresh clone).  Stellyra is listed first since it isn't a third-party repository with a comparable commit history to check; the rest are ordered most to least recently active as of this writing:

| Library | Last commit | Age | Notes |
|---|---|---|---|
| Stellyra | -- | -- | New project, actively developed as part of this work |
| libsigc++ | 2026-05-17 (`3.8.1`) | ~3.5 months | |
| sigslot | 2025-07-26 (`Version 1.2.3`) | ~1 year | A version-bump commit, not evidence of new feature work |
| rocket | 2024-08-21 | ~2 years | A bug-fix commit after roughly 4 years of prior silence |
| nano-signal-slot | 2022-08-07 | ~4 years | |
| nod | 2022-05-02 | ~4.3 years | |
| vdk-signals | 2018-08-02 (`Major rewrite`) | ~8 years | Only 2 commits in the repository's entire history; not archived, but no indication of ongoing work |

This is raw commit-history data, not a judgment about code quality - every library in this comparison held up correctly across the benchmarks run against it.  A quiet repository and a poorly-maintained one aren't the same thing.

---

## Qt

### When to use Qt over Stellyra

- **You're already using Qt for other reasons.**  If your application already links against Qt for GUI, QML, D-Bus, or its cross-platform abstractions, signals and slots come along for free.
- **Ecosystem and hiring pool.**  Qt is almost certainly the most widely deployed signal/slot mechanism in the C++ world - more documentation, more engineers who already know the API, and a far longer production track record than Stellyra has.
- **`Qt::BlockingQueuedConnection`.**  A third connection mode where the emitting thread blocks until the queued slot has actually finished running on the target thread - a genuine synchronization primitive (deferred delivery plus a rendezvous) that Stellyra doesn't provide as a built-in option.
- **`QueuedConnection` is Qt's most battle-tested cross-thread mechanism.**  Stellyra's cross-thread story is comparatively young.
- **Signals can name their own parameters.**  A Qt signal is declared as an ordinary member function (e.g. `void valueChanged(int newValue);`), so each parameter gets a real name - visible in the header and in generated documentation, and queryable at runtime via `QMetaMethod::parameterNames()`.  Every other library in this comparison declares a signal as a bare function type or type list (`boost::signals2::signal< void( int ) >`, `sigc::signal< void( Args... ) >`, and so on), so none of them can attach an enforced parameter name either - C++ lets you write one inside a function type purely as documentation, but the compiler discards it.  Stellyra's `Event<Args...>` is the same bare type-list shape, so it has no comparable way to attach a name to a parameter.
- **A signal can declare default argument values, usable at the trigger site.**  `void changed(int value, int role = 0);` lets `emit changed(5)` omit the trailing argument at the point of emission - ordinary C++ default-argument resolution, unaffected by MOC.  This is a different convenience from Stellyra's own partial argument matching (see `MIGRATION.md`, which covers that per library): that one lets a *slot* take fewer arguments than the signal provides, this one lets the *emitting code* supply fewer arguments than the signal declares.  Stellyra's `Event<Args...>::operator()` always requires every argument at the `trigger()`/`emit()` call site - there's no default-value mechanism for a template argument list.
- **`sender()` identifies who triggered the call, from inside the slot, with no setup.**  `QObject::sender()` lets one slot connected to several different signals, or connected to the same signal from several different sender objects, find out which object actually emitted, discovered at call time with no per-connection parameter and no change to the signal itself.  A Stellyra handler only ever has what the event's declared `Args...` give it; identifying the sender has to be baked in at connect time (a bound argument, or a capturing lambda), not discovered from inside the handler.

### When to use Stellyra over Qt

- **Meaningfully faster across most scenarios**, including a real scaling cliff in Qt's per-receiver `QThread` approach under receiver-deferred dispatch at higher receiver counts - not just a constant-factor slowdown.  See `BENCHMARK_RESULTS.md` for current numbers.
- **No framework dependency.**  Stellyra is header-only.
- **Native sender-context deferral, with a real safety implication for Qt specifically.**  `Qt::DirectConnection` synchronously invokes a connected slot from whatever thread happens to call `emit`, with zero built-in synchronization - any coordination needed is on the caller to build.  Stellyra's `BasicEvent::operator()` checks the sender's own thread context on every `trigger()` call and automatically defers to the correct `EventLoop` when needed, regardless of connection type.  Qt's usual workarounds (the two-hop self-connection trick, or `invokeMethod`) are both slower and easy to forget to build in the first place.
- **Tunable concurrency granularity as distinct, swappable types.**  `Event` (fully serialized), `SharedEvent` (parallel-capable), and `SingleThreadedEvent` (zero locking overhead) are independently chosen types.  Qt's connection-type mechanism addresses a different axis (where dispatch happens), not this one (how much serialization the dispatch itself provides).
- **No scaffolding tax to get signals/slots working at all.**  A plain Stellyra `Event` member and a `connect()` call are enough on their own.  Qt's signal/slot machinery sits inside a larger meta-object system that often needs attention even for basic cases: `Q_OBJECT` vs `Q_GADGET` (and knowing which one a given class actually needs), `Q_PROPERTY` declarations for anything exposed to the property system, and `qRegisterMetaType()` / `Q_DECLARE_METATYPE()` for custom types crossing a queued connection - each its own macro or registration call, easy to omit, and often with no compile-time signal that something's missing.  The common failure mode is a connection that silently does nothing at runtime (a queued signal with an unregistered argument type, a slot that doesn't fire because the class needed `Q_OBJECT` and only had `Q_GADGET`), which usually means searching the Qt documentation or Q&A boards to find the solution rather than getting a direct error at the call site.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## Boost.Signals2

### When to use Boost.Signals2 over Stellyra

- **You're already using Boost for other reasons.**  If your project already depends on Boost for other reasons, Signals2 comes without adding a new dependency.
- **Named signal groups.**  Slots can be organised into named groups (any comparable type - int, string, enum) within a single signal, executing in sorted order and disconnectable as a unit.  Stellyra's `ConnectionGroup` manages connections across multiple events rather than ordering slots within one.
- **`track_foreign()` works with any `shared_ptr`.**  Boost's non-invasive lifetime tracking isn't tied to a library-specific base type - any `shared_ptr`-managed object can be tracked this way.  Stellyra's non-inheriting pattern (pairing a receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>`) reaches the same non-invasiveness without requiring `shared_ptr` management of the receiver at all, so this isn't strictly a gap, just a different mechanism for the same goal.
- **C++11 compatibility**, if your toolchain can't move to C++17.

### When to use Stellyra over Boost.Signals2

- **Any cross-thread deferred dispatch requirement.**  Boost.Signals2 invokes slots synchronously on the emitting thread with no queuing or affinity model - there's no built-in way to defer delivery to a different thread.
- **You don't want to take on a dependency on a huge library just for event handling.**  Boost.Signals2 is itself header-only, so this isn't a build-system distinction the way header-only-vs-compiled usually is, but adopting it still means depending on Boost as a whole for one feature: versioning and vendoring Boost, wiring up `find_package(Boost)` (or equivalent) in the build, and a far larger header footprint than the handful of files Signals2 itself needs.  Stellyra depends on nothing beyond the C++ standard library.
- **Built-in return-value combiners.**  Stellyra ships 10+ built-in combiners; Boost.Signals2 ships 2 (`optional_last_value`, the default, and `last_value`, kept for compatibility with the original Boost.Signals library - there's no `first_value`) and needs a custom combiner for anything else.
- **Connection and event introspection beyond a single handle.**  Boost.Signals2's `connection::connected()` gives a live state query for one connection, which Stellyra also provides - but Boost has nothing comparable to `EventInspector`'s per-connection snapshots and aggregate statistics across an entire event.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## nano-signal-slot

### When to use nano over Stellyra

- **Raw single-threaded and same-thread dispatch speed.**  nano is the fastest or near-fastest library in nearly every single-threaded benchmark.  If your dispatch is entirely same-thread and call-count dominates your profile, nano's minimalism wins.
- **Concurrent dispatch speed too, not just single-threaded.**  nano's `TS_Policy` - a spin lock held for the entire handler-invocation loop, the simplest serialization strategy of any library compared - wins the raw contention-scaling comparison outright at every thread count tested, 1 through 32, ahead of Stellyra's lock-free `ConcurrentEvent`.  See Scenario 9 in `BENCHMARK_RESULTS.md`.
- **Header-only, effectively zero-overhead when you don't ask for thread safety.**   Its `TS_Policy` variant provides genuine, fully serialized concurrent dispatch (the policy's spin-lock is held for the entire handler-invocation loop, not just a connection-list snapshot) at a smaller footprint than Stellyra's `Event`.
- **Simplicity.**  No deferred dispatch, no thread-affinity concept, no introspection.  If that matches what you actually need, it's a smaller thing to reason about.

### When to use Stellyra over nano

- **Any cross-thread deferred dispatch requirement.**  nano has no concept of a thread, event loop, or queue - it cannot defer delivery under any configuration.
- **Composition, not just inheritance, for lifetime tracking.**  nano's `Observer` mixin requires inheritance - its `Signal::connect()` overload requires the receiver pointer to be implicitly convertible to `Observer*`, a conversion only inheritance provides.  Stellyra's `Trackable`/`Anchor` supports both.
- **No policy parameter to keep in sync.**  `Signal<RT(Args...), MT_Policy>` inherits `Observer<MT_Policy>`, and nano's own README states as a warning, not a compile-time check, that "when using a non-default Policy you must make sure that both Signal and Observer use the same policy" - it goes on to recommend hand-written type aliases specifically to reduce the risk of the two drifting apart.  Stellyra's `Trackable` has no analogous template parameter to keep synchronized with an event's `MutexType` - it's a single concrete type usable with any `BasicEvent<MutexType, Args...>`.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## libsigc++

### When to use libsigc++ over Stellyra

- **GTK+/gtkmm integration.**  It's gtkmm's native signal system, so if your application already uses gtkmm, signals and slots come along for free.
- **Built-in signal adaptors.**  `sigc::bind()`, `sigc::hide()`, and `sigc::retype()` transform a slot's signature in-place (pre-binding an argument, discarding a trailing one, retyping a parameter) without writing a wrapper lambda.  Stellyra has no equivalent adaptor layer - the same effect requires a lambda at the call site.
- **`sigc::exception_catch()` catches an exception thrown inside a connected slot, with no try/catch written at the call site.**  It wraps a functor so an exception raised inside it is routed to a separate catcher functor instead of propagating out, and catchers can be cascaded to handle multiple exception types.  Stellyra has no equivalent, and the consequence of an uncaught exception depends on the connection type: for a Direct connection it propagates to the caller of `trigger()`/`emit()`, where the engineer can catch it - but for a Deferred connection there's no such caller left on the stack by the time the handler runs; the exception escapes whichever `EventLoop::drain()` call is executing it instead (neither `dispatch()` nor `drain()` wraps handler invocation in a try/catch).  Run manually, that surfaces at the `drain()` call site.  Run via `AutoDrainThread`, it escapes that thread's entry function entirely, which is `std::terminate()` by the C++ standard - not something the engineer can catch after the fact at all.
- **`sigc::compose()` gives built-in functional composition for a slot's inputs.**  It chains the return value(s) of one or two "getter" functors into a "setter" functor's arguments, and the result connects directly like any other slot (confirmed in `sigc++/adaptors/compose.h`).  Stellyra has no ready-made composition utility - `connectLambda()` accepts any callable, so the same effect is reachable, but the composition itself (a lambda, a reusable functor, or `std::bind`) has to be written by the engineer rather than available in the library.
- **C++11 compatibility**, if your toolchain can't move to C++17.

### When to use Stellyra over libsigc++

- **Thread safety.**  libsigc++ is explicitly not thread-safe and requires external synchronization for any concurrent access; Stellyra's `Event`/`SharedEvent`/`ConcurrentEvent` give you that guarantee as a type choice, with no synchronization to build yourself.
- **No thread-safe invocation guarantee at all.**  `sigc::trackable`'s destruction cleanup works correctly in single-threaded code but isn't protected against concurrent emission and destruction.  Stellyra's `Trackable` disconnect is serialized against any in-flight dispatch unconditionally, whether or not the application is otherwise single-threaded.
- **Any cross-thread deferred dispatch requirement.**  libsigc++ has no thread/loop/queue concept whatsoever.
- **Bulk-disconnect at scale.**  `sigc::trackable::notify_callbacks()` shows a severe scaling cliff as the number of tracked connections grows, well beyond Stellyra's `extractConnectionsTo()` (see `BENCHMARK_RESULTS.md`, Scenario 8b) - though the two aren't scoped identically: `notify_callbacks()` disconnects the receiver from every signal it's connected to, where Stellyra's call disconnects only the connections to one specific event.
- **Built-in return-value combiners.**  Stellyra ships 10+ built-in combiners; libsigc++ supports custom combiners but ships none itself.
- **No LGPL dynamic-linking obligation.**  libsigc++ is LGPL and typically linked dynamically; Stellyra is header-only under a permissive license, which removes that consideration entirely for a safety-critical SOUP evaluation.
- **No non-virtual-destructor footgun in the lifetime-tracking base.**  libsigc++'s own documentation for `sigc::trackable` warns, verbatim: "there is no virtual destructor. Don't use `trackable*` as pointer type for managing your data or the destructors of your derived types won't be called when deleting your objects" (`sigc++/trackable.h`).  Stellyra's `Trackable` declares `virtual ~Trackable()`, so that specific class of bug can't occur regardless of how a `Trackable*` is used to manage a derived receiver's lifetime.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## sigslot

### When to use sigslot over Stellyra

- **Established, simple, synchronous-only signal/slot needs.**  A reasonably well-known, mature, header-only library.  If your application never needs cross-thread deferred dispatch, there's little reason to introduce Stellyra's larger surface area.
- **Opt-in access to a full connection handle from inside the slot, lambdas included.**  `connect_extended()` passes the slot a `sigslot::connection&` as its first parameter - this works for a lambda exactly as well as for a member function - giving `disconnect()`, `block()`/`unblock()`, `connected()`/`valid()`, and a `connection_blocker` RAII helper, all without the caller needing to have kept the handle `connect()` returned.  It's less ambient than rocket's thread-local `current_connection()` (see the rocket section below) since it requires calling `connect_extended()` instead of plain `connect()` and accepting the extra leading parameter, but it reaches lambdas the same way Stellyra's by-value `disconnect(receiver, &Class::method)` can't.

### When to use Stellyra over sigslot

- **Concurrent scaling with more receivers.**  sigslot shows the worst degradation under load of any library in the serialized-dispatch comparison, including libraries with no built-in serialization at all.  See `BENCHMARK_RESULTS.md` for current numbers.
- **Any cross-thread deferred dispatch requirement.**  sigslot has no thread/loop/queue concept whatsoever.
- **Non-invasive tracking that doesn't require `shared_ptr` management.**  sigslot's inheritance-based tracking (`observer`/`observer_base`) is the only option for a plain-pointer receiver; its other mechanism, ADL-detected `to_weak()` tracking (see "Why Stellyra over any other library?" below), reaches non-invasiveness only by requiring the receiver to already be `shared_ptr`/`weak_ptr`-managed.  Stellyra's `Trackable`/`Anchor` composition via `Tracked<T>` is non-invasive for an ordinary, non-`shared_ptr`-managed receiver too.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## rocket

### When to use rocket over Stellyra

- **Built-in timer scheduling tied to the signal system itself, if you're willing to patch the header.**  `set_timeout` (and a corresponding interval variant) lets you schedule a delayed call to a slot without building your own timer infrastructure - but rocket's shipped `rocket.hpp` self-defines `ROCKET_NO_TIMERS` unconditionally before the feature's own guard is ever checked, so the feature is dead code by default with no supported `-D` opt-in; getting it working means hand-editing your local copy of the header to delete that guard, not just defining a macro.  Stellyra has no scheduled dispatch built in either way.
- **A handler can abort the rest of its own emission.**  `rocket::abort_emission()`, callable from inside a running handler, stops any further handlers from running for that emission.  This one is real and unconditional - not macro-gated.  Stellyra has no equivalent - once dispatch starts, every connected handler runs.
- **A full connection handle, reachable from inside any slot with zero setup - including a lambda.**  `rocket::current_connection()` is thread-local and returns a complete `rocket::connection` (`disconnect()`, `is_connected()`, copyable and storable for later use) from inside *any* connected slot, lambdas included - no capture, no reference to the signal needed (see rocket's README doc).  `current_connection()` and `is_connected()` are both unconditional; `block()`/`unblock()`/`is_blocked()` are also real methods on `connection`, but - like the timer feature above - they're dead code in the shipped header by default (`ROCKET_NO_BLOCKING_CONNECTIONS` is self-defined the same way), so don't count on them without the same hand-edit.  Stellyra's closest equivalent - `event.disconnect(*this, &Class::method)` or `event.disconnectFree(&func)`, both real, supported, by-value self-disconnect paths (confirmed in `event_storage.h`; legitimate reentrant self-disconnect for `Event`'s default type is explicitly documented in `ARCHITECTURE.md`'s Reentrancy section) - still requires the handler to already have a reference to the sender event in scope or have the ability to access the sender event.  This kind of in-handler reentrant self-management is also unsupported on `SharedEvent`/`CombiningEvent` specifically (`KNOWN_ISSUES.md`) - a deadlock risk there, not just an inconvenience.
- **Public-domain licensing**, if that specific model matters for your project.
- **Reasonable baseline single-threaded speed**, a bit slower than Stellyra's baseline but broadly comparable.

### When to use Stellyra over rocket

- **A real safety footgun in rocket's naming.**  `thread_safe_policy` sounds like it means handler execution is mutually exclusive, but it only protects the connection list during traversal - concurrent `emit()` calls from different threads can genuinely run their handlers in parallel with no synchronization.  A developer relying on the word "safe" to mean "serialized" would be wrong, silently.  Stellyra's `Event` vs. `SharedEvent` split makes this distinction explicit in the type system.
- **A second, related footgun for composition.**  rocket's `trackable` auto-disconnect only activates through public inheritance.  Composing it as a plain member instead (the way Stellyra's `Anchor` concept explicitly supports) silently skips tracking that connection, with no compile error or warning.
- **No sender- or receiver-affinity cross-thread dispatch.**  rocket's queued-connection feature (off by default, via `ROCKET_NO_QUEUED_CONNECTIONS`) only targets the thread that called `connect()` - there's no way to bind a connection to the sender's thread or the receiver's thread the way Stellyra's `EventLoop` does.
- **A third, library-wide footgun: four advertised features are dead code as shipped, with no supported opt-in.**  Timers, connection blocking, queued connections, and the smart-pointer extensions are each gated behind a macro that `rocket.hpp` self-defines before the feature's own guard is ever reached - so predefining the macro to `0` or any other value still counts as "defined" and the feature stays off.  The README's "opt-out by defining X" framing for these features doesn't match the shipped code, which is opt-out of something that was never on.  The only way to get any of them working is a hand-edit of the local header, not a `-D` flag - a materially different maintenance posture than "just `#include` it."  Stellyra has no equivalent gap: everything in its feature list works out of the box.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## nod

### When to use nod over Stellyra

- **Small, simple, MIT-licensed, single-header library.**  `nod.hpp` is a single ~680-line header, dependency-free, and targets C++11 rather than C++17 - a smaller, more self-contained unit to read, audit, or vendor than Stellyra's multi-header split.  If your requirements don't need anything Stellyra provides that nod doesn't (see the section below), the smaller surface area and the broader toolchain compatibility (C++11, for a toolchain that can't move to C++17) may be reason enough on their own.
- **A different aggregation strategy per call, with no combiner type to declare.**  `signal::accumulate(init, op)` and `signal::aggregate<Container>(args...)` let the *same* signal object be triggered with an ad-hoc reduction or a different target container at each call site - e.g. `sig.accumulate(0, std::plus<int>{})(10, 100)` then `sig.accumulate(1, std::multiplies<int>{})(10, 100)` on the same `sig`, with zero combiner code written.  Stellyra's `CombiningEvent<Combiner, ReturnType, Args...>` bakes the combiner into the event's *type* at declaration time - getting a different aggregation strategy, or just collecting results into a container, means picking one of the 10+ built-in combiners or writing a custom combiner functor; there's no per-call override of an existing `CombiningEvent`.

### When to use Stellyra over nod

- **Performance.**  nod is the slowest library in nearly every benchmark, often by a wide margin, and the gap persists regardless of thread count.  The root cause is structural: `nod::signal`'s dispatch heap-allocates and returns a full copy of the connection list on every single emission, independent of whether anything concurrent is even happening.
- **No automatic receiver-lifetime tracking at all.**  nod only provides `scoped_connection`, an RAII wrapper tied to the connection handle's own lifetime, with no way to tie teardown to the receiver object's destruction.  Stellyra offers "destroy the receiver, the connections clean up automatically."
- **A "thread-safe means serialized" footgun.**  `multithread_policy` (nod's default) also only protects the connection list, not handler execution.
- **nod has no cross-thread deferred dispatch.**

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## vdk-signals

### When to use vdk-signals over Stellyra

- **Raw dispatch speed under sustained concurrent load**, especially when connections are set up once (or rarely) and dispatched to heavily rather than churned at runtime.  Two qualifications on this, though: Stellyra's execute-now *serialised* dispatch (handlers externally forced one at a time) beats vdk's equivalent outright.  And even in the raw, unserialized comparison, it's not one-sided - Stellyra's lock-free `ConcurrentEvent` is narrowly ahead at very low contention (1-2 threads); vdk edges ahead from 4 threads on, though the two stay close together through 8 threads, and the gap (biggest around 1.7x at 16 threads) narrows again by 32.  See `BENCHMARK_RESULTS.md` for current numbers.
- **No explicit call needed to bind a receiver to its thread, for the common case.**  `vdk::context` captures the constructing thread automatically, so a receiver built on the thread it's meant to run on is ready for thread-affine dispatch immediately - nothing analogous to Stellyra's `setEventLoop()` call has to be made.  Stellyra's `Trackable` has no `EventLoop` until `setEventLoop()` is explicitly called; until then, `Auto`-resolved connections fall back to `Direct` if the sender has no `EventLoop` either (the common "nothing's been migrated yet" case), or to `None` - silently dropping events, not an error - if the sender already has an `EventLoop` but this receiver doesn't yet.  The tradeoff is the next bullet below: vdk's binding is fixed at construction and can't be reassigned afterward the way Stellyra's can.
- **Disconnect by value reaches ordinary functors too, not just methods and free functions.**  vdk's `disconnect(object, slot)` / `disconnect(slot)` removes a connection by re-supplying an equal value for a method-plus-object pair, a free function, *or* an equality-comparable functor, with no handle kept from `connect()`.  Stellyra's equivalent by-value `disconnect(receiver, &Class::method)` / `disconnectFree(&func)` covers methods and free functions the same way, but has no counterpart for a functor - that still needs the `Connection` `connect()` returned.  Neither library's by-value path reaches an ordinary capturing lambda, since closures aren't equality-comparable in C++ either way - vdk's own docs state disconnection by id is used for exactly that case, which is what Stellyra's `Connection` handle is for too.

### When to use Stellyra over vdk-signals

- **Connect/disconnect speed.**  If your application does frequent dynamic connection churn (e.g. regular sender/receiver construction/destruction) rather than mostly emitting to a stable set of receivers, this could dominate real-world performance regardless of vdk's dispatch-speed advantage.
- **Two separate axes of thread control, neither of which vdk has an equivalent for.**  Stellyra lets you control both where a *sender* lives (via the sender's own `EventLoop`, checked automatically on every `trigger()` regardless of connection type) and where each *receiver* lives (via `ConnectionType::Direct`/`Deferred`/`Auto`, chosen per connection).  vdk only has the second axis, and even that is fixed at receiver *construction* time, not connect time: a receiver's channel is captured from whichever thread constructs its `context` base, not whichever thread later calls `connect()`.  To bind a vdk receiver to a specific worker thread, that receiver must actually be constructed on that thread - there's no way to construct it elsewhere and assign it a different home afterward the way `receiver.setEventLoop()` allows.
- **Built-in serialized concurrent dispatch as a first-class, ready-made option.**  vdk's `exec::sync` dispatches immediately, inline, on whichever thread called `emit()`, with no locking at all.  That's what makes it fast, but it also means concurrent `emit()` calls from different threads genuinely run their handlers in parallel, with no built-in way to force one-at-a-time execution short of adding your own external lock (exactly what had to be done to measure vdk on equal footing in the "execute-now, serialised" benchmark).  Stellyra's `Event` type gives you that guarantee natively, by simply choosing a connection type.
- **No header-only integration.**  vdk requires compiling `signals.cpp` as a real translation unit, unlike Stellyra's header-only model.
- **A small but real API-discoverability gap.**  vdk's "auto-resolve" dispatch behavior is the implicit zero-value of its `exec` enum rather than a named enumerator, discoverable only by reading the dispatch switch itself.  Stellyra's equivalent, `ConnectionType::Auto`, is an explicit, named, self-documenting enum member.

See "Why Stellyra over any other library?" at the end of this document for features unique to Stellyra, not specific to this comparison.

---

## Why Stellyra over any other library?

Features below are unique to Stellyra among every library in this comparison.  Listed once here rather than repeated in each section above.

- **Native sender-context deferral.**  `BasicEvent::operator()` checks the sender's own thread context on every `trigger()` call and automatically defers dispatch to the sender's `EventLoop` when triggered from elsewhere, regardless of connection type.  Qt has no automatic equivalent - only hand-built, approximate workarounds (the two-hop self-connection trick, or invokeMethod); no other library listed here has any path to this at all.
- **Conditional connections via an arbitrary predicate, with explicit sender/receiver evaluation context.**  `ConnParams::when()` (or its constructor form) gates a handler on a predicate evaluated either in the receiver's context (default - right before the handler runs, during the drain for Deferred connections) or the sender's context (right at `trigger()`, before a Deferred task is even posted), via `PredicateContext::Receiver`/`::Sender`.  nod, rocket, vdk-signals, nano-signal-slot, sigslot, and libsigc++ don't have anything predicate-shaped on a connection.  Qt has nothing comparable either - its context-object overload gates a connection's *lifetime* on an object's destruction, not on a condition evaluated per invocation.
- **Four distinct, swappable concurrency-guarantee types, not a single on/off policy.**  `Event` (fully serialized, RecursiveMutex), `ConcurrentEvent` (lock-free, epoch-based reclamation), `SharedEvent` (parallel-capable), and `SingleThreadedEvent` (zero locking overhead) let the guarantee level be chosen per event as a type, rather than a single library-wide policy flag.  `Event` and `ConcurrentEvent` aren't strictly ordered by speed - `Event`'s uncontended mutex is cheaper at low thread counts, `ConcurrentEvent`'s fixed per-call cost only pays off once real contention sets in (the crossover is between 4 and 8 threads - see Scenario 9).  `BasicEvent` supports specifying the mutex type, so custom mutex/locks can be used to extend concurrency beyond the 4 built-in variants.
- **Both inheritance and composition for lifetime tracking.**  `Trackable` has a public constructor and ships an explicit `Anchor` alias for composition.  Every other inheritance-based tracking mechanism checked (Qt's `QObject`, nano's `Observer`, rocket's `trackable`, vdk's `context`) requires actual inheritance - vdk's case is the strictest: `context`'s constructor and destructor are `protected`, so there isn't even a way to construct one as a composed member.  sigslot's `observer_base` path is the same way, but sigslot also has a completely separate, non-invasive mechanism - Argument-Dependent Lookup (ADL)-detected `to_weak()` tracking for `shared_ptr`/`weak_ptr`-managed receivers (see the sigslot section) - that needs neither inheritance nor a composed tracker object at all, so it isn't a clean fit for either side of this "inheritance vs. composition" framing.
- **Genuine connection and event introspection.**  `EventInspector` provides per-connection snapshots and aggregate event statistics, human-readable dumps, and Graphviz DOT export.  It does not report a connected receiver's address or concrete type - only whether a connection is a "method" or "function/lambda" connection, since `Callable` doesn't expose its bound object pointer generically.  Nothing else tested provides anything comparable regardless.
- **Event recording.**  `RecordableBasicEvent` and `EventRecorder` provide a dedicated mechanism for capturing emissions, with an explicit recording-mode lifecycle.  Nothing else tested has an equivalent.
- **Compile-time-enforced public connect, private trigger.**  `PrivateEvent<FriendType, Args...>` (aliased `PEvent`) wraps an event with private inheritance plus `friend FriendType`, so external code can call `connect()`/`disconnect()`/`block()` freely but only `FriendType` can call `operator()`/`trigger()`/`emit()`.  Qt is the only other library here with anything comparable - the `private signals:` convention, where MOC-generated connection infrastructure bypasses normal C++ access control for `connect()` while ordinary access control blocks external `emit`.  Both are compile-time enforced, but Qt's needs the MOC toolchain and a coding convention; Stellyra's is enforced by the type system alone, with the owner class as a template parameter.  Every other library here only supports private triggering by making the whole event/signal a private member, which blocks external `connect()` too.

**Not unique, but uncommon features**

- **Return value combiners.**  Stellyra's `CombiningEvent` ships several built-in combiners.  Boost.Signals2's `Combiner` template parameter, rocket's `Collector` template parameter, libsigc++'s `signal_with_accumulator`, and nod's per-call `accumulate()`/`aggregate()` are all comparable mechanisms - nod's is the most flexible of the four, since it doesn't require declaring a combiner type at all.
- **Priority ordering, though more granular than what exists elsewhere.**  `ConnParams::prio()` orders handlers per-connection, by an arbitrary integer.  Boost.Signals2's named signal groups (executed in sorted order) and sigslot's `group_id` both give a comparable ordering mechanism, just at a coarser, group-level granularity rather than per-connection.
- **A ready-made continuous-drain abstraction.**  `AutoDrainThread` gives you a working drain loop for an `EventLoop` out of the box.  Qt's `QThread` by default runs an event loop when calling `QThread::start()`.  vdk's closest analog, `signals_execute()`, is a raw primitive you must call in your own hand-written loop.
- **Partial argument matching, without hand-written adaptors.**  A handler can omit trailing arguments the event provides, resolved automatically for member function connections at compile time with no runtime cost.  Qt does the same automatically, via MOC-generated invocation wrappers.  Boost.Signals2 and libsigc++ support it too, but only through explicit adaptor boilerplate (`boost::bind`, `sigc::hide`) that the engineer has to write at each call site.  sigslot, nano-signal-slot, vdk-signals, rocket, and nod require an exact signature match - there's no mechanism for this at all.
- **Event-to-event forwarding.**  `forwardTo()` relays one event's triggers into another.  sigslot's freestanding `connect(sig1, sig2)` - documented in its own readme as "Signal chaining" - connects one signal to another with compatible arguments, a directly comparable mechanism.  Qt goes further still: since a signal is just an ordinary member function, connecting one signal directly to another (`connect(&sender, &Sender::someSignal, &relay, &Relay::anotherSignal)`) needs no special syntax at all beyond a normal `connect()` call.
