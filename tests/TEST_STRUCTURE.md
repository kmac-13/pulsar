# Stellyra Unit Tests

## Test Files

| File | Tests | Coverage |
|---|---|---|
| `test_helpers.hpp` | - | Shared helpers: `TestButton`, `TestHandler`, `TestValidator`, `DataSender`, `DataReceiver`, `RelayNode`, `Checker`, static free function handlers |
| `test_basic_connections.cpp` | 8 | Basic connections, lambdas, manual and auto-disconnect, disconnect by receiver |
| `test_advanced_connections.cpp` | 8 | Single-shot, priority ordering, FIFO for equal priority, conditional connections, blocking |
| `test_combining_and_groups.cpp` | 7 | `CombiningEvent` (LogicalAnd, LogicalOr), `ScopedConnection`, `ConnectionGroup` (block/unblock/disconnect) |
| `test_free_functions.cpp` | 8 | Free functions, static functions, `connectFree`/`connectOnceFree`, disconnect by pointer, mixed connection types |
| `test_event_loops.cpp` | 6 | Deferred connections, connection info inspection, loop migration, manual `processEvents`, multiple loops |
| `test_external_event_loop.cpp` | 7 | Manually-processed `EventLoop`: deferred-until-drained, same-loop Direct resolution, multi-event drain, migration preserving pending events, `start`/`stop` no-ops |
| `test_thread_safety.cpp` | 5 | Concurrent emit/disconnect, cross-thread emission, rapid connect/disconnect, concurrent blocking, emit/disconnect race condition |
| `test_edge_cases.cpp` | 9 | Event forwarding, chained forwarding, empty emission, self-disconnection during emission, recursive emission, double disconnect, use after disconnect, handler destruction during emission, null sender |
| `test_sender_deferral.cpp` | 7 | No-loop direct execution, manual loop deferral, emit from inside drain, auto-processed loop deferral from thread, two loops on one thread, deferral with Deferred receiver, owned-loop serialisation |
| `test_recordable_event.cpp` | 9 | Record/replay, recording limit, pause/resume, stop clears recording, `replayWithSpeed`, timing stats, CSV export, `dumpRecordings`, performance statistics |
| `test_coverage_gaps.cpp` | 21 | `PrivateEvent` (connect/disconnect/auto-disconnect/inspector), `EventInspector` (dump/graph/summary/DOT/connection info), `ConnectionGuard`, `ConnectionGroup` (cleanup/has-active/release/range-for/indexed), `ScopedConnection::release`, `disconnectAndSetEventLoop`, move-only captures on Deferred connections, `connectIf` with member function condition |

**Total: 95 tests across 11 binaries**

---

## Building and Running

```bash
cmake -B build
cmake --build build
cd build && ctest

# Run a specific test binary
./test_basic_connections
./test_coverage_gaps
```

---

## Adding New Tests

Add to the appropriate file based on the coverage table above.  Use the helpers from `test_helpers.hpp`:

```cpp
TEST( BasicConnections, NewFeature )
{
    auto button = std::make_shared< TestButton >();
    auto handler = std::make_shared< TestHandler >();

    button->clicked.connect( handler, &TestHandler::onClicked );
    button->click( 1, 1 );

    EXPECT_EQ( handler->callCount, 1 );
}
```
