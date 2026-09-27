#!/usr/bin/env python3
"""
compare.py -- Pulsar benchmark result aggregator

Reads Google Benchmark JSON output files produced by the per-library
executables and emits a Markdown comparison table to stdout.

Usage
-----
    # Run each benchmark executable with JSON output:
    bench_pulsar_ts.exe  --benchmark_format=json --benchmark_out=results/pulsar_ts.json
    bench_pulsar_st.exe  --benchmark_format=json --benchmark_out=results/pulsar_st.json
    bench_nano_st.exe    --benchmark_format=json --benchmark_out=results/nano_st.json
    bench_nano_ts.exe    --benchmark_format=json --benchmark_out=results/nano_ts.json
    bench_sigslot_st.exe --benchmark_format=json --benchmark_out=results/sigslot_st.json
    bench_sigslot_mt.exe --benchmark_format=json --benchmark_out=results/sigslot_mt.json
    bench_rocket_st.exe  --benchmark_format=json --benchmark_out=results/rocket_st.json
    bench_rocket_ts.exe  --benchmark_format=json --benchmark_out=results/rocket_ts.json
    bench_nod.exe        --benchmark_format=json --benchmark_out=results/nod.json
    bench_vdk_st.exe     --benchmark_format=json --benchmark_out=results/vdk_st.json
    bench_vdk_ts.exe     --benchmark_format=json --benchmark_out=results/vdk_ts.json
    bench_qt.exe         --benchmark_format=json --benchmark_out=results/qt.json
    # (bench_libsigcpp.exe and bench_boost.exe if available)

    # Aggregate:
    python compare.py results/ -o BENCHMARK_RESULTS.md
    # (writes UTF-8 directly; avoids shell redirection encoding issues -
    # see the -o handling in main() for details. `python compare.py
    # results/ > BENCHMARK_RESULTS.md` still works but is not recommended
    # on Windows/PowerShell, where `>` redirection can re-encode output.)

Output
------
A Markdown file with one table per scenario group.  Times are reported in
nanoseconds per iteration (ns/iter), taken from the benchmark's real_time
field (wall-clock time), not cpu_time -- see load_results()'s docstring for
why.
"""

import json
import os
import re
import sys

# Windows' default console/redirect encoding (cp1252) can't encode arbitrary
# Unicode.  Reconfigure stdout to UTF-8 so redirected output (e.g.
# `> BENCHMARK_RESULTS.md`) doesn't crash on non-ASCII characters.  Available
# since Python 3.7; harmless no-op on platforms where stdout is already UTF-8.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Library display names and the JSON files that feed them.
# Order controls column order in the output table.
# ---------------------------------------------------------------------------

LIBRARIES = [
    # (display_name,          json_filename,        note)
    ("Pulsar ST",             "pulsar_st.json",      "no locking"),
    ("Pulsar TS",             "pulsar_ts.json",      ""),
    ("Qt6",                   ["qt_1.json", "qt_2.json", "qt_3.json", "qt_4.json"],
     "split across four processes (one per benchmark cluster) -- see "
     "run_benchmarks.sh's comment on why Qt's split mirrors vdk's, without "
     "vdk's confirmed thread_local root cause"),
    ("nano ST",               "nano_st.json",        ""),
    ("nano TS",               ["nano_ts_1.json", "nano_ts_2.json"],
     "split into non-concurrent (_1) and concurrent (_2) processes"),
    ("sigslot ST",            "sigslot_st.json",     ""),
    ("sigslot MT",            ["sigslot_mt_1.json", "sigslot_mt_2.json"],
     "split into non-concurrent (_1) and concurrent (_2) processes"),
    ("rocket ST",             "rocket_st.json",      ""),
    ("rocket TS",             ["rocket_ts_1.json", "rocket_ts_2.json"],
     "thread-safe policy protects connection list only, not handler "
     "execution; split into non-concurrent (_1) and concurrent (_2) processes"),
    ("nod",                   ["nod_1.json", "nod_2.json"],
     "split into non-concurrent (_1) and concurrent (_2) processes"),
    ("vdk ST",                "vdk_st.json",         "vdk::lite"),
    ("vdk TS",                ["vdk_ts_1.json", "vdk_ts_crossthread.json",
                                "vdk_ts_threadaffinity.json", "vdk_ts_direct.json",
                                "vdk_ts_perreceiver.json", "vdk_ts_contention.json"],
     "split across multiple processes -- see run_benchmarks.sh's comment on "
     "the MinGW thread_local destructor issue this works around"),
    ("libsigc++",             "libsigcpp.json",      "optional"),
    ("Boost.Signals2",        "boost.json",          "optional"),
]

# ---------------------------------------------------------------------------
# Scenario groups: (heading, [benchmark_name_prefix, ...])
# benchmark_name_prefix must match the BM_ function name in the source.
# ---------------------------------------------------------------------------

# ----------------------------------------------------------------------------
# Feature checklist -- static, hand-curated reference data, not derived from
# any benchmark JSON. Verified directly against each library's source during
# this project's investigation (see project history for specifics -- e.g.
# nod's lack of tracking, rocket's/nod's connection-list-only thread safety,
# vdk's non-header-only build).
#
# Deliberately limited to features checked directly against source rather
# than everything a library might plausibly support -- e.g. free-function
# and lambda connections are omitted here even though most of these
# libraries likely support them, because that wasn't independently verified
# with the same rigor as the rows below.
# ----------------------------------------------------------------------------

FEATURE_LIBRARIES = [
    "Pulsar", "Qt6", "nano-signal-slot", "sigslot", "rocket", "nod", "vdk-signals",
]

FEATURES = [
    (
        "Header-only (no separate compilation step)",
        {"Pulsar": "Yes", "Qt6": "No (moc + link against Qt Core)",
         "nano-signal-slot": "Yes", "sigslot": "Yes", "rocket": "Yes", "nod": "Yes",
         "vdk-signals": "No (signals.cpp must be compiled)"},
    ),
    (
        "Automatic receiver-lifetime tracking (destroying the receiver auto-disconnects it)",
        {"Pulsar": "Yes, opt-in (inherit Trackable)",
         "Qt6": "Yes (built into QObject)",
         "nano-signal-slot": "Yes, opt-in (inherit Observer)",
         "sigslot": "Yes, opt-in (inherit observer_base)",
         "rocket": "Yes, opt-in (inherit trackable)",
         "nod": "No -- only scoped_connection, tied to the connection handle's own lifetime, not the receiver's",
         "vdk-signals": "Yes, opt-in (inherit context)"},
    ),
    (
        "Thread-safe connect()/disconnect() (safe to mutate the connection list from another thread during dispatch)",
        {"Pulsar": "Yes (Event/SharedEvent)", "Qt6": "Yes",
         "nano-signal-slot": "Yes (TS_Policy)", "sigslot": "Yes (SIGSLOT_THREAD_SAFE)",
         "rocket": "Yes (thread_safe_policy)", "nod": "Yes (multithread_policy, default)",
         "vdk-signals": "Yes"},
    ),
    (
        "Concurrent dispatch is fully serialised (vs. only connection-list-safe, allowing handlers to run in parallel)",
        {"Pulsar": "Depends on event type: Event=serialised, SharedEvent=parallel",
         "Qt6": "DirectConnection=parallel (no locking at all); QueuedConnection=serialised (one target queue)",
         "nano-signal-slot": "Yes, fully serialised (TS_Policy holds its lock for the whole handler loop)",
         "sigslot": "Yes, fully serialised",
         "rocket": "No -- thread_safe_policy protects the connection list only, not handler execution",
         "nod": "No -- multithread_policy protects the connection list only, not handler execution",
         "vdk-signals": "No for exec::sync -- lock-free, no serialisation of handler execution at all"},
    ),
    (
        "Receiver-deferred dispatch (queue delivery to the receiver's own thread/loop)",
        {"Pulsar": "Yes, native (ConnectionType::Deferred)",
         "Qt6": "Yes, native (Qt::QueuedConnection)",
         "nano-signal-slot": "No", "sigslot": "No", "rocket": "No", "nod": "No",
         "vdk-signals": "Yes, native (per-thread channel model)"},
    ),
    (
        "Sender-deferred dispatch (queue delivery to the sender's own thread, regardless of receiver)",
        {"Pulsar": "Yes, native -- unique among the libraries tested here",
         "Qt6": "No native primitive (workarounds exist -- see Scenario 6)",
         "nano-signal-slot": "No", "sigslot": "No", "rocket": "No", "nod": "No",
         "vdk-signals": "No"},
    ),
    (
        "Automatic connection-type resolution (direct if same-thread, queued if cross-thread, chosen automatically)",
        {"Pulsar": "Yes (ConnectionType::Auto, the default)",
         "Qt6": "Yes (Qt::AutoConnection, the default)",
         "nano-signal-slot": "No -- no thread-affinity concept at all",
         "sigslot": "No -- no thread-affinity concept at all",
         "rocket": "No -- no thread-affinity concept at all",
         "nod": "No -- no thread-affinity concept at all",
         "vdk-signals": "Yes (default exec{}, same-thread=sync/cross-thread=queued)"},
    ),
    (
        "Framework / external dependency",
        {"Pulsar": "None (standalone)", "Qt6": "Full framework (Qt Core minimum)",
         "nano-signal-slot": "None (standalone)", "sigslot": "None (standalone)",
         "rocket": "None (standalone)", "nod": "None (standalone)",
         "vdk-signals": "None (standalone)"},
    ),
]

# ----------------------------------------------------------------------------
# Per-scenario reasons why specific libraries are N/A, shown alongside the
# bare name listing instead of leaving the reader to guess. Deliberately
# NOT exhaustive: only populated where the reason is already independently
# verified elsewhere in this file (the Feature Checklist above, itself
# checked directly against each library's source per its own header
# comment) or stated explicitly in that scenario's own description_lines.
# A library with no entry here for a given scenario still gets listed by
# the N/A line, just without a reason -- silence here means "not yet
# verified why", never "no reason exists". Add entries as reasons get
# confirmed, rather than guessing to fill this out completely.
#
# Only applies to scenarios using the auto-detected N/A path (a plain list
# of benchmark-name strings, not explicit_columns) -- scenarios with
# explicit_columns (7a, 7b, 9, ...) already hand-curate exactly which
# libraries appear and explain the gaps in their own description_lines, so
# this dict is never consulted for them.
# ----------------------------------------------------------------------------

NA_REASONS: Dict[str, Dict[str, str]] = {
    "Scenario 4 - Scoped receiver lifetime": {
        "nod": "no automatic receiver-lifetime tracking -- see Feature Checklist",
    },
    # Scenario 5 moved to explicit_columns (see SCENARIO_GROUPS) so it can
    # show both Pulsar TS (Event) and Pulsar TS (ConcurrentEvent) columns
    # side by side -- explicit_columns scenarios hand-curate their own
    # columns and never consult this dict (see the comment above), so its
    # old entry here was removed rather than left as dead, stale config.
}


SCENARIO_GROUPS = [
    (
        "Scenario 1 - Single-threaded emission, 1 connection",
        [
            "**What this tests:** the cheapest possible operation each library "
            "can do -- one `trigger()`/`emit()` call dispatching synchronously "
            "to exactly one connected handler, no threading, no contention. "
            "The floor every other scenario builds on.",
        ],
        ["BM_Emit_1Connection"],
    ),
    (
        "Scenario 2 - Single-threaded emission, N connections",
        [
            "**What this tests:** the same single-threaded dispatch as "
            "Scenario 1, but with N handlers connected to one event/signal, "
            "at N = 1/10/100/1000 -- shows how dispatch cost scales with "
            "connection-list size alone (no threading involved).",
        ],
        [
            "BM_Emit_NConnections/1",
            "BM_Emit_NConnections/10",
            "BM_Emit_NConnections/100",
            "BM_Emit_NConnections/1000",
        ],
    ),
    (
        "Scenario 2a - Single-threaded emission, 1 Auto connection",
        [
            "**What this tests:** implemented by Pulsar and Qt6, the two "
            "libraries with an auto-resolving connection type. Identical to "
            "Scenario 1, but connecting with `ConnectionType::Auto` (the "
            "default when no type is given) / `Qt::AutoConnection` instead "
            "of an explicit direct type -- confirms Auto correctly resolves "
            "to zero-overhead direct dispatch when sender and receiver "
            "share no distinct thread context. vdk-signals has an "
            "analogous auto-resolving `exec{}` default but no dedicated "
            "benchmark for it in this suite.",
        ],
        ["BM_Emit_1Connection_Auto"],
    ),
    (
        "Scenario 2b - Single-threaded emission, N Auto connections",
        [
            "**What this tests:** the N-connection version of 2a, at the "
            "same N = 1/10/100/1000 as Scenario 2.",
        ],
        [
            "BM_Emit_NConnections_Auto/1",
            "BM_Emit_NConnections_Auto/10",
            "BM_Emit_NConnections_Auto/100",
            "BM_Emit_NConnections_Auto/1000",
        ],
    ),
    (
        "Scenario 2c - Single-threaded emission, 1 connection, syntax comparison",
        [
            "**What this tests:** whether the *syntax* used to connect a "
            "handler has any hidden runtime cost, holding everything else "
            "fixed. \"Emit 1Connection\" is the same baseline as Scenario 1 "
            "for whichever library has it. For Pulsar specifically, that's "
            "a member function passed as a compile-time template argument "
            "via `connect<&Receiver::onFired>(...)`; \"Free\"/\"FreeNTTP\" "
            "compare the free-function equivalent: a runtime function "
            "pointer (`connectFree(fn)`) vs. the same function passed as a "
            "compile-time template argument (`connectFree<&fn>()`). There "
            "is no runtime (non-template) member-function variant in "
            "Pulsar's own comparison here -- that one lives in Scenario 2d "
            "instead, for N connections.",
            "",
            "libsigc++ participates too, on a different axis: it has no "
            "NTTP-style connection syntax at all (every connection is "
            "runtime-wrapped), so its \"Free\" row compares `sigc::mem_fun` "
            "(the baseline) against `sigc::ptr_fun` (free function).",
            "",
            "The \"Lambda\" row is a genuine cross-library comparison, not "
            "a single-library curiosity: Pulsar, Qt, libsigc++, rocket, "
            "nod, and sigslot all take real ownership of a directly-"
            "connected lambda (verified against each one's actual storage "
            "-- rocket's `std::function`-based slot_type, nod's "
            "`_slots.push_back(std::forward<T>(slot))`, sigslot's "
            "`make_slot<slot_t>(std::forward<Callable>(c), ...)`, all move "
            "the callable into internally-owned storage, same as Pulsar's "
            "`connectLambda` and Qt's native lambda-slot support). nano is "
            "a genuine, confirmed exception, not a missing benchmark: its "
            "`connect(L& instance)` takes a reference to an "
            "*externally-owned* functor object rather than storing a copy "
            "-- a temporary lambda would dangle immediately, so there's no "
            "equivalent operation to benchmark for it here. vdk-signals "
            "also genuinely supports this (`connect(Fn slot)`, confirmed "
            "earlier) but doesn't have a benchmark for it yet -- not "
            "included in this pass.",
        ],
        [
            "BM_Emit_1Connection",
            "BM_Emit_1Connection_Free",
            "BM_Emit_1Connection_FreeNTTP",
            "BM_Emit_1Connection_Lambda",
        ],
    ),
    (
        "Scenario 2d - Single-threaded emission, N connections, syntax comparison",
        [
            "**What this tests:** the member-function counterpart to 2c's "
            "free-function comparison -- \"NConnections\" is the "
            "compile-time NTTP form (`connect<&Receiver::onFired>(...)`, "
            "same as Scenario 2); \"RuntimeMethod\" uses Pulsar's other "
            "member-connect overload, where the member pointer is a plain "
            "runtime argument (`connect(receiver, &Receiver::onFired, "
            "...)`) instead of a template parameter.",
        ],
        [
            "BM_Emit_NConnections/1",
            "BM_Emit_NConnections_RuntimeMethod/1",
            "BM_Emit_NConnections/10",
            "BM_Emit_NConnections_RuntimeMethod/10",
            "BM_Emit_NConnections/100",
            "BM_Emit_NConnections_RuntimeMethod/100",
            "BM_Emit_NConnections/1000",
            "BM_Emit_NConnections_RuntimeMethod/1000",
        ],
    ),
    (
        "Scenario 3 - Connect / disconnect throughput",
        [
            "**What this tests:** repeatedly connecting then disconnecting a "
            "handler in a loop -- isolates connection-list bookkeeping cost "
            "from dispatch cost (no `trigger()`/`emit()` calls happen here).",
            "",
            "Pulsar gets two columns per build (ST and TS): **Event** "
            "(`pulsar::Event`, RecursiveMutex-backed -- still Pulsar's "
            "primary/default event type) and **ConcurrentEvent** "
            "(`pulsar::ConcurrentEvent`, regular-Mutex-backed, lock-free "
            "epoch-based dispatch). Under a ST build both variants have "
            "their locking compiled out to a no-op (see platform.h), so any "
            "gap between the two ST columns reflects Event's in-place, "
            "mutex-protected connection list versus ConcurrentEvent's "
            "copy-on-write snapshot rebuild on every connect()/disconnect() "
            "-- not lock contention, which only the TS columns exercise.",
        ],
        [
            {"_display": "ConnectDisconnect",
             "Pulsar ST (Event)":           "BM_ConnectDisconnect",
             "Pulsar ST (ConcurrentEvent)": "BM_ConnectDisconnect_ConcurrentEvent",
             "Pulsar TS (Event)":           "BM_ConnectDisconnect",
             "Pulsar TS (ConcurrentEvent)": "BM_ConnectDisconnect_ConcurrentEvent",
             "Qt6":         "BM_ConnectDisconnect",
             "nano ST":     "BM_ConnectDisconnect",
             "nano TS":     "BM_ConnectDisconnect",
             "sigslot ST":  "BM_ConnectDisconnect",
             "sigslot MT":  "BM_ConnectDisconnect",
             "rocket ST":   "BM_ConnectDisconnect",
             "rocket TS":   "BM_ConnectDisconnect",
             "nod":         "BM_ConnectDisconnect",
             "vdk ST":      "BM_ConnectDisconnect",
             "vdk TS":      "BM_ConnectDisconnect",
             "libsigc++":   "BM_ConnectDisconnect"},
        ],
        [
            ("Pulsar ST (Event)", "Pulsar ST"),
            ("Pulsar ST (ConcurrentEvent)", "Pulsar ST"),
            ("Pulsar TS (Event)", "Pulsar TS"),
            ("Pulsar TS (ConcurrentEvent)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("nano ST", "nano ST"),
            ("nano TS", "nano TS"),
            ("sigslot ST", "sigslot ST"),
            ("sigslot MT", "sigslot MT"),
            ("rocket ST", "rocket ST"),
            ("rocket TS", "rocket TS"),
            ("nod", "nod"),
            ("vdk ST", "vdk ST"),
            ("vdk TS", "vdk TS"),
            ("libsigc++", "libsigc++"),
        ],
    ),
    (
        "Scenario 4 - Scoped receiver lifetime",
        [
            "**What this tests:** a receiver's connection(s) being torn "
            "down automatically when the receiver object goes out of scope "
            "(RAII-style), rather than via an explicit disconnect() call.",
        ],
        ["BM_ScopedReceiverLifetime"],
    ),
    (
        "Scenario 5 - Cross-thread deferred emission",
        [
            "**What this tests:** one thread emits; a second thread (the "
            "receiver's actual owning context) later drains and executes "
            "the handler. Measures the full cross-thread round trip -- "
            "queue, wake, dequeue, invoke -- not just the emitting call.",
            "",
            "*N/A for this scenario: Pulsar ST, nano ST (no native "
            "receiver-deferred dispatch primitive -- see Feature "
            "Checklist), nano TS (no native receiver-deferred dispatch "
            "primitive -- see Feature Checklist), sigslot ST (no native "
            "receiver-deferred dispatch primitive -- see Feature "
            "Checklist), sigslot MT (no native receiver-deferred dispatch "
            "primitive -- see Feature Checklist), rocket ST (no native "
            "receiver-deferred dispatch primitive -- see Feature "
            "Checklist), rocket TS (no native receiver-deferred dispatch "
            "primitive -- see Feature Checklist), nod (no native "
            "receiver-deferred dispatch primitive -- see Feature "
            "Checklist), vdk ST, libsigc++*",
            "",
            "Pulsar's two columns use the same `ConnectionType::Deferred` "
            "+ `EventLoop` + `AutoDrainThread` machinery either way -- only "
            "the sender's event type differs (`Event`, RecursiveMutex, vs. "
            "`ConcurrentEvent`, regular Mutex/lock-free dispatch), "
            "isolating whatever gap remains between the two for this "
            "access pattern.",
        ],
        [
            {"_display": "CrossThreadDeferred",
             "Pulsar TS (Event)":           "BM_CrossThreadDeferred",
             "Pulsar TS (ConcurrentEvent)": "BM_CrossThreadDeferred_ConcurrentEvent",
             "Qt6":    "BM_CrossThreadDeferred",
             "vdk TS": "BM_CrossThreadDeferred"},
        ],
        [
            ("Pulsar TS (Event)", "Pulsar TS"),
            ("Pulsar TS (ConcurrentEvent)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("vdk TS", "vdk TS"),
        ],
    ),
    (
        "Scenario 6 - Thread-affinity forwarding, Pulsar vs Qt's two mechanisms",
        [
            "**What this tests:** forcing dispatch back onto the *sender's* "
            "own thread (not the receiver's). Pulsar has a native "
            "primitive for this (its sender-loop check in "
            "`BasicEvent::operator()`); Qt has none, so two different "
            "workarounds are benchmarked here, both against the same "
            "Pulsar baseline:",
            "",
            "- **Qt6 (Signal-Forwarding)** -- a hand-built two-hop trick: "
            "the sender connects one of its own signals to itself via "
            "`Qt::AutoConnection`, exploiting the fact that AutoConnection "
            "routes by the *receiving* object's thread affinity, with the "
            "sender acting as its own receiver.",
            "- **Qt6 (InvokeMethod)** -- uses `QMetaObject::invokeMethod` "
            "directly instead of the self-connection trick.",
        ],
        [
            {"_display": "ThreadAffinityForwarding/1",
             "Pulsar TS":               "BM_SenderDeferral_ThreadAffinity/1",
             "Qt6 (Signal-Forwarding)": "BM_ThreadAffinityForwarding_SignalForwarding/1",
             "Qt6 (InvokeMethod)":      "BM_ThreadAffinityForwarding_InvokeMethod/1"},
            {"_display": "ThreadAffinityForwarding/5",
             "Pulsar TS":               "BM_SenderDeferral_ThreadAffinity/5",
             "Qt6 (Signal-Forwarding)": "BM_ThreadAffinityForwarding_SignalForwarding/5",
             "Qt6 (InvokeMethod)":      "BM_ThreadAffinityForwarding_InvokeMethod/5"},
        ],
        [
            ("Pulsar TS", "Pulsar TS"),
            ("Qt6 (Signal-Forwarding)", "Qt6"),
            ("Qt6 (InvokeMethod)", "Qt6"),
        ],
    ),
    (
        "Scenario 7a (receiver-deferred)",
        [
            "**What this tests:** genuine cross-thread queue + drain on the "
            "receiver's own context -- one or more emitter threads post, "
            "the receiver's owning thread later drains and executes. Only "
            "Pulsar, Qt, and vdk-signals have any deferred-dispatch "
            "primitive at all; nano/sigslot/rocket/nod have no concept of "
            "a thread, event loop, or queue, so they cannot appear in this "
            "table by construction, not by omission.",
            "",
            "Pulsar's two columns use identical per-receiver "
            "`EventLoop`/`AutoDrainThread` and multi-emitter-thread setup; "
            "only the sender's event type differs (`Event` vs. "
            "`ConcurrentEvent`).",
        ],
        [
            {"_display": "ConcurrentEmission PerReceiver/1",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_PerReceiver/1",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_PerReceiver_ConcurrentEvent/1",
             "Qt6":    "BM_ConcurrentEmission_PerReceiver/1",
             "vdk TS": "BM_ConcurrentEmission_PerReceiver/1"},
            {"_display": "ConcurrentEmission PerReceiver/5",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_PerReceiver/5",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_PerReceiver_ConcurrentEvent/5",
             "Qt6":    "BM_ConcurrentEmission_PerReceiver/5",
             "vdk TS": "BM_ConcurrentEmission_PerReceiver/5"},
        ],
        [
            ("Pulsar TS (Event)", "Pulsar TS"),
            ("Pulsar TS (ConcurrentEvent)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("vdk TS", "vdk TS"),
        ],
    ),
    (
        "Scenario 7a (execute-now, serialised)",
        [
            "**What this tests:** synchronous dispatch, externally forced "
            "to one handler at a time. This is the fair cross-library "
            "comparison point: nano's TS_Policy and sigslot's "
            "multi_threaded policy serialise internally; rocket's "
            "thread_safe_policy and nod's multithread_policy only protect "
            "their connection list (see bench_rocket.cpp / bench_nod.cpp), "
            "so their _Serialized variants add an external mutex to match; "
            "Pulsar's Event (RecursiveMutex) and Qt's DirectConnection+"
            "external mutex and vdk's forced exec::sync+external mutex "
            "complete the set.",
            "",
            "**Pulsar TS (Event)** is the genuinely-serialised Pulsar data "
            "point this table's own comparison point above refers to: "
            "`pulsar::Event`'s RecursiveMutex is held for the whole "
            "handler-invocation loop, same as nano/sigslot's internal "
            "locks. **Pulsar TS (ConcurrentEvent)** is included for "
            "reference, not as a second serialised implementation -- "
            "`pulsar::ConcurrentEvent`'s dispatch is lock-free and "
            "parallel-capable regardless of context, so its number here is "
            "identical to its own column in the execute-now, parallel "
            "table below, not a narrower, forced-serial variant of it.",
        ],
        [
            {"_display": "ConcurrentEmission Serialized/1",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_Direct_Serialized/1",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_Direct_Serialized_ConcurrentEvent/1",
             "Qt6":        "BM_ConcurrentEmission_Direct_Serialized/1",
             "nano TS":    "BM_ConcurrentEmission_PerReceiver/1",
             "sigslot MT": "BM_ConcurrentEmission_PerReceiver/1",
             "rocket TS":  "BM_ConcurrentEmission_PerReceiver_Serialized/1",
             "nod":        "BM_ConcurrentEmission_PerReceiver_Serialized/1",
             "vdk TS":     "BM_ConcurrentEmission_Direct_Serialized/1"},
            {"_display": "ConcurrentEmission Serialized/5",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_Direct_Serialized/5",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_Direct_Serialized_ConcurrentEvent/5",
             "Qt6":        "BM_ConcurrentEmission_Direct_Serialized/5",
             "nano TS":    "BM_ConcurrentEmission_PerReceiver/5",
             "sigslot MT": "BM_ConcurrentEmission_PerReceiver/5",
             "rocket TS":  "BM_ConcurrentEmission_PerReceiver_Serialized/5",
             "nod":        "BM_ConcurrentEmission_PerReceiver_Serialized/5",
             "vdk TS":     "BM_ConcurrentEmission_Direct_Serialized/5"},
        ],
        [
            ("Pulsar TS (Event)", "Pulsar TS"),
            ("Pulsar TS (ConcurrentEvent)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("nano TS", "nano TS"),
            ("sigslot MT", "sigslot MT"),
            ("rocket TS", "rocket TS"),
            ("nod", "nod"),
            ("vdk TS", "vdk TS"),
        ],
    ),
    (
        "Scenario 7a (execute-now, parallel)",
        [
            "**What this tests:** synchronous dispatch with no external "
            "serialisation at all -- handler invocations from different "
            "threads may genuinely run concurrently. rocket's and nod's "
            "plain PerReceiver benchmark IS this variant (their own "
            "locking never covers handler execution, only connection-list "
            "safety -- see bench_rocket.cpp / bench_nod.cpp), not a "
            "serialised one. nano and sigslot have no equivalent: their "
            "policies serialise internally with no way to opt out, so "
            "they cannot appear here.",
            "",
            "**Pulsar TS (ConcurrentEvent)** is the genuine entry here -- "
            "`pulsar::ConcurrentEvent`'s lock-free, epoch-based dispatch "
            "lets handler invocations from different threads actually run "
            "concurrently, same as Qt's DirectConnection and vdk's "
            "exec::sync. **Pulsar TS (Event)** is included as a reference "
            "baseline, not a competing parallel implementation: "
            "`pulsar::Event` always serialises (its RecursiveMutex is held "
            "for the whole handler loop, identical code to its column in "
            "the execute-now, serialised table above), so its number here "
            "shows what forced serialisation costs under the exact same "
            "multi-emitter thread pressure the genuinely-parallel column "
            "faces.",
        ],
        [
            {"_display": "ConcurrentEmission Parallel/1",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_Direct_Parallel_ConcurrentEvent/1",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_Direct_Parallel/1",
             "Qt6":       "BM_ConcurrentEmission_Direct_Parallel/1",
             "rocket TS": "BM_ConcurrentEmission_PerReceiver/1",
             "nod":       "BM_ConcurrentEmission_PerReceiver/1",
             "vdk TS":    "BM_ConcurrentEmission_Direct_Parallel/1"},
            {"_display": "ConcurrentEmission Parallel/5",
             "Pulsar TS (ConcurrentEvent)": "BM_ConcurrentEmission_Direct_Parallel_ConcurrentEvent/5",
             "Pulsar TS (Event)":           "BM_ConcurrentEmission_Direct_Parallel/5",
             "Qt6":       "BM_ConcurrentEmission_Direct_Parallel/5",
             "rocket TS": "BM_ConcurrentEmission_PerReceiver/5",
             "nod":       "BM_ConcurrentEmission_PerReceiver/5",
             "vdk TS":    "BM_ConcurrentEmission_Direct_Parallel/5"},
        ],
        [
            ("Pulsar TS (ConcurrentEvent)", "Pulsar TS"),
            ("Pulsar TS (Event)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("rocket TS", "rocket TS"),
            ("nod", "nod"),
            ("vdk TS", "vdk TS"),
        ],
    ),
    (
        "Scenario 7b - Concurrent emission, serialised dispatch, Pulsar vs Qt's three mechanisms",
        [
            "**What this tests:** multiple threads emitting concurrently, "
            "all serialised onto a single shared drain/dispatch context. "
            "Pulsar's `BM_ReceiverDeferral_Serialised` (every receiver "
            "gets its own `Deferred` connection to one shared `EventLoop`, "
            "so one emission produces N separate queue posts) is compared "
            "against three ways of achieving genuinely-queued dispatch in "
            "Qt, each a real pattern developers use:",
            "",
            "- **Qt6 (Signal-Forwarding)** -- forward to the sender's own "
            "thread via the two-hop `Qt::AutoConnection` self-connect "
            "trick (see Scenario 6), then connect every receiver there "
            "with an explicit `Qt::QueuedConnection` rather than assuming "
            "`Direct` is safe once \"home\". One forwarding queue post per "
            "emission, plus N further queued posts (one per receiver).",
            "- **Qt6 (InvokeMethod)** -- same idea, using "
            "`QMetaObject::invokeMethod` for the initial hop instead of "
            "the self-connect trick, then the same explicit "
            "`Qt::QueuedConnection` per receiver.",
            "- **Qt6 (QueuedConnection)** -- no forwarding hop at all: "
            "every receiver moved directly to a shared `QThread` and "
            "connected with its own `Qt::QueuedConnection`. The most "
            "direct structural match to Pulsar's mechanism, with no extra "
            "hop cost.",
            "",
            "All three genuinely queue every receiver dispatch now (fixed "
            "from an earlier version of this benchmark that used "
            "`DirectConnection` for the receiver fan-out after the hop, "
            "which was measuring a fundamentally cheaper, non-equivalent "
            "operation -- see Scenario 6, which still tests that original, "
            "genuinely different sender-forwarding-only pattern).",
        ],
        [
            {"_display": "ConcurrentEmission Serialised/1",
             "Pulsar TS":                  "BM_ReceiverDeferral_Serialised/1",
             "Qt6 (Signal-Forwarding)":    "BM_ConcurrentEmission_Serialised_SignalForwarding/1",
             "Qt6 (InvokeMethod)":         "BM_ConcurrentEmission_Serialised_InvokeMethod/1",
             "Qt6 (QueuedConnection)":     "BM_ConcurrentEmission_Serialised_QueuedConnection/1"},
            {"_display": "ConcurrentEmission Serialised/5",
             "Pulsar TS":                  "BM_ReceiverDeferral_Serialised/5",
             "Qt6 (Signal-Forwarding)":    "BM_ConcurrentEmission_Serialised_SignalForwarding/5",
             "Qt6 (InvokeMethod)":         "BM_ConcurrentEmission_Serialised_InvokeMethod/5",
             "Qt6 (QueuedConnection)":     "BM_ConcurrentEmission_Serialised_QueuedConnection/5"},
        ],
        [
            ("Pulsar TS", "Pulsar TS"),
            ("Qt6 (Signal-Forwarding)", "Qt6"),
            ("Qt6 (InvokeMethod)", "Qt6"),
            ("Qt6 (QueuedConnection)", "Qt6"),
        ],
    ),
    (
        "Scenario 8a - Disconnect-by-target",
        [
            "**What this tests:** disconnecting one specific (sender, "
            "receiver) connection out of n concurrent ones on the same "
            "event/signal. Pulsar via a linear scan of the connection "
            "list (`BM_DisconnectByTarget_LinearScan`, see "
            "event_storage.h); Qt6 via `QObject::disconnect(sender, "
            "signal, receiver, slot)` (`BM_DisconnectByTarget`). Not "
            "asserted to share the same underlying complexity -- see "
            "bench_qt_1.cpp's own comment on BM_DisconnectByTarget for "
            "why this measures the same externally observable operation "
            "without claiming mechanism equivalence. Does not exercise "
            "GenData batching -- see Scenario 8b for that.",
        ],
        [
            {"_display": "DisconnectByTarget/1",
             "Pulsar ST": "BM_DisconnectByTarget_LinearScan/1",
             "Pulsar TS": "BM_DisconnectByTarget_LinearScan/1",
             "Qt6":       "BM_DisconnectByTarget/1"},
            {"_display": "DisconnectByTarget/10",
             "Pulsar ST": "BM_DisconnectByTarget_LinearScan/10",
             "Pulsar TS": "BM_DisconnectByTarget_LinearScan/10",
             "Qt6":       "BM_DisconnectByTarget/10"},
            {"_display": "DisconnectByTarget/100",
             "Pulsar ST": "BM_DisconnectByTarget_LinearScan/100",
             "Pulsar TS": "BM_DisconnectByTarget_LinearScan/100",
             "Qt6":       "BM_DisconnectByTarget/100"},
            {"_display": "DisconnectByTarget/1000",
             "Pulsar ST": "BM_DisconnectByTarget_LinearScan/1000",
             "Pulsar TS": "BM_DisconnectByTarget_LinearScan/1000",
             "Qt6":       "BM_DisconnectByTarget/1000"},
        ],
        [
            ("Pulsar ST", "Pulsar ST"),
            ("Pulsar TS", "Pulsar TS"),
            ("Qt6", "Qt6"),
        ],
    ),
    (
        "Scenario 8b - Bulk disconnect via Trackable::extractConnectionsTo()",
        [
            "**What this tests:** Disconnecting all of a receiver's "
            "connections in one batched call, as an alternative to 8a's "
            "linear scan-based disconnect-by-target.",
            "",
            "libsigc++ participates via `sigc::trackable::"
            "notify_callbacks()`, confirmed directly from its own doc "
            "comment: \"can therefore be used to disconnect from all "
            "signals.\" The match is approximate, not exact: Pulsar's "
            "`extractConnectionsTo(target)` disconnects only the "
            "connections to one specific event; `notify_callbacks()` "
            "disconnects the receiver from *every* signal it's connected "
            "to, with no way to scope it to just one. Qt6 participates "
            "via `QObject::disconnect(sender, signal, receiver, "
            "nullptr)` (`BM_DisconnectAllForReceiver_Batch`), which "
            "disconnects every connection matching that (sender, signal, "
            "receiver) triple in one call, per Qt's own documented "
            "behaviour -- the same scope as Pulsar's "
            "`extractConnectionsTo(target)` (one specific event, not "
            "every signal the receiver is connected to). All rows "
            "measure the same underlying question -- does bulk-disconnect "
            "cost scale with the number of tracked connections -- at "
            "different granularity.",
        ],
        [
            {"_display": "DisconnectTracker_Batch/1",
             "Pulsar ST": "BM_DisconnectTracker_Batch/1",
             "Pulsar TS": "BM_DisconnectTracker_Batch/1",
             "Qt6":       "BM_DisconnectAllForReceiver_Batch/1",
             "libsigc++": "BM_DisconnectTracker_Batch/1"},
            {"_display": "DisconnectTracker_Batch/10",
             "Pulsar ST": "BM_DisconnectTracker_Batch/10",
             "Pulsar TS": "BM_DisconnectTracker_Batch/10",
             "Qt6":       "BM_DisconnectAllForReceiver_Batch/10",
             "libsigc++": "BM_DisconnectTracker_Batch/10"},
            {"_display": "DisconnectTracker_Batch/100",
             "Pulsar ST": "BM_DisconnectTracker_Batch/100",
             "Pulsar TS": "BM_DisconnectTracker_Batch/100",
             "Qt6":       "BM_DisconnectAllForReceiver_Batch/100",
             "libsigc++": "BM_DisconnectTracker_Batch/100"},
            {"_display": "DisconnectTracker_Batch/1000",
             "Pulsar ST": "BM_DisconnectTracker_Batch/1000",
             "Pulsar TS": "BM_DisconnectTracker_Batch/1000",
             "Qt6":       "BM_DisconnectAllForReceiver_Batch/1000",
             "libsigc++": "BM_DisconnectTracker_Batch/1000"},
        ],
        [
            ("Pulsar ST", "Pulsar ST"),
            ("Pulsar TS", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("libsigc++", "libsigc++"),
        ],
    ),
    (
        "Scenario 9 - Contention scaling",
        [
            "**What this tests:** how each library's thread-safety "
            "mechanism scales as genuine concurrent contention increases, "
            "isolated from every other variable. `CONTENTION_RECEIVER_"
            "COUNT` (4) receivers are fixed throughout every row; the "
            "only thing that varies is the number of threads "
            "simultaneously calling emit/trigger on the SAME shared "
            "event -- from 1 (no contention at all) up to 32.",
            "",
            "Every column here uses that library's own \"execute-now, "
            "parallel\" or \"thread-safe\" dispatch mode -- no external "
            "serialisation is imposed by the benchmark itself, so what's "
            "being measured is each library's own internal thread-safety "
            "mechanism under increasing pressure, not an artificial lock "
            "we added. Pulsar gets two columns here, not the four-way "
            "mutex-type comparison earlier revisions of this table had: "
            "**Pulsar (ConcurrentEvent)**'s dispatch path is lock-free "
            "(epoch-based reclamation, no mutex of any kind), the same "
            "code regardless of context. **Pulsar (Event)** genuinely "
            "contends on `pulsar::Event`'s RecursiveMutex as thread count "
            "rises -- still Pulsar's primary/default event type, and the "
            "real mutex-under-contention data point missing from a "
            "ConcurrentEvent-only table. (An earlier version of this table "
            "additionally compared `std::mutex`, `std::shared_mutex`, and "
            "a hand-rolled `SpinLock` against otherwise-identical "
            "ConcurrentEvent-style dispatch code; once that dispatch "
            "mechanism changed to be lock-free, those three extra columns "
            "became byte-for-byte copies of the ConcurrentEvent one and "
            "were removed rather than left as dead weight -- Event's "
            "RecursiveMutex column below is a different, still-relevant "
            "comparison, not a revival of those three.)",
            "",
            "The guarantee level genuinely differs across the "
            "non-Pulsar columns, and that difference is exactly what "
            "this table makes visible, not something to gloss over: "
            "nano (`Spin_Mutex`, a hand-rolled atomic spin lock) and "
            "sigslot (a real `std::mutex`) both serialise the *entire* "
            "handler-invocation loop. rocket (`thread_safe_policy`) and nod "
            "(`multithread_policy`, its default) only protect the "
            "connection-list snapshot during dispatch -- confirmed "
            "directly against both sources -- not handler execution "
            "itself, so their numbers reflect a narrower promise than "
            "everyone else's, even though both use a real `std::mutex` "
            "for what they do protect.",
        ],
        [
            {"_display": "ContentionScaling/1",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/1",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/1",
             "Qt6":                           "BM_ContentionScaling_Direct/1",
             "nano TS":                       "BM_ContentionScaling_Direct/1",
             "sigslot MT":                    "BM_ContentionScaling_Direct/1",
             "rocket TS":                     "BM_ContentionScaling_Direct/1",
             "nod":                           "BM_ContentionScaling_Direct/1",
             "vdk TS":                        "BM_ContentionScaling_Direct/1"},
            {"_display": "ContentionScaling/2",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/2",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/2",
             "Qt6":                           "BM_ContentionScaling_Direct/2",
             "nano TS":                       "BM_ContentionScaling_Direct/2",
             "sigslot MT":                    "BM_ContentionScaling_Direct/2",
             "rocket TS":                     "BM_ContentionScaling_Direct/2",
             "nod":                           "BM_ContentionScaling_Direct/2",
             "vdk TS":                        "BM_ContentionScaling_Direct/2"},
            {"_display": "ContentionScaling/4",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/4",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/4",
             "Qt6":                           "BM_ContentionScaling_Direct/4",
             "nano TS":                       "BM_ContentionScaling_Direct/4",
             "sigslot MT":                    "BM_ContentionScaling_Direct/4",
             "rocket TS":                     "BM_ContentionScaling_Direct/4",
             "nod":                           "BM_ContentionScaling_Direct/4",
             "vdk TS":                        "BM_ContentionScaling_Direct/4"},
            {"_display": "ContentionScaling/8",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/8",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/8",
             "Qt6":                           "BM_ContentionScaling_Direct/8",
             "nano TS":                       "BM_ContentionScaling_Direct/8",
             "sigslot MT":                    "BM_ContentionScaling_Direct/8",
             "rocket TS":                     "BM_ContentionScaling_Direct/8",
             "nod":                           "BM_ContentionScaling_Direct/8",
             "vdk TS":                        "BM_ContentionScaling_Direct/8"},
            {"_display": "ContentionScaling/16",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/16",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/16",
             "Qt6":                           "BM_ContentionScaling_Direct/16",
             "nano TS":                       "BM_ContentionScaling_Direct/16",
             "sigslot MT":                    "BM_ContentionScaling_Direct/16",
             "rocket TS":                     "BM_ContentionScaling_Direct/16",
             "nod":                           "BM_ContentionScaling_Direct/16",
             "vdk TS":                        "BM_ContentionScaling_Direct/16"},
            {"_display": "ContentionScaling/32",
             "Pulsar (ConcurrentEvent)":      "BM_ContentionScaling_Direct_ConcurrentEvent/32",
             "Pulsar (Event)":                "BM_ContentionScaling_Direct/32",
             "Qt6":                           "BM_ContentionScaling_Direct/32",
             "nano TS":                       "BM_ContentionScaling_Direct/32",
             "sigslot MT":                    "BM_ContentionScaling_Direct/32",
             "rocket TS":                     "BM_ContentionScaling_Direct/32",
             "nod":                           "BM_ContentionScaling_Direct/32",
             "vdk TS":                        "BM_ContentionScaling_Direct/32"},
        ],
        [
            ("Pulsar (ConcurrentEvent)", "Pulsar TS"),
            ("Pulsar (Event)", "Pulsar TS"),
            ("Qt6", "Qt6"),
            ("nano TS", "nano TS"),
            ("sigslot MT", "sigslot MT"),
            ("rocket TS", "rocket TS"),
            ("nod", "nod"),
            ("vdk TS", "vdk TS"),
        ],
    ),
]

NA = "N/A"


def load_results(results_dir: Path) -> Dict[str, Dict[str, float]]:
    """
    Returns: { library_display_name: { benchmark_name: real_time_ns } }

    Uses real_time (wall-clock), not cpu_time, for every scenario -- not
    just the barrier-synchronized concurrent ones where the difference is
    most dramatic. Confirmed root cause: Google Benchmark's default
    iteration-count auto-tuning is driven by cpu_time. For any benchmark
    where the measuring/main thread spends most of its wall-clock time
    blocked (waiting on a std::barrier for worker threads to finish, as
    every ConcurrentEmission*/ContentionScaling/ReceiverDeferral* scenario
    in this suite does), that thread's own CPU consumption is tiny relative
    to how long the operation actually takes -- so cpu_time systematically
    UNDERSTATES real cost for exactly the scenarios this project cares
    about measuring accurately, sometimes by an order of magnitude or more.
    This was directly responsible for one confirmed, large, wrong result in
    this project's own history: Qt's ContentionScaling numbers understated
    by >20x at 32 threads in an earlier BENCHMARK_RESULTS.md, traced back
    to this exact field choice. real_time has no such blind spot -- it's
    simply how long the operation took, which is what every comparison in
    this project actually wants to know. For genuinely single-threaded,
    non-blocking scenarios (the large majority of this suite), real_time
    and cpu_time are close enough that the choice barely matters; the fix
    is applied uniformly rather than special-cased per scenario so a future
    barrier-based benchmark doesn't quietly reintroduce the same class of
    error.

    Handles three ways a benchmark executable may have been invoked:
      1. no --benchmark_repetitions: one raw entry per benchmark name.
      2. --benchmark_repetitions=N (no aggregates-only): N raw entries
         sharing the same name, plus _mean/_median/_stddev/_cv aggregate
         rows.
      3. --benchmark_repetitions=N --benchmark_report_aggregates_only=true:
         only the aggregate rows, no raw entries at all.

    The _median aggregate is preferred whenever present (matching this
    project's benchmarking methodology, which distrusts a single raw
    run - see run_benchmarks.sh) - not the mean, since Google Benchmark's
    own aggregation warns the mean is sensitive to outliers at this scale.
    Falls back to a raw entry only when no aggregate rows exist (style 1).

    Note this is a change from the previous version, which discarded every
    aggregate row unconditionally and kept whichever raw entry happened to
    be processed last - under style 2 that silently reported one arbitrary
    repetition instead of a real aggregate, and under style 3 it discarded
    every row for every benchmark, since there was nothing left to fall
    back to.
    """
    data: Dict[str, Dict[str, float]] = {}

    AGGREGATE_SUFFIXES = ("_mean", "_median", "_stddev", "_cv")

    for display_name, filename, _ in LIBRARIES:
        # filename is normally a single JSON file; a library can instead
        # list several, whose "benchmarks" arrays get concatenated before
        # processing -- e.g. vdk TS, whose thread-creating benchmarks run
        # as separate processes (each writing its own file) rather than
        # accumulating thread lifecycles within one process. See
        # run_benchmarks.sh's comment on the MinGW thread_local
        # destructor issue this works around.
        filenames = [filename] if isinstance(filename, str) else filename

        benchmarks: List[dict] = []
        any_file_found = False
        for fname in filenames:
            path = results_dir / fname
            if not path.exists():
                continue
            any_file_found = True

            with open(path, encoding="utf-8") as f:
                try:
                    raw = json.load(f)
                except json.JSONDecodeError as e:
                    print(f"# Warning: could not parse {path}: {e}", file=sys.stderr)
                    continue

            benchmarks.extend(raw.get("benchmarks", []))

        if not any_file_found:
            continue

        # base_name -> {"median": val, "raw": val}; median wins if present.
        collected: Dict[str, Dict[str, float]] = {}

        for b in benchmarks:
            name = b.get("name", "")
            real_time = b.get("real_time", None)
            if real_time is None:
                continue

            kind = "raw"
            base_name = name
            for suffix in AGGREGATE_SUFFIXES:
                if name.endswith(suffix):
                    base_name = name[: -len(suffix)]
                    kind = suffix[1:]  # "mean" / "median" / "stddev" / "cv"
                    break

            # Strip /min_time:N.NNN suffix added when ->MinTime() is used in
            # benchmark registration, e.g. BM_Foo/1/min_time:5.000 -> BM_Foo/1
            if "/min_time:" in base_name:
                base_name = base_name[: base_name.index("/min_time:")]

            entry = collected.setdefault(base_name, {})
            if kind == "median":
                entry["median"] = float(real_time)
            elif kind == "raw":
                # Under style 2, multiple raw entries share this base_name;
                # the median aggregate (set above/below regardless of
                # processing order) takes priority over any of them, so
                # which raw entry wins here only matters for style 1
                # (exactly one raw entry) or as a fallback if a run somehow
                # produced raw entries but no median (shouldn't normally
                # happen, but better to keep the first than the last).
                entry.setdefault("raw", float(real_time))
            # mean/stddev/cv are read from the JSON but not used for the
            # report; only median and the style-1 raw fallback are surfaced.

        lib_results: Dict[str, float] = {}
        for base_name, vals in collected.items():
            if "median" in vals:
                lib_results[base_name] = vals["median"]
            elif "raw" in vals:
                lib_results[base_name] = vals["raw"]

        if lib_results:
            data[display_name] = lib_results

    return data


def format_ns(ns: Optional[float]) -> str:
    if ns is None:
        return NA
    if ns < 1_000:
        return f"{ns:.1f} ns"
    if ns < 1_000_000:
        return f"{ns/1_000:.2f} us"
    return f"{ns/1_000_000:.3f} ms"


def resolve_value(bm_name: Optional[str], lib_data: Dict[str, float]) -> Optional[float]:
    """Look up a single benchmark name in one library's results, matching
    render_table's own lookup exactly (including the min_time-suffix
    fallback for the plain-string entry form)."""
    if bm_name is None:
        return None
    val = lib_data.get(bm_name)
    if val is None:
        matches = [v for k, v in lib_data.items() if k == bm_name]
        val = matches[0] if matches else None
    return val


def list_benchmark_names(
    scenario_benchmarks: List,
    columns: List[Tuple[str, str]],
) -> str:
    """Build a "Column: BM_Name, ..." summary of every benchmark name that
    feeds into a scenario's table, so a reader can go straight to the
    source without reverse-engineering it from the data. Strips the
    trailing "/<N>" Arg suffix and de-duplicates, since a table's rows
    are normally the same benchmark family at different Arg values.
    """
    per_column: Dict[str, List[str]] = {display: [] for display, _ in columns}

    for entry in scenario_benchmarks:
        if isinstance(entry, dict):
            for display, _ in columns:
                bm_name = entry.get(display)
                if bm_name is not None:
                    base = re.sub(r"/\d+$", "", bm_name)
                    if base not in per_column[display]:
                        per_column[display].append(base)
        else:
            base = re.sub(r"/\d+$", "", entry)
            for display, _ in columns:
                if base not in per_column[display]:
                    per_column[display].append(base)

    parts = []
    for display, _ in columns:
        names = per_column[display]
        if names:
            parts.append(f"**{display}**: " + ", ".join(f"`{n}`" for n in names))
    return "*Benchmarks: " + "; ".join(parts) + "*" if parts else ""


def all_na_libraries(
    scenario_benchmarks: List,
    results: Dict[str, Dict[str, float]],
    present_libs: List[str],
) -> List[str]:
    """Return the subset of present_libs that would show N/A in every row
    of this scenario's table -- these get dropped from the table entirely
    and listed above it instead, so a 12-column table isn't mostly N/A
    padding for scenarios only 2-3 libraries can actually do."""
    na_libs = []
    for lib in present_libs:
        lib_data = results.get(lib, {})
        has_any_value = False
        for entry in scenario_benchmarks:
            if isinstance(entry, dict):
                bm_name = entry.get(lib)
            else:
                bm_name = entry
            if resolve_value(bm_name, lib_data) is not None:
                has_any_value = True
                break
        if not has_any_value:
            na_libs.append(lib)
    return na_libs


def render_table(
    scenario_benchmarks: List,
    results: Dict[str, Dict[str, float]],
    columns: List[Tuple[str, str]],
) -> List[str]:
    """Render one Markdown table.

    columns is a list of (display_name, source_lib) pairs. source_lib
    identifies which library's results to read; display_name is the column
    header shown. These are the same string in the common case (one column
    per library, e.g. ("Qt6", "Qt6")), but a scenario can point multiple
    columns at the SAME source_lib with different benchmark names each --
    e.g. Qt's two sender-affinity-forwarding mechanisms, both compared
    against the same Pulsar baseline in one table (see Scenario 6 / 7b).

    Each entry in scenario_benchmarks is either:
      - a plain str: same benchmark name used for every column
      - a dict with '_display' (row label) and per-column name overrides,
        keyed by each column's display_name; columns not in the dict fall
        back to N/A

    Column order is fixed for the whole table (so ranking is the only thing
    that moves between rows, not column position). Each non-N/A cell gets
    an inline "(#N)" fastest-to-slowest rank computed within its own row --
    per-row, not per-column, because which column is fastest often changes
    from one row to the next within the same table (e.g. at N=1 vs N=5).
    """
    rows: List[Tuple[str, List[Tuple[str, Optional[float]]]]] = []

    for entry in scenario_benchmarks:
        if isinstance(entry, dict):
            # Per-column name override
            label = entry.get("_display", "")
            raw_vals: List[Tuple[str, Optional[float]]] = []
            for display_name, source_lib in columns:
                bm_name = entry.get(display_name)
                lib_data = results.get(source_lib, {})
                raw_vals.append((display_name, resolve_value(bm_name, lib_data)))
            rows.append((label, raw_vals))
        else:
            bm_name = entry
            raw_vals = []
            for display_name, source_lib in columns:
                lib_data = results.get(source_lib, {})
                raw_vals.append((display_name, resolve_value(bm_name, lib_data)))
            label = bm_name.replace("BM_", "").replace("_", " ")
            rows.append((label, raw_vals))

    # Header
    header = "| Scenario |" + "".join(f" {display_name} |" for display_name, _ in columns)
    sep    = "|---|" + "---|" * len(columns)
    lines  = [header, sep]

    for label, raw_vals in rows:
        # Rank only the entries that have data; N/A cells get no rank.
        ranks: Dict[str, int] = {}
        for i, (col, val) in enumerate(
            sorted((pair for pair in raw_vals if pair[1] is not None), key=lambda p: p[1])
        ):
            ranks[col] = i + 1

        cells = []
        for col, val in raw_vals:
            if val is None:
                cells.append("N/A")
            else:
                cells.append(f"{format_ns(val)} (#{ranks[col]})")

        row = f"| {label} |" + "".join(f" {c} |" for c in cells)
        lines.append(row)

    return lines


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <results-directory> [-o output.md]", file=sys.stderr)
        sys.exit(1)

    # Optional -o/--output flag: write directly to a file with explicit
    # UTF-8 encoding rather than relying on `> file.md` shell redirection.
    # On Windows, PowerShell's `>` redirection re-encodes a child process's
    # stdout according to its own console encoding (historically UTF-16LE
    # with BOM, or the legacy code page) regardless of what encoding the
    # producing program used - this is why previous output redirected via
    # `python compare.py results/ > BENCHMARK_RESULTS.md` could come out as
    # UTF-16 or with mangled multi-byte characters (e.g. "×" turning into
    # "├ù") even though this script always produces correct UTF-8 text.
    # Writing the file directly with open(..., encoding="utf-8") sidesteps
    # the shell's redirection encoding entirely.
    output_path: Optional[Path] = None
    args = sys.argv[1:]
    if "-o" in args or "--output" in args:
        flag = "-o" if "-o" in args else "--output"
        idx = args.index(flag)
        if idx + 1 >= len(args):
            print(f"{flag} requires a filename argument", file=sys.stderr)
            sys.exit(1)
        output_path = Path(args[idx + 1])
        del args[idx:idx + 2]

    if not args:
        print(f"Usage: {sys.argv[0]} <results-directory> [-o output.md]", file=sys.stderr)
        sys.exit(1)

    results_dir = Path(args[0])
    if not results_dir.is_dir():
        print(f"Not a directory: {results_dir}", file=sys.stderr)
        sys.exit(1)

    results = load_results(results_dir)
    present_libs = [name for name, _, _ in LIBRARIES if name in results]

    if not present_libs:
        print("# No benchmark JSON files found in", results_dir, file=sys.stderr)
        sys.exit(1)

    lines: List[str] = []
    lines.append("# Pulsar Benchmark Results")
    lines.append("")
    lines.append(
        "Generated by `compare.py`.  Times are wall-clock (real) time per "
        "iteration, not CPU time - see load_results() in compare.py for why.  "
        "A library with NO data anywhere in a scenario's table is dropped from "
        "that table's columns entirely and listed above it instead (e.g. \"N/A "
        "for this scenario: nano ST, nano TS\") - this keeps tables from being "
        "mostly N/A padding when only 2-3 libraries can do something.  Within a "
        "table that IS shown, an individual N/A cell means that specific row "
        "(not the whole scenario) isn't supported by that configuration.  "
        "A library's columns are omitted entirely (not shown as N/A) when its "
        "results JSON is absent - e.g. Boost.Signals2 is optional "
        "and not currently wired into the run (see RUNNING.md); the absence "
        "does not represet a capability gap.")
    lines.append("")
    lines.append(
        "When --benchmark_repetitions > 1, the median aggregate is used (not "
        "the mean, which Google Benchmark's own docs note is sensitive to "
        "outliers at this scale); falls back to a single raw run only when no "
        "repetitions were requested.")
    lines.append("")
    lines.append(
        "Each row's non-N/A cells are individually ranked fastest-to-slowest "
        "as \"(#1)\", \"(#2)\", etc. - computed per row, so a library's rank can "
        "(and does) change between rows in the same table, e.g. between N=1 "
        "and N=5 of the same scenario.")
    lines.append("")
    lines.append(
        "Collected on a single machine (Windows / MinGW-w64 GCC 13.1.0) - "
        "several findings here are toolchain-specific (e.g. std::shared_mutex's "
        "cost on this platform) and are not claims about behaviour on other "
        "compilers or operating systems.")
    lines.append("")
    lines.append(
        "Concurrent scenarios are split by what they actually measure, not by "
        "benchmark name alone, since several libraries share a name "
        "(\"PerReceiver\") for genuinely different operations:")
    lines.append(
        "  - **receiver-deferred**: a real cross-thread queue + drain round "
        "trip through an event loop/channel.  Only Pulsar, Qt, and "
        "vdk-signals have this primitive at all.")
    lines.append(
        "  - **execute-now, serialised**: synchronous dispatch, externally "
        "forced to one handler at a time (either by the library's own "
        "internal lock, or an external mutex added for libraries whose "
        "own locking only protects the connection list, not handler "
        "execution - see the per-scenario notes below).")
    lines.append(
        "  - **execute-now, parallel**: synchronous dispatch with no "
        "serialisation at all - handler invocations from different "
        "threads may genuinely run concurrently.  Not available for "
        "libraries whose thread-safe policy serialises internally with "
        "no way to opt out (nano, sigslot).")
    lines.append("")

    # Machine / compiler info from first available JSON
    for _, filename, _ in LIBRARIES:
        filenames = [filename] if isinstance(filename, str) else filename
        path = None
        for fname in filenames:
            candidate = results_dir / fname
            if candidate.exists():
                path = candidate
                break
        if path is None:
            continue
        try:
            with open(path, encoding="utf-8") as f:
                raw = json.load(f)
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
        ctx = raw.get("context", {})
        if ctx:
            lines.append("## Environment")
            lines.append("")
            lines.append(f"- **Compiler:** {ctx.get('library_build_type', '')}")
            lines.append(f"- **CPUs:** {ctx.get('num_cpus', '?')} × "
                         f"{ctx.get('mhz_per_cpu', '?')} MHz")
            lines.append(f"- **Date:** {ctx.get('date', 'unknown')}")
            lines.append("")
        break

    # ------------------------------------------------------------------
    # Feature checklist -- static reference data, not derived from the
    # benchmark JSON. See the FEATURES definition above for provenance.
    # ------------------------------------------------------------------
    lines.append("## Feature Checklist")
    lines.append("")
    lines.append(
        "*What each library actually provides, independent of how fast it "
        "is. A library winning a benchmark table above doesn't mean it's "
        "the right choice if it's missing a feature you need -- e.g. "
        "vdk-signals is the fastest library in several tables but is the "
        "only one that isn't header-only, and nod is the only one with no "
        "automatic receiver-lifetime tracking at all.*"
    )
    lines.append("")
    header = "| Feature |" + "".join(f" {lib} |" for lib in FEATURE_LIBRARIES)
    sep    = "|---|" + "---|" * len(FEATURE_LIBRARIES)
    lines.append(header)
    lines.append(sep)
    for feature_name, cells in FEATURES:
        row = f"| {feature_name} |" + "".join(
            f" {cells.get(lib, 'Unknown')} |" for lib in FEATURE_LIBRARIES
        )
        lines.append(row)
    lines.append("")

    for scenario_entry in SCENARIO_GROUPS:
        if len(scenario_entry) == 4:
            heading, description_lines, scenario_benchmarks, explicit_columns = scenario_entry
        else:
            heading, description_lines, scenario_benchmarks = scenario_entry
            explicit_columns = None

        lines.append(f"## {heading}")
        lines.append("")

        for desc_line in description_lines:
            lines.append(desc_line)
        if description_lines:
            lines.append("")

        if explicit_columns is not None:
            # Custom per-table columns (e.g. two Qt mechanisms compared
            # against the same Pulsar baseline in one table) -- already a
            # small, deliberately curated set, so the all-N/A-library
            # pruning below doesn't apply here.
            bm_list = list_benchmark_names(scenario_benchmarks, explicit_columns)
            if bm_list:
                lines.append(bm_list)
                lines.append("")
            table_lines = render_table(scenario_benchmarks, results, explicit_columns)
            lines.extend(table_lines)
        else:
            na_libs = all_na_libraries(scenario_benchmarks, results, present_libs)
            active_libs = [lib for lib in present_libs if lib not in na_libs]

            if na_libs:
                reasons = NA_REASONS.get(heading, {})
                na_parts = []
                for lib in na_libs:
                    reason = reasons.get(lib)
                    na_parts.append(f"{lib} ({reason})" if reason else lib)
                lines.append(f"*N/A for this scenario: {', '.join(na_parts)}*")
                lines.append("")

            if active_libs:
                columns = [(lib, lib) for lib in active_libs]
                bm_list = list_benchmark_names(scenario_benchmarks, columns)
                if bm_list:
                    lines.append(bm_list)
                    lines.append("")
                table_lines = render_table(scenario_benchmarks, results, columns)
                lines.extend(table_lines)
            else:
                lines.append("*No library has data for this scenario.*")
        lines.append("")

    lines.append("---")
    lines.append("")
    lines.append("*Results collected using "
                 "[Google Benchmark](https://github.com/google/benchmark). "
                 "All times are median wall-clock (real) time, not CPU time - "
                 "see load_results()'s docstring in compare.py for why; run "
                 "on a quiet system with no other significant load.*")

    output_text = "\n".join(lines)
    if output_path is not None:
        # newline="\n" prevents Python's universal-newline translation from
        # converting to "\r\n" on Windows - keeps output byte-identical
        # regardless of platform, matching what the existing
        # BENCHMARK_RESULTS.md files in this repo use (LF line endings)
        with open(output_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(output_text)
            f.write("\n")
        print(f"Wrote {output_path} (UTF-8)", file=sys.stderr)
    else:
        print(output_text)


if __name__ == "__main__":
    main()
