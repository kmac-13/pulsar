#pragma once
#ifndef KMAC_STELLYRA_EXTRAS_H
#define KMAC_STELLYRA_EXTRAS_H

/**
 * @file stellyra_extras.h
 * @brief Stellyra Extras - Extended functionality for the Stellyra event library
 *
 * Includes everything from <kmac/sellyra.h> plus extended features:
 * RAII connection helpers, combining events, inspection, and recording.
 *
 * Namespace: kmac::stellyra
 *
 * @section when_to_use When to Use
 *
 * Include <kmac/stellyra.h> for normal event-driven programming.
 * Include <kmac/stellyra_extras.h> when you need any of the following:
 *
 * - **ScopedConnection** / **ConnectionGroup** - RAII connection lifetime management
 * - **CombiningEvent** - events that collect and aggregate handler return values
 * - **ConcurrentEvent** - lock-free-dispatch event for high-performance
 *   concurrent connect/disconnect/trigger from multiple threads
 * - **EventInspector** - runtime connection graph inspection and Graphviz DOT export
 * - **RecordableEvent** - event emission recording, replay, and performance statistics
 * - **FastRecursiveMutex** / **SpinRecursiveMutex** - optional mutex types for
 *   BasicEvent<MutexType, ...>, alongside the core library's Event/SharedEvent/
 *   SingleThreadedEvent
 *
 * @section components Component Headers
 *
 * | Header               | Purpose                                             |
 * |----------------------|-----------------------------------------------------|
 * | scoped_connection.h  | ScopedConnection (RAII auto-disconnect)             |
 * | connection_guard.h   | ConnectionGuard (thread-safe Connection access)     |
 * | connection_group.h   | ConnectionGroup (multi-connection RAII)             |
 * | combining_event.h    | CombiningEvent<ReturnType, Combiner, Args...>       |
 * | combiners.h          | Built-in combiner functors (10+ provided)           |
 * | concurrent_event.h   | ConcurrentEvent<Args...>, SingleThreadedConcurrentEvent<Args...> - |
 * |                      | aliases over BasicConcurrentEvent (basic_concurrent_event.h)       |
 * | epoch_domain.h       | EpochDomain - lock-free reclamation backing ConcurrentEvent's      |
 * |                      | dispatch path; not included by this umbrella directly - pulled in  |
 * |                      | transitively via basic_concurrent_event_impl.h                     |
 * | event_inspector.h    | EventInspector<Args...>                             |
 * | recordable_event.h   | RecordableEvent<Args...>                            |
 * | event_recorder.h     | EventRecorder<Args...>                              |
 * |                      | (included transitively via recordable_event.h)      |
 * | property.h           | Property<T>, ReadOnlyProperty<Owner, T>,            |
 * |                      | ComputedProperty<Owner, Fn, T>, ConstProperty<T>    |
 * | mutex_types.h        | FastRecursiveMutex, SpinRecursiveMutex - optional   |
 * |                      | mutex types for BasicEvent<MutexType, ...>, not     |
 * |                      | used by any core-library Event alias, not held to   |
 * |                      | the same validation bar (see the header's own       |
 * |                      | doc comment)                                        |
 */

// core library
#include <kmac/stellyra.h>

// RAII connection helpers
#include <kmac/stellyra/connection_guard.h>
#include <kmac/stellyra/connection_group.h>

// combining events
#include <kmac/stellyra/combining_event.h>
#include <kmac/stellyra/combiners.h>

// concurrent event
#include <kmac/stellyra/concurrent_event.h>

// inspection
#include <kmac/stellyra/event_inspector.h>

// recording
#include <kmac/stellyra/recordable_event.h>

// properties
#include <kmac/stellyra/property.h>

// optional mutex types
#include <kmac/stellyra/mutex_types.h>

#endif // KMAC_STELLYRA_EXTRAS_H
