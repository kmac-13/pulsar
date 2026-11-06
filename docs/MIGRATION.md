# Migrating to Stellyra

This guide covers migrating to Stellyra from other C++ event libraries.  Each section lists the required changes, notes any non-invasive alternatives, and highlights features that Stellyra provides which may not have an equivalent in the source library.

---

## From Qt Signals

**Difficulty:** ⚠️ Medium

### Changes needed (full migration - class drops QObject dependency entirely)

Applies to a class with no remaining dependency on Qt's meta-object system.

- remove `Q_OBJECT` macro and `signals` (`Q_SIGNALS`) / `slots` (`Q_SLOTS`) access specifiers
- replace `QObject::connect()` calls with `event.connect()`
- replace `emit signal(args)` / `Q_EMIT signal(args)` with `event(args)` (or `event.trigger(args)` or `event.emit(args)`)
- receivers: either inherit `stellyra::Trackable` (simplest) or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` member via `Tracked<T>` (non-invasive alternative)

**Not a candidate for full migration if the class still needs:**
- **`Q_INVOKABLE`** - meta-object method invocation (`QMetaObject::invokeMethod`, QML, scripting)
- **`Q_PROPERTY` with a `NOTIFY` signal** - Qt's property-binding system depends on that signal firing through the meta-object system; this predates QML and applies equally to Qt Designer's property editor, Qt Script/scripting bindings, D-Bus property exposure (`QDBusAbstractAdaptor`), and any other code that reads properties via `QMetaProperty`
- **`QML_ELEMENT` / `QML_SINGLETON` / `QML_UNCREATABLE`** - QML's binding engine has no visibility into Stellyra

Removing `Q_OBJECT` removes all of these at once, with no partial version - there's no way to drop `Q_OBJECT` and keep any of them working.  A class with any of these dependencies must stay on Qt; migrate a different class, perform a partial migration, or don't migrate this one at all.

### Partial migration (class stays QObject, only some signals move)

Applies when a class must keep `Q_OBJECT` for `Q_INVOKABLE`, QML registration, or property bindings, but has other, purely-internal signals that aren't exposed to any of that.

- `Q_OBJECT`, `Q_PROPERTY`, `Q_INVOKABLE`, and QML registration macros all stay untouched
- only signals with no `Q_PROPERTY NOTIFY` reference and no QML/scripting exposure are candidates to replace with Stellyra `Event`s
- the class ends up with both Qt signals (for QML/property/invokable needs) and Stellyra `Event`s (for internal communication) side by side - `Q_GADGET` classes fall into this same bucket by default, since they have no signals/slots to migrate at all, but can freely add Stellyra `Event`s for anything that isn't property/QML-driven

### Sender - non-invasive option

Instead of inheriting `stellyra::Trackable`, embed it as a member and pass its address to the Event constructor:

```cpp
// Qt style
class CustomClass : public QObject {
	Q_OBJECT
signals:
	void customSignal( int value );
};

// Stellyra - with inheritance
class CustomClass : public stellyra::Trackable {
public:
	stellyra::Event< int > customEvent{ this };
};

// Stellyra - without inheritance (non-invasive)
class CustomClass {
	stellyra::Trackable _anchor;
public:
	stellyra::Event< int > customEvent{ &_anchor };
	CustomClass() {}
};
```

### Connection syntax

```cpp
// Qt
QObject::connect( sender, &Sender::customSignal, receiver, &Receiver::customSlot );

// Stellyra
sender->customEvent.connect( receiver, &Receiver::customSlot );
```

### Thread safety

Qt's `AutoConnection` compares the **emitting thread** against the receiver's thread affinity - emission always runs on the calling thread, and if that differs from the receiver's thread, the slot is queued to the receiver.  The emitting object's own thread affinity plays no role in that decision.  Stellyra's `Auto` connection instead compares the **sender's EventLoop** against the current drain context: if the event isn't being processed in the sender's own loop, Stellyra defers the entire emission there first, regardless of which OS thread is involved.  One consequence: multiple manual `EventLoop`s can coexist on a single OS thread and still dispatch correctly, a pattern `QObject`'s thread-affinity model can't express.

**Moving a receiver between threads mid-flight:** Qt's `QObject::moveToThread()` and Stellyra's `Trackable::setEventLoop()` solve a similar problem - relocating a live receiver while deferred work may already be queued for it - but migrate at different granularities.  `moveToThread()` scans the calling thread's entire posted-event list for anything addressed to the object (or, recursively, its children) and relocates each one individually - so an already-posted queued signal/slot call or `deleteLater()` ends up delivered on the new thread rather than stranded or dropped.  `setEventLoop()` moves only pending, not-yet-draining tasks tagged for that receiver in one call; a task already inside the old loop's active drain batch finishes there rather than being migrated mid-dispatch.  Both guarantee nothing still queued is lost, just via different mechanisms - event-by-event scanning for Qt, a single batch-aware move for Stellyra.

### Features gained

- event recording and replay
- return value aggregation (CombiningEvent)
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- EventInspector with Graphviz export
- sender-side emission deferral
- permissive licensing - Stellyra is BSD 3-Clause; Qt Signals is LGPL/Commercial, which requires either dynamic linking (LGPL) or a paid commercial license (for static linking or closed-source distribution) - that obligation goes away entirely

### Features lost / changed

- string-based dynamic connections (old SIGNAL/SLOT macro syntax) - no equivalent
- named signal parameters
- Qt Designer visual wiring - no equivalent
- QML / scripting integration - no equivalent

---

## From Boost.Signals2

**Difficulty:** ✅ Easy

### Changes needed

- replace `boost::signals2::signal< void( Args... ) >` with `stellyra::Event< Args... >`
- replace `signal.connect( slot )` with `event.connect( receiver, slot )`
- receivers: either inherit `stellyra::Trackable` (simplest) or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>` - equivalent to Boost's `track_foreign( shared_ptr )` pattern, but without requiring the receiver to be `shared_ptr`-managed
- no `shared_ptr` wrapping required for receivers - ordinary C++ object lifetime management applies
- replace `signal( args )` with `event( args )` (or `event.trigger( args )` / `event.emit( args )`)

### Connection syntax

```cpp
// Boost.Signals2
boost::signals2::signal< void( int ) > sig;
sig.connect( &handler );
sig( 42 );

// Stellyra
stellyra::Event< int > event{ this };
event.connect( receiver, &Receiver::handler );
event( 42 );
```

### Named signal groups

Boost's named groups let you tag slots with a group key at connect time and later disconnect every slot sharing that key in one call (`sig.disconnect(group_id)`) - a connect-time tagging mechanism, not primarily an ordering one (sorted-by-group execution is a side effect, not the point).  Stellyra has no equivalent group-key parameter on `connect()`. The closer match is `ConnectionGroup`: create an instance and += each connection into it (works for a single event or across several), then use `disconnectAll()`, `blockAll()`, or `unblockAll()` to act on every connection in the group at once - the same bulk-operation capability Boost's named groups provide, just reached through an explicit object instead of a connect-time key.  The difference is mechanism, not sequencing: Boost's grouping is implicit, via a key passed to `connect()` itself, with no object to create; Stellyra's is explicit, via an object you create and add connections to.  Since Stellyra's `Connection` is a lightweight, copyable handle, the same connection can be added to more than one `ConnectionGroup` at once - something Boost can't do with a single `connect()` call, since each one assigns its slot to exactly one group.  The trade-off: because every copy refers to the same underlying handler, disconnecting through any one group disconnects it everywhere, including in any other group still holding a copy.

```cpp
// Boost - named groups
sig.connect( 1, slotA );  // group 1
sig.connect( 2, slotB );  // group 2
sig.disconnect( 1 );      // bulk-disconnect everything in group 1

// Stellyra - ConnectionGroup
ConnectionGroup group1, group2;
Connection conn = event.connect( receiver, &Receiver::slotA );
group1 += conn;
group2 += conn;          // same connection, both groups
group1.disconnectAll();  // disconnects conn - group2's copy now reports disconnected too
```

### Features gained

- cross-thread deferred connections and EventLoop support
- event recording and replay
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- EventInspector with Graphviz export
- sender-side emission deferral
- 10+ built-in combiners (vs Boost's 2)

### Features lost / changed

- partial argument matching - Boost supports this already, but only via explicit adaptor boilerplate (boost::bind) written at each call site; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via connectFree<&Func>() - but not for the pointer-value connectFree(funcPtr) form, and not for lambdas, both of which require an exact signature match
- license changes from the Boost Software License to BSD 3-Clause - both are permissive with no copyleft obligation, so this is a lateral move, not a functional change

---

## From libsigc++

**Difficulty:** ⚠️ Medium

### Changes needed

- replace `sigc::signal< void( Args... ) >` with `stellyra::Event< Args... >`
- replace `signal.connect( slot )` with `event.connect( receiver, slot )`
- receivers: either inherit `stellyra::Trackable` (simplest) or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>` - replaces the `sigc::trackable` base class or `shared_ptr<sigc::trackable>` member
- no `shared_ptr` wrapping required for receivers - ordinary C++ object lifetime management applies
- replace `signal.emit( args )` with `event( args )` (or `event.trigger( args )` / `event.emit( args )`)

### Signal adaptors

libsigc++'s `hide()` adaptor (discarding trailing arguments) is usually unnecessary in Stellyra - partial argument matching already drops trailing arguments automatically for member-function connections, and for free functions connected via `connectFree<&Func>()`.  `bind()` (pre-binding an argument) and `retype()` (changing a slot's apparent signature) have no direct equivalent; use a lambda for those:

```cpp
// libsigc++
sig.connect( sigc::hide( slot ) );          // discard trailing arg
sig.connect( sigc::bind( slot, value ) );   // pre-bind an arg

// Stellyra
event.connect( receiver, &Receiver::slot );                     // trailing arg dropped automatically
event.connect( receiver, [value]( int ) { slot( value ); } );   // pre-bind - lambda still needed
```

### Features gained

- thread safety (libsigc++ is not thread-safe)
- cross-thread deferred connections and EventLoop support
- event recording and replay
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- sender-side emission deferral
- EventInspector with Graphviz export
- no LGPL dynamic-linking obligation - Stellyra is BSD 3-Clause; libsigc++ is LGPL and is typically linked dynamically to satisfy that, a consideration this removes entirely

### Features lost / changed

- signal adaptors (`bind`, `retype`) - no direct equivalent; use lambdas.  `hide()` usually isn't needed at all, since partial argument matching already drops trailing arguments for member-function connections (and free functions via `connectFree<&Func>()`)
- return value accumulation - libsigc++ supports this via `signal<Ret(Args...)>::accumulated<T_accumulator>`, which requires hand-writing a full accumulator type (`operator()(first, last)` over an iterator range of the slots' return values) and declaring the signal with that type built in; libsigc++ ships no pre-built accumulator types.  Stellyra accumulates through its normal call syntax (`operator()`/`trigger()`/`emit()`) using whichever combiner the event was given, and ships 10+ built-in named combiners (`LogicalAnd`, `Sum`, `Maximum`, etc.) so a custom accumulator type usually isn't needed
- GTKmm integration - if using gtkmm, consider whether the migration is worth it; libsigc++ is the native signal system for gtkmm

---

## From nano-signal-slot

**Difficulty:** ✅ Easy

### Changes needed

- replace `Nano::Signal< void( Args... ) >` with `stellyra::Event< Args... >`
- replace `Nano::Observer<Policy>` base class with `stellyra::Trackable` (or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>` - nano has no equivalent non-invasive option)
- replace `sig.fire(args)` with `event(args)` (or `event.trigger(args)` / `event.emit(args)`)
- no `shared_ptr` wrapping required for receivers - ordinary C++ object lifetime management applies
- connection syntax is nearly identical - nano's `sig.connect<&Receiver::slot>(receiver)` / `sig.disconnect<&Receiver::slot>(receiver)` carry over directly as `event.connect<&Receiver::slot>(receiver)` / `event.disconnect<&Receiver::slot>(receiver)`; Stellyra also returns a `Connection` handle from `connect()` if you'd rather store and disconnect that way, which nano doesn't offer:

```cpp
// nano-signal-slot
sig.connect< &Receiver::slot >( receiver );
sig.disconnect< &Receiver::slot >( receiver );

// Stellyra - same NTTP syntax carries over directly
event.connect< &Receiver::slot >( receiver );
event.disconnect< &Receiver::slot >( receiver );

// Stellyra also offers a Connection handle, if preferred
auto conn = event.connect( receiver, &Receiver::slot );
conn.disconnect();
```

### Policy system

nano's compile-time policy selection (`ST_Policy`, `TS_Policy`) has a per-event equivalent in Stellyra: use `stellyra::SingleThreadedEvent<Args...>` (backed by a zero-overhead `NullMutex`) in place of `stellyra::Event<Args...>` for a signal that never needs locking, chosen per instance rather than library-wide:

```cpp
// Equivalent to nano's ST_Policy for one signal - removes mutex overhead for just this event
stellyra::SingleThreadedEvent< int > sig;
```

(Stellyra also has a separate, library-wide `STELLYRA_THREAD_SAFE=0` flag for bare-metal targets that removes real synchronization primitives everywhere - a coarser, whole-library switch rather than nano's per-signal policy choice.)

### Features gained

- cross-thread deferred connections and EventLoop support
- conditional connections (via `ConnParams`/`.when()`), single-shot connections
- priority ordering
- sender-side emission deferral
- partial argument matching - nano requires an exact signature match with no mechanism for this at all; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via `connectFree<&Func>()` (not for the pointer-value `connectFree(funcPtr)` form, and not for lambdas, both of which still require an exact match)
- selective disconnect and block per connection (connection handles)
- event recording and replay
- EventInspector with Graphviz export

### Features lost / changed

- license changes from MIT to BSD 3-Clause - both are permissive with no copyleft obligation, so this is a lateral move, not a functional change
- return value accumulation - nano supports this via `fire_accumulate(accumulator, args...)`, which requires a hand-written accumulator callable at each call site; Stellyra accumulates through its normal call syntax (`operator()` / `trigger()` / `emit()`) using whichever combiner the event was given, and ships 10+ built-in named combiners (`LogicalAnd`, `Sum`, `Maximum`, etc.) so a custom accumulator usually isn't needed

---

## From sigslot

**Difficulty:** ✅ Easy

### Changes needed

- replace `sigslot::signal< Args... >` with `stellyra::Event< Args... >`
- replace `signal.connect( &Receiver::slot, receiver )` with `event.connect( receiver, &Receiver::slot )` - note the argument order flips: sigslot takes the member-function pointer first and the object pointer second for every `connect()`/`disconnect()` overload, Stellyra takes the receiver first
- receivers: sigslot has two separate, unrelated non-invasive-ish tracking mechanisms, not one combined pattern - (1) inherit `sigslot::observer`/`sigslot::observer_st`, or (2) manage the receiver via `std::shared_ptr`/`std::weak_ptr` with no inheritance at all, which sigslot auto-detects through an ADL `to_weak()` customization point.  Stellyra's `stellyra::Trackable` base class maps to (1); its non-inheriting `Tracked<T>`/`Anchor` pattern is the closer match for (2)'s "no inheritance needed" property, but - unlike (2) - doesn't require the receiver to be `shared_ptr`-managed at all
- `scoped_connection` maps directly to `ScopedConnection`

### Connection syntax comparison

```cpp
// sigslot - member-function pointer first, object pointer second
sigslot::signal< int > sig;
sig.connect( &Receiver::onValue, &receiver );
sig( 42 );
sig.disconnect( &Receiver::onValue, &receiver );  // disconnect one slot
sig.disconnect( &receiver );                      // disconnect everything for this receiver

// Stellyra - receiver first, member-function pointer second
stellyra::Event< int > event{ this };
event.connect( receiver, &Receiver::onValue );
event( 42 );
event.disconnect( receiver, &Receiver::onValue );  // disconnect one slot
event.disconnect( receiver );                      // disconnect everything - only if receiver inherits Trackable;
                                                   // with the non-invasive Tracked<T> pattern, disconnect via the anchor instead
```

### Features gained

- cross-thread deferred connections and EventLoop support
- return values with combiners
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- partial argument matching - sigslot requires an exact signature match with no mechanism for this at all; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via `connectFree<&Func>()` (not for the pointer-value `connectFree(funcPtr)` form, and not for lambdas, both of which still require an exact match)
- connection blocking
- event recording and replay
- EventInspector with Graphviz export
- sender-side emission deferral

### Features lost / changed

- one-line bulk disconnect by receiver - sigslot's `sig.disconnect(&receiver)` works for any receiver; Stellyra's equivalent (`disconnect(Trackable&)`) only works directly when the receiver inherits `Trackable` - with the non-invasive `Tracked<T>` pattern, bulk disconnect goes through the anchor object instead of the receiver itself
- license changes from MIT to BSD 3-Clause - both are permissive with no copyleft obligation, so this is a lateral move, not a functional change

---

## From vdk-signals

**Difficulty:** ✅ Easy

### Changes needed

- replace `vdk::signal< void( ArgTs... ) >` with `stellyra::Event< ArgTs... >`
- replace `vdk::context` with `stellyra::Trackable` as the base class; unlike `Trackable`, `vdk::context` has a protected constructor/destructor and cannot be instantiated on its own, not even as a composed member - `signal::connect()`'s free-callable overload is `static_assert`-enforced to require a type that inherits from `context` (`is_base_of_v<context, Ty>`), so the "separate context for a free callable/lambda" pattern always means a small user-defined class derived from `context`, never a bare `vdk::context` instance - Stellyra's non-inheriting `Tracked<T>`/`Anchor` pattern (a real stand-alone `Trackable` member, needing no derived wrapper class) has no equivalent in vdk-signals
- no `shared_ptr` wrapping required for receivers - ordinary C++ object lifetime management applies
- replace emission calls with `event(args)` (or `event.trigger(args)` / `event.emit(args)`)

### Connection syntax

```cpp
// vdk-signals - receiver inherits vdk::context
Receiver receiver;
sig.connect( &receiver, &Receiver::onValue );
sig( 42 );  // or sig.emit( 42 )

// vdk-signals - a free callable/lambda tracked via a context;
// vdk::context itself can't be instantiated (protected ctor), so this
// requires a small derived class, not a bare vdk::context member
class SlotContext : public vdk::context {};
SlotContext ctx;
sig.connect( &ctx, [&receiver]( int val ) { receiver.onValue( val ); } );

// Stellyra - receiver inherits stellyra::Trackable
stellyra::Event< int > event{ this };
event.connect( receiver, &Receiver::onValue );
event( 42 );  // or event.emit( 42 );

// Stellyra - syntax with a separate Trackable - no derived wrapper class needed
stellyra::Trackable anchor;
event.connect( { receiver, anchor }, &Receiver::onValue );
```

### Thread affinity model

vdk-signals' `vdk::context` automatically binds an object to its *creating* thread - any slot on a context object is serialized to that thread with no manual dispatch code, and cross-thread calls are queued per-thread and drained by explicitly calling `signals_execute()`.  This is implicit and receiver-side, closer to Qt's model than to Stellyra's: Stellyra has no automatic "home thread" for a receiver - a connection's dispatch target is whichever `EventLoop` you explicitly assign via `Deferred`/`Auto` connection types, not something the receiver gets automatically just by existing.

### Features gained

- return values and combiners (10+ built-in, vs vdk-signals' none)
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- connection blocking
- check if connected (`isConnected()`)
- event recording and replay
- EventInspector with Graphviz export
- partial argument matching - vdk-signals requires an exact signature match with no mechanism for this at all; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via `connectFree<&Func>()` (not for the pointer-value `connectFree(funcPtr)` form, and not for lambdas, both of which still require an exact match)
- sender-side emission deferral

### Features lost / changed

- license changes from Apache 2.0 to BSD 3-Clause - both are permissive with no copyleft obligation, so this is a lateral move, not a functional change
- automatic thread affinity - vdk-signals' `vdk::context` gives every object a home thread automatically, with zero setup; Stellyra's equivalent (`Deferred`/`Auto` connections routed to an explicitly-assigned `EventLoop`) requires creating and assigning that loop yourself
- lock-free emission - vdk-signals' emission path is lock-free via atomics by default; Stellyra's `Event` uses a real mutex (`RecursiveMutex` by default) - `BasicConcurrentEvent` (lock-free, epoch-based reclamation) is the closer match if that matters for your use case

---

## From rocket

**Difficulty:** ⚠️ Medium

### Changes needed

- replace `rocket::signal< R( Args... ), Collector, ThreadingPolicy >` with `stellyra::Event< Args... >`
- replace `rocket::trackable` base class with `stellyra::Trackable` (or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>` - rocket's receiver inheritance is optional already, but its non-invasive path is manual `scoped_connection` management, not automatic disconnect)
- no `shared_ptr` wrapping required for receivers - ordinary C++ object lifetime management applies
- replace emission calls with `event(args)` (or `event.trigger(args)` / `event.emit(args)`)
- rocket's `queued_connection` dispatches on the thread that called `connect()` (connection-site affinity) - re-map this to Stellyra's sender/receiver `EventLoop` model, which dispatches based on explicit loop assignment rather than where `connect()` happened to be called (note that `queued_connection` is disabled by default in rocket's shipped header (see "Features lost / changed" below) - this re-mapping only matters if the project has already worked around that to use it)

### Connection syntax

```cpp
// rocket
rocket::signal< void( int ) > sig;
sig.connect( &receiver, &Receiver::onValue );
sig( 42 );

// Stellyra
stellyra::Event< int > event{ this };
event.connect( receiver, &Receiver::onValue );
event( 42 );
```

### Features gained

- automatic receiver cleanup without inheritance - rocket's non-invasive path still requires manual `scoped_connection` management; Stellyra's `Tracked<T>` pattern gives full automatic disconnect-on-destruction without inheriting anything
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- connection blocking - rocket's `block()`/`unblock()`/`is_blocked()` features exist but are disabled by default (`ROCKET_NO_BLOCKING_CONNECTIONS` is self-defined in the shipped header with no supported way to opt back in short of hand-editing it); Stellyra's blocking works out of the box
- check if connected (`isConnected()`)
- event recording and replay
- EventInspector with Graphviz export
- partial argument matching - rocket requires an exact signature match with no mechanism for this at all; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via `connectFree<&Func>()` (not for the pointer-value `connectFree(funcPtr)` form, and not for lambdas, both of which still require an exact match)
- sender-side emission deferral
- 10+ built-in named combiners (vs rocket's 5: `first`, `last`, `minimum`, `maximum`, `range`)

### Features lost / changed

- emission abort - a rocket slot can call `rocket::abort_emission()` to stop the emission mid-flight, preventing any later-registered slot from running. Stellyra has no equivalent: a handler cannot halt dispatch to the remaining connections on the same trigger
- `current_connection()` access - a rocket slot can inspect or disconnect its own connection from inside its own body; Stellyra has no equivalent built-in access to "my own `Connection`" from within a handler
- built-in timers - rocket's `set_interval()`/`set_timeout()` schedule periodic or one-shot callbacks on the current thread's dispatch queue, but like connection blocking above, this feature is disabled by default in the shipped header (`ROCKET_NO_TIMERS`) with no supported opt-in - using it means maintaining a hand-patched fork of `rocket.hpp`, not consuming it as-is; Stellyra has no timer/scheduling facility either way - this needs an external mechanism (a thread, a platform timer API, etc.) if you rely on it
- call-time combiner override - rocket lets you override the combiner policy per call, on top of the one declared at signal-declaration time; Stellyra's combiner is fixed as part of the `CombiningEvent`'s declared type; there's no per-call override
- public-domain flexibility - Stellyra is BSD 3-Clause, which requires retaining the copyright notice; rocket's public-domain (dual-licensed permissive) model has no such requirement, though both are free for commercial use with no copyleft obligation

---

## From nod

**Difficulty:** ✅ Easy

### Changes needed

- replace `nod::signal< void( Args... ) >` with `stellyra::Event< Args... >`
- `nod::signal::connect()` takes a single callable matching the signal's exact signature (`std::function<R(A...)>`) - there's no overload taking a receiver object and a member-function pointer as a pair, so existing call sites already wrap member functions in `std::bind` or a lambda.  Migration replaces that wrapped callable with a direct `event.connect( receiver, &Receiver::method )` call, dropping the bind/lambda wrapper entirely
- add receiver lifetime tracking - nod has no base class and no auto-disconnect mechanism at all; every connection must be manually disconnected or wrapped in nod's own `scoped_connection`. Inherit `stellyra::Trackable` (simplest) or pair a non-inheriting receiver with a standalone `Trackable`/`Anchor` via `Tracked<T>` to get automatic disconnect-on-destruction, something nod never provided
- replace emission calls (nod is `operator()`-only) with `event(args)` (or `event.trigger(args)` / `event.emit(args)`)
- `nod::scoped_connection` maps directly to `ScopedConnection`

### Connection syntax

```cpp
// nod - no connect(object, &Method) overload; the slot must already be a
// bound callable matching the signal's full signature
nod::signal< void( int ) > sig;
sig.connect( std::bind( &Receiver::onValue, &receiver, std::placeholders::_1 ) );
// equivalently: sig.connect( [&receiver]( int v ) { receiver.onValue( v ); } );
sig( 42 );

// Stellyra - connects the receiver and member function directly, no bind/lambda needed
stellyra::Event< int > event{ this };
event.connect( receiver, &Receiver::onValue );
event( 42 );
```

### Features gained

- automatic receiver cleanup - nod requires manual disconnect always; Stellyra's `Trackable`/`Tracked<T>` gives unconditional automatic cleanup
- disconnect by receiver - nod has no receiver tracking, so it can't disconnect by receiver at all; only by connection handle
- move-only lambda captures - nod's handler storage is `std::function`-based, so captures must be copyable; Stellyra has no such restriction
- conditional connections (via `ConnParams`/`.when()`)
- single-shot connections
- priority ordering
- connection blocking
- check if connected (`isConnected()`)
- cross-thread deferred connections and EventLoop support
- event recording and replay
- EventInspector with Graphviz export
- partial argument matching - nod requires an exact signature match with no mechanism for this at all; Stellyra resolves it automatically at compile time for member-function connections and for free functions connected via `connectFree<&Func>()` (not for the pointer-value `connectFree(funcPtr)` form, and not for lambdas, both of which still require an exact match)
- 10+ built-in named combiners (vs nod's none built-in)

### Features lost / changed

- flexible per-call return-value collection - nod's `accumulate(init, op)` and `aggregate<Container>()` let you choose the combiner (and, for `aggregate()`, the container type) at the call site with no declared policy type on the signal at all. Stellyra's combiner is baked into the `CombiningEvent`'s declared type at compile time - more built-in combiners to choose from (10+ named ones vs nod's none), but less flexibility to vary the combiner call-to-call on the same event.
- license changes from MIT to BSD 3-Clause - both are permissive with no copyleft obligation, so this is a lateral move, not a functional change
