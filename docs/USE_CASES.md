# Stellyra Use Cases

Stellyra is a lightweight, thread-safe C++ event library designed with asynchronous event handling in mind.  Its EventLoop model, object-affinity scheduling, and automatic connection lifetime management make it a natural fit for a wide range of software - from simple single-threaded applications that benefit from asynchronous execution to complex multi-threaded systems where thread-safe cross-component communication is a core requirement.

This document describes the architectural patterns where Stellyra is most effective, specific categories of software that embody those patterns, and situations where a different tool is the better choice.

---

## Architectural Fit

### Synchronous vs Asynchronous Execution

- **Synchronous** - the receiver completes executing before the sender's trigger call returns.  The caller knows handlers have run when the call returns.
- **Asynchronous** - the sender's trigger call returns before the receiver executes.  Handlers are deferred and run later, either on the same thread or a different thread (depending on EventLoop configurations).

Asynchronous execution means the emitter posts an event and returns immediately - the handler runs later, when the event queue is drained.  This separation in time applies even in a single-threaded application: the emitter doesn't wait for the handler, regardless of whether they share a thread.  A manual EventLoop makes this explicit - the same thread emits events during one part of its work, then drains the queue during another, processing only the work that was posted.

This deferred processing provides substantial architectural benefits that have nothing to do with threading:

- **State machine safety** - asynchronous dispatch prevents recursive transitions; a handler that emits another event does not re-enter the emitter
- **Reentrancy resistance** - deferred processing naturally breaks recursive call chains that can cause subtle bugs in synchronous listener systems
- **Isolation from slow or failing handlers** - the triggering operation returns immediately; slow or misbehaving handlers cannot delay the emitter
- **Burst absorption** - a queue smooths spikes in event production; the emitter never stalls waiting for consumers to catch up
- **Shorter critical sections** - control loops, interrupt-adjacent code, and time-sensitive paths can emit events and return without waiting for handler execution

The tradeoffs of asynchronous execution are also real: synchronous dispatch preserves a contiguous call stack (easier to debug), provides immediate consistency (handlers have run by the time the trigger returns), and carries no per-event queueing overhead.  For simple notification with no reentrancy or timing concerns, synchronous dispatch is simpler.

### Coupling: Direct Calls vs Interfaces vs Events

Decoupling components is not binary - there is a spectrum from tight coupling to loose coupling, and different techniques operate at different points on it.

**Direct method calls** between concrete types are the tightest form of coupling: the caller knows the exact type, the method name, and the call timing.

**Interface-based design and dependency injection** decouple the caller from the concrete type - the caller holds a pointer to an abstract interface and calls methods on it.  This is a genuine improvement: components can be substituted, mocked in tests, and configured externally.  It is the right tool for many problems.  However, it still couples the caller to:
- the *interface type* (the caller must know the specific abstract type to declare a pointer or reference to it)
- the method *name* (two interfaces with the same method name create multiple inheritance conflicts)
- the *timing* of the call (the caller decides both when to call and, inseparably, when the receiver's code runs - a direct call permits no gap between the two, where a deferred event does)
- a *result or error* - if the caller needs either an immediate return value or a synchronous way to catch and react to a failure (a thrown exception, an error code), only a direct or interface call provides that channel back; a fire-and-forget event has no way to hand either one back to the sender
- the *existence* of at least one receiver (the caller holds a reference it must obtain from somewhere)
- a *concrete class* - the interface must be implemented by a class or struct; free functions, static functions, and lambdas cannot implement an interface directly, so even implementing a simple observer requires defining a class, instantiating it, and managing its lifetime

**A raw callback (function pointer or `std::function`)** sits between interface-based design and event-driven design: it drops the *interface type* and *concrete class* couplings - a lambda or free function satisfies the signature directly, with no abstract base class or implementing class required.  What it still lacks, though, is everything an event library formalises:
- *one-to-many broadcast* - a single `std::function` member is one callback, not a list; adding a second subscriber means hand-rolling a `std::vector<std::function<...>>` and its own add/remove bookkeeping
- *automatic lifetime safety* - nothing disconnects the callback when the object a captured pointer or reference refers to is destroyed
- *deferred dispatch* - the call is inherently synchronous and direct; there's no built-in way to defer execution to another thread or loop without the callback itself implementing thread deferral and multi-threaded safety

**Event-driven design** decouples further.  The emitter knows only the event's argument types - not the method name, not the receiver type, not whether anyone is listening.  Handlers can be lambdas, free functions, or member functions with any name, as long as the signature is compatible.  One could fairly argue that depending on an event's argument types is still a form of coupling - and it is.  But it is the *minimum possible* contract: the two sides agree on what data is being communicated, nothing more.

The pattern itself also drops two assumptions every form above shares: it doesn't presuppose exactly one receiver, the way a raw callback's single slot does, and it doesn't presuppose synchronous, inline execution, the way every call-based form above does.  But those are removed assumptions, not guarantees.  A naive, hand-rolled observer implementation can have the exact same dangling-callback risk as a raw callback, no thread safety at all, and still require the caller to manage its own subscriber list by hand - "event-driven" alone doesn't fix any of that.

What turns the removed assumptions into real guarantees is the library, not the pattern.  A well-built event library formalises:
- *one-to-many broadcast* without hand-rolled bookkeeping
- *automatic lifetime safety* - connections are tracked and torn down automatically when either side is destroyed, which matters more here than for a raw callback, since broadcast means more receivers that can be destroyed mid-flight, not just one
- *actual deferred dispatch* - a real queue and drain mechanism that lets a connection run later, on another loop or thread, rather than just the theoretical possibility of it
- *thread-safe connection management* - connecting and disconnecting handlers safely from multiple threads, rather than something the caller has to add on top

None of this follows automatically from "using events."  It's the gap between a quality library and a naive one - and it's exactly what to evaluate when choosing an event (e.g. signal/slot) library, event-driven design being decoupled enough to make the choice worth making in the first place.

A **mediator** (sometimes called a setup or broker class) owns the wiring between components.  Neither the emitter nor the receiver knows the other exists at compile time.  The mediator connects them at startup and then steps out of the way:

```
Component A  ──events──►  Mediator  ──connects──►  Component B handlers
Component B  ──events──►  Mediator  ──connects──►  Component A handlers
```

Neither component has a dependency on the other.  The mediator is the only class with knowledge of both, and it exists solely to make connections.  This makes components independently testable - connect a test handler to observe behaviour without constructing the full system.

### Where Stellyra Fits

Stellyra implements asynchronous event-driven communication with explicit ownership semantics.  Its EventLoop model maps naturally onto layered architectures: each layer's objects can have their own EventLoop, guaranteeing that internal state is always accessed from the correct context.  Cross-layer communication via Deferred connections respects thread ownership without requiring explicit locking.

A system can start with all objects on a single manual EventLoop - deterministic, debuggable, no threading complexity - and later distribute work across threads by assigning EventLoops to objects, without changing any of the event connection logic.  The mediator's wiring code remains identical regardless of the threading model.

---

## Strong Fits

### Desktop Applications

Complex desktop applications - CAD software, IDEs, scientific tools, medical imaging, financial workstations, audio/video tools - naturally evolve into a layered topology:

```
UI → Service Layer → Worker Threads → Hardware / Network
```

The complexity in these applications comes from managing state ownership across threads.  Stellyra's object-affinity model allows components like a `DocumentManager`, `Renderer`, or `NetworkClient` to be scheduled safely within their designated execution contexts.  The event-driven communication model eliminates callback chains and makes code easier to navigate and unit test.  RAII-driven automatic connection cleanup means short-lived views and plugin tabs disconnect themselves safely when destroyed - no dangling callbacks, no use-after-free.

### Medical Device Software

Layered medical device software - patient monitors, infusion pumps, diagnostic instruments, surgical robotics - typically separates a hardware abstraction layer (HAL) from the clinical logic built on top of it:

```
Sensors / Actuators → HAL (drivers, register access, timing-critical I/O) → Device State Machine → Alarm Management / UI / Logging / Communication Stack
```

The HAL is the wrong place for events, and the right place for direct calls or interface-based design: driver code typically has one fixed caller, a deterministic timing budget, and a call path that needs to be traceable line-for-line during safety-certification review (IEC 62304, IEC 61508, and DO-178C reviewers all expect this) - the tight coupling direct calls provide is exactly what's wanted there, not a liability to engineer away.

Everything above the HAL has the opposite problem.  Alarm management, logging, a communication stack, and UI all need to react when a sensor reading changes, a state transition occurs, or an alarm condition is met - without the device's core state machine needing to know how many of them exist, or coupling its own correctness to each one's implementation.  Forcing that relationship through direct calls instead produces the tightly-coupled web described under "Coupling: Direct Calls vs Interfaces vs Events" above, where the state machine ends up depending on every consumer of its state rather than the other way around.  Event-driven design is the right fit for exactly this layer: the "State machine safety" and "Reentrancy resistance" benefits described under "Synchronous vs Asynchronous Execution" above apply directly to a device's core state machine, and a single manual EventLoop keeps the fully deterministic, reproducible dispatch order that verification and validation testing requires (see "Where Stellyra Fits" above).

This is the same architectural split used in Desktop Applications (above) and Robotics and Control Systems (below): tight, direct coupling at the hardware boundary, loose, event-driven coupling everywhere above it.

### Simulation Systems

Simulations - traffic, factory, robotics, aerospace, discrete-event systems - are inherently event-driven and benefit from loose coupling between subsystems like physics, AI, and rendering loops.

The single-to-multi-threaded migration path described above makes single-threaded execution a first-class option here, not a compromise: fully deterministic for debugging and testing, with no rewrite needed when scaling up to real threads later.

### Robotics and Control Systems

Industrial automation, autonomous vehicles, drone software, lab equipment, and manufacturing systems typically flow from sensors through filtering and decision logic to actuators.  Each stage naturally becomes an event-producing object.

Hardware controllers like a `MotorController` require exactly one execution context.  Stellyra's sender thread affinity guarantees that critical commands like `MoveCommand` and `EmergencyStop` are serialised through the object's EventLoop, avoiding race conditions without manual locking boilerplate.

For a detailed architectural pattern covering differential (change-driven) machine control, desktop simulation portability, and hardware fault reproduction via `RecordableEvent`, see [`USE_CASE_DIFFERENTIAL_MACHINE_CONTROL.md`](USE_CASE_DIFFERENTIAL_MACHINE_CONTROL.md).

### Hardware-in-the-Loop (HIL) Testing

Engineering rigs that interface physical hardware with real-time simulated environments - common in automotive and aerospace - require coordination between a simulated master clock and asynchronous hardware interrupts.

Manually driving an EventLoop (`drain()`) lets developers step through hardware simulations frame-by-frame during testing, then move to production over the same migration path described above, without touching the handler logic itself - only how the EventLoop is driven (manual `drain()` calls during testing, versus assigning it to a dedicated thread for production) changes between the two.

### Plugin Architectures and Modular Monoliths

Teams that reject the overhead of microservices or distributed actors but still need internal modularity can use Stellyra to structure a large process as discrete subsystems that interact purely through explicit event boundaries.  When plugins are dynamically loaded or unloaded, automatic lifetime tracking prevents dangling connections across the shared infrastructure.

### Scientific and Engineering Software

Data acquisition, instrument control, analysis pipelines, and visualisation systems often run hardware, processing, visualisation, and storage concurrently.  Stellyra's EventLoop ownership model makes reasoning about which thread owns which state substantially clearer than shared mutexes.

### Financial Strategy Platforms

Quantitative trading systems require strict separation between historical back-testing and live execution.  Back-testing must run single-threaded for 100% reproducible execution ordering.  Stellyra allows strategy and risk-management objects to be written once, run single-threaded for historical validation, and spread across real-time threads in live production without touching that logic itself - only the EventLoop wiring (which thread drains which loop) differs between the two modes.

### Workflow and Job Engines

Build systems, job schedulers, automation frameworks, and ETL pipelines map naturally to event-driven coordination: `TaskStarted`, `TaskFinished`, `TaskFailed`, `DependencySatisfied`.

### Embedded Applications (Soft Real-Time)

State-machine-driven embedded systems on Linux-based platforms - automotive infotainment, smart appliances, laboratory instruments - benefit from Stellyra's `SingleThreadedEvent<Args...>` alias (`NullMutex`), which eliminates mutex overhead entirely on a per-event basis, while a manually-drained `EventLoop` (`drain()`) integrates cleanly into existing main loops.

---

## Reasonable Fits with Caveats

### Network Services

Services composed of cooperating subsystems - `SessionManager`, `AuthenticationService`, `Cache`, `MetricsCollector` - where the primary complexity is coordination rather than throughput are a good fit.  Stellyra would typically sit *above* the networking layer (Asio, libuv, etc.) rather than replace it.  Ultra-high-throughput networking built around `epoll`, `io_uring`, or lock-free queues is better served by those specialised tools directly.

### Media Processing Pipelines

Audio processing, video processing, and streaming systems map naturally to pipeline stages where each stage emits events.  The `Direct` vs `Deferred` distinction can tune performance while maintaining clean interfaces.  When filtering with `ConnParams`/`.when()`, make sure to choose the correct predicate context for what's being checked - see `BEST_PRACTICES.md` for guidance.

### Game Engine Subsystems

High-level subsystems - asset management, audio, networking, AI services, tooling - are a reasonable fit.  For core gameplay logic processing millions of events per frame, data-oriented entity component system (ECS) dispatchers (EnTT, Flecs) are better choices due to their cache-efficient contiguous memory layouts.  Stellyra's connection model stores handlers in a per-event `std::vector<HandlerEntry>` behind a type-erased `Callable` layer (heap-allocated for capturing lambdas) rather than a contiguous, type-specific layout, which incurs cache-miss and indirect-call overhead unsuitable for inner gameplay loops even with `SingleThreadedEvent`.

---

## Not the Right Tool

### Pure Callback Notification

If an object simply needs an `onChanged()` notifier with no threading, lifetime management, or event loop requirements, Stellyra's full machinery may be overkill.  Libraries such as sigslot and nano-signal-slot are lighter choices for simple local notification; Boost.Signals2 is a comparable alternative if you are already using Boost.

### High-Throughput Async I/O

Stellyra does not manage kernel multiplexing, raw socket buffers, or async I/O scheduling.  It is not an alternative to `Asio`, `libuv`, or `io_uring` - it is complementary.  Let the I/O framework pull bytes off the wire; let Stellyra coordinate the stateful application objects that process them.

### Massive-Scale Actor Systems

Applications requiring hundreds of thousands of ephemeral actors, distributed cluster routing, supervision trees, or multi-node deployment are better served by dedicated actor frameworks such as CAF (C++ Actor Framework).

### Deep Bare-Metal Microcontrollers

Stellyra’s bare-metal mode (STELLYRA_THREAD_SAFE=0) is in fact built for exactly this class of target - it exists so Stellyra can compile where <mutex>, <shared_mutex>, and <atomic> aren’t available, and its volatile-based fallback is explicitly documented as safe on single-core bare-metal (multi-core bare-metal/RTOS targets need a custom Atomic<T> specialisation).  What the flag doesn’t touch is the connection storage model itself - std::vector<HandlerEntry> and heap-allocated lambda captures are unaffected by STELLYRA_THREAD_SAFE, so the fragmentation risk above remains regardless of which value it’s set to.
