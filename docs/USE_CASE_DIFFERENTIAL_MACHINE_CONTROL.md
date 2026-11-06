# Use Case: Differential Machine Control via Deferred Event Loops

## Problem

Traditional real-time machine control systems typically use a **Cyclic Executive (Sense-Plan-Act)** architecture.  A periodic hardware tick (e.g., every 10 ms) triggers a task that reads all inputs, runs all processing logic, then writes all outputs - unconditionally, every cycle.

While deterministic, this "process everything" model has real costs as systems grow:

- CPU cycles are spent recalculating state even when no physical inputs have changed
- planning logic becomes polluted with change-detection boilerplate (`if (current != last)`)
- the Sense, Plan, and Act concerns become entangled as logic grows, making the codebase harder to maintain
- idle machines (steady state, no motion) consume the same resources as fully active ones
- hardware-dependent code is difficult to test off-target; reproducing a field failure requires physically recreating the hardware conditions that triggered it

## How Stellyra Helps

Stellyra supports running multiple event loops on the same thread.  When loops are run sequentially within a periodic tick, deferred events produced by one loop are available to the next by the time it executes.  This makes it practical to restructure the cyclic executive as a **differential pipeline** - one that only propagates work downstream when inputs actually change.

The three-phase structure below is an **architectural pattern** built on top of Stellyra's multi-loop capability, not a built-in feature of the library.  The ordering is imposed by the developer; Stellyra provides the mechanism that makes it tractable without threading complexity.

## Pattern: Sense → Plan → Act

The system retains its deterministic execution frame (e.g., a 10 ms hardware tick).  Within each tick, three event loops execute sequentially on the same thread:

```
[10ms Tick Starts]
	  │
	  ▼
 ┌───────────┐
 │  Loop 1   │ ──► SENSE: Scans all hardware inputs.
 │  (Sense)  │     Fires deferred events only on state delta.
 └─────┬─────┘
	  │ (Loop 1 completes; Loop 2 begins)
	  ▼
 ┌───────────┐
 │  Loop 2   │ ──► PLAN: Processes deferred input events.
 │  (Plan)   │     Executes control logic; fires deferred output events.
 └─────┬─────┘
	  │ (Loop 2 completes; Loop 3 begins)
	  ▼
 ┌───────────┐
 │  Loop 3   │ ──► ACT: Processes deferred output events.
 │   (Act)   │     Writes final state to hardware registers or I/O.
 └───────────┘
	  │
 [10ms Tick Ends] ──► CPU enters idle state for remainder of the frame.
```

An important detail: events flow from one loop to the next because sensor objects in the Sense phase are connected (via Stellyra connections) to receivers on Loop 2, and planning objects in the Plan phase are connected to receivers on Loop 3.  Stellyra routes events to whichever loop the receiver is assigned to - the developer's connection wiring determines the flow; the library enforces it.

### Loop 1 - Sense (executes first)

Reads 100% of physical inputs and compares each value against the previous frame's snapshot.  If a change is detected, it pushes a `StateChanged` event into Loop 2's queue via Stellyra's deferred dispatch.  If nothing has changed, Loop 2's queue remains empty.

**Rule:** Loop 1 never executes planning or business logic.  It completes its scan as fast as possible and yields.

### Loop 2 - Plan (executes second)

Wakes only if Loop 1 detected state changes and populated Loop 2's queue.  Executes state machines, kinematics, or control laws only for the components that reported a change.  If a calculation results in a required hardware adjustment, it pushes a deferred event to Loop 3.

If no inputs have changed, Loop 2's queue is empty and it drains instantly without doing any work.

### Loop 3 - Act (executes third)

Drains its queue and writes only to the specific actuators or registers that the Plan phase flagged for update.

If Loop 2 produced no output events, Loop 3 drains instantly.

## What You Get

**Zero-work ticks during steady state.**  When a machine is idle or inputs are unchanged, Loop 1 completes in microseconds, leaves Loop 2 and Loop 3 empty, and the thread yields for the remainder of the frame.  CPU time and thermal headroom are preserved without any special idle logic.

**Clean separation of concerns.**  The Sense layer does not need to know what logic depends on its data.  It broadcasts state changes; the Plan layer decides what to do with them.  Adding a new input or a new control behaviour does not require touching the other phases.

**No change-detection boilerplate.**  The act of firing a deferred event is the structural guarantee that an input changed.  Planning logic does not need tracking variables or conditional guards.

**No threading complexity.**  Because all three loops run sequentially on a single thread, there is no risk of data races, deadlocks, or the need for mutex locking.  Data is inherently safe because execution is strictly cooperative.

---

## Desktop Simulation and Off-Target Testing

Because the control pipeline has no direct knowledge of hardware - it only knows about events - the same code that runs on the target hardware can run on a desktop platform without modification.  Only the objects at the boundary (sensors and actuators) need to be swapped out.

**On desktop:**
- a timer or a simple loop drives the tick, calling `drain()` on each loop in sequence
- sensor objects are replaced by mock objects that generate synthetic state changes
- actuator objects log their commands or feed a visualisation rather than writing to registers

**On hardware:**
- the tick is driven by the hardware interrupt
- sensor objects read real hardware registers
- actuator objects write real hardware registers

The Stellyra event connections, state machines, and control logic are identical in both configurations.  This is dependency injection at the architectural level: the control pipeline is decoupled from its hardware boundary by the event interface.

---

## Recording, Replay, and Fault Reproduction

Stellyra's `RecordableEvent` and `EventInspector` capabilities integrate naturally with this architecture to address one of the hardest problems in machine control: reproducing a field failure deterministically off-target.

### Recording a Hardware Run

Replace the plain `Event` members on sensor objects with `RecordableEvent`:

```cpp
class TemperatureSensor : public stellyra::Trackable
{
private:
	float _last = 0.0f;
public:
	stellyra::RecordableEvent< float > temperatureChanged { this };

	void scan()
	{
		float current = readHardwareRegister();
		if ( current != _last )
		{
			_last = current;
			temperatureChanged( current );
		}
	}
};
```

Enable recording at startup and export after the run:

```cpp
sensor.temperatureChanged.recorder().startRecording();

// ... machine runs ...

sensor.temperatureChanged.recorder().stopRecording();
sensor.temperatureChanged.recorder().exportToCSV( "temperature_run.csv" );
```

The CSV captures every state change with timestamps.  If the machine encounters a fault, the recording up to that point is a complete, self-contained description of the input sequence that led to it.

### Replaying Off-Target

Load the recording on a desktop system with mock hardware boundaries.  Connect the same planning and actuation logic as on the target.  Replay the recorded sensor events:

```cpp
sensor.temperatureChanged.recorder().replay();                // instant replay
sensor.temperatureChanged.recorder().replayWithTiming();      // original timing preserved
sensor.temperatureChanged.recorder().replayWithSpeed( 0.1 );  // 10x slower for analysis
```

The planning and actuation layers respond exactly as they did on hardware - the same events, the same state transitions, the same output commands - but now in a controlled desktop environment where you can attach debuggers, add logging, and inspect state freely.

### Inspecting Connection Topology

During development or commissioning, `EventInspector` provides a live view of which sensors are connected to which planning components, their connection types, and their current state:

```cpp
stellyra::EventInspector< float > inspector( sensor.temperatureChanged );
inspector.dumpConnectionGraph( std::cout );

// export as Graphviz DOT for visual inspection
std::ofstream dot( "sensor_connections.dot" );
dot << inspector.toDotString();
// dot -Tpng sensor_connections.dot -o sensor_connections.png
```

This is particularly useful during system integration, when confirming that all sensor-to-planner and planner-to-actuator connections have been correctly wired by the mediator.

### Performance Statistics

`RecordableEvent` can track emission counts and average timing per sensor channel:

```cpp
sensor.temperatureChanged.enableStatistics();

// ... run a commissioning sequence ...

auto stats = sensor.temperatureChanged.getStatistics();
std::cout << "Emissions: " << stats.emissionCount << "\n";
std::cout << "Avg emit time: " << stats.avgEmitTimeNs << " ns\n";
```

This gives a data-driven basis for the worst-case tick budget calculation described in the Constraints section below.

---

## Constraints and Cautions

**Budget for worst-case load.**  Because work is event-driven, developers must calculate the system's worst-case tick - the scenario where all inputs change simultaneously (e.g. an emergency stop).  The total execution time of all cascading events across all three loops under maximum load must fit comfortably within the tick window.  Use `RecordableEvent` performance statistics during commissioning to measure actual handler execution times and validate the budget.

**Coalesce redundant events where possible.**  If multiple interrelated sensor changes would trigger the same downstream calculation, consider deduplicating them before Loop 2 processes them.  Running the same heavy computation multiple times within a single tick wastes the efficiency gains the pattern is designed to provide.

**No blocking I/O in handlers.**  Because execution is single-threaded, a single blocking call (slow disk write, synchronous network log, etc.) in any handler will stall the entire thread and cause the system to miss its next tick.  Long-running operations must be offloaded outside the core pipeline.  `RecordableEvent` CSV export should happen outside the tick window - for example, on a dedicated logging thread or after the machine has stopped.
