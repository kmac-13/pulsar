#pragma once
#ifndef KMAC_STELLYRA_H
#define KMAC_STELLYRA_H

/**
 * @file stellyra.h
 * @brief Stellyra - Modern Thread-Safe C++ Event Library
 *
 * A header-only, thread-safe event / handler library for C++17+ with
 * event loops and automatic connection lifecycle management.
 *
 * Namespace: kmac::stellyra
 *
 * @section overview Overview
 *
 * Stellyra is a modern C++17 header-only event library for in-process
 * event-driven programming.  It provides type-safe events with automatic
 * connection lifetime management, built-in thread safety, cross-thread
 * deferred connections, and event-to-event forwarding.  Stellyra has no
 * external dependencies beyond the C++ standard library.
 *
 * For extended functionality (RAII helpers, combining events, inspection,
 * recording) include <kmac/stellyra_extras.h> instead.
 *
 * @section quick_start Quick Start
 *
 * @code
 * #include <kmac/stellyra.h>
 *
 * namespace stellyra = kmac::stellyra;
 *
 * class Button : public stellyra::Trackable {
 * public:
 *     stellyra::Event< int, int > clicked { this };
 *     void click( int x, int y ) { clicked( x, y ); }
 * };
 *
 * class Handler : public stellyra::Trackable {
 * public:
 *     void onClicked( int x, int y ) { ... }
 * };
 *
 * Button button;
 * Handler handler;
 * button.clicked.connect< &Handler::onClicked >( handler );
 * button.click( 10, 20 );
 * @endcode
 *
 * @section features Features
 *
 * - type-safe event dispatching via operator(), trigger(), or emit()
 * - Direct (immediate) and Deferred (postponed) connection types
 * - automatic connection type resolution (ConnectionType::Auto)
 * - automatic disconnection when sender or receiver is destroyed
 * - sender-side emission deferral to EventLoop drain context
 * - event loop migration with pending-event transfer
 * - connection blocking / unblocking without disconnecting
 * - single-shot connections (auto-disconnect after first invocation)
 * - priority-based connection ordering (higher value = earlier execution)
 * - conditional connections (call handler only when predicate returns true)
 * - per-connection configuration (priority, predicate, single-shot) bundled
 *   via ConnParams, settable in any combination at a connect() call site
 * - event-to-event forwarding (forwardTo) for signal-chaining patterns
 * - full thread safety on all operations (opt-out via STELLYRA_THREAD_SAFE=0 for bare-metal)
 *
 * @section components Component Headers
 *
 * | Header               | Purpose                                                   |
 * |----------------------|-----------------------------------------------------------|
 * | version.h            | Version macros and constants                              |
 * | config.h             | Compile-time feature flags (STELLYRA_THREAD_SAFE,           |
 * |                      | STELLYRA_ENABLE_THREAD, STELLYRA_LOG_CONNECTION_ISSUES)       |
 * | platform.h           | platform::Mutex, SharedMutex, Atomic (STELLYRA_THREAD_SAFE) |
 * | stellyra_fwd.h         | Forward declarations (standalone; included                |
 * |                      | transitively by all component headers)                    |
 * | callable.h           | Callable<ReturnType(Args...)> - move-only type-erased callable |
 * | connection_type.h    | ConnectionType, PredicateContext                               |
 * | trackable.h          | Trackable base class - owns/tracks connection lifetimes        |
 * | connection.h         | Connection, ScopedConnection, BlockGuard                       |
 * | conn_params.h        | ConnParams<MutexType,Args...> - priority/predicate/single-shot bundle |
 * | event_flags.h        | EventFlags, ConnectionTypes - per-slot bit-field state         |
 * | handler_entry.h      | HandlerEntry<Args...> - per-connection storage slot            |
 * | event_detail.h       | Shared detail helpers (arity traits, PmfInvoker, connection-type resolution) |
 * | event_impl_base.h    | EventImplBase - type-erased base shared by Connection/EventLoop    |
 * | event_impl.h         | EventImpl<MutexType,Args...> - dispatch, mutex, handler storage    |
 * | event_storage.h      | EventStorage<MutexType,Args...> - connect/disconnect/block API     |
 * | event.h              | Event<Args...>, SharedEvent<Args...>, SingleThreadedEvent<Args...> |
 * | private_event.h      | PrivateEvent<FriendType,Args...> (PEvent alias)               |
 * | event_loop.h         | EventLoop - manually-drained task queue for Deferred dispatch |
 * | auto_drain_thread.h  | AutoDrainThread - dedicated background thread that drains     |
 * |                      | an EventLoop; not included by this umbrella (pulls in         |
 * |                      | \<thread\>/\<atomic\>/\<condition_variable\> unconditionally, |
 * |                      | which would break STELLYRA_THREAD_SAFE=0 bare-metal builds) -   |
 * |                      | include it directly if needed                                 |
 * | inspection_info.h    | ConnectionInfo, EventInfo data structures; not included by    |
 * |                      | this umbrella - pulled in by \<kmac/stellyra_extras.h\> via     |
 * |                      | EventInspector                                                |
*/

// version and platform
#include "stellyra/version.h"
#include "stellyra/config.h"
#include "stellyra/platform.h"

// core components
#include "stellyra/connection.h"
#include "stellyra/event_loop.h"
#include "stellyra/trackable.h"

// event implementations
#include "stellyra/event.h"
#include "stellyra/private_event.h"

#endif // KMAC_STELLYRA_H
