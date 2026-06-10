#pragma once
#ifndef KMAC_PULSAR_H
#define KMAC_PULSAR_H

/**
 * @file pulsar.h
 * @brief Pulsar - Modern Thread-Safe C++ Event Library
 *
 * A header-only, thread-safe event / handler library for C++17+ with
 * event loops and automatic connection lifecycle management.
 *
 * Namespace: kmac::pulsar
 *
 * @section overview Overview
 *
 * Pulsar is a modern C++17 header-only event library for in-process
 * event-driven programming.  It provides type-safe events with automatic
 * connection lifetime management, built-in thread safety, cross-thread
 * deferred connections, event recording and replay, and comprehensive
 * debugging tools.  Pulsar has no external dependencies beyond the C++
 * standard library.
 *
 * For extended functionality (RAII helpers, combining events, inspection,
 * recording) include <kmac/pulsar_extras.h> instead.
 *
 * @section quick_start Quick Start
 *
 * @code
 * #include <kmac/pulsar.h>
 *
 * namespace pulsar = kmac::pulsar;
 *
 * class Button : public pulsar::Object {
 * public:
 *     pulsar::Event< int, int > clicked { this };
 *     void click( int x, int y ) { clicked( x, y ); }
 * };
 *
 * class Handler : public pulsar::Object {
 * public:
 *     void onClicked( int x, int y ) { ... }
 * };
 *
 * auto button = std::make_shared< Button >();
 * auto handler = std::make_shared< Handler >();
 * button->clicked.connect( handler, &Handler::onClicked );
 * button->click( 10, 20 );
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
 * - full thread safety on all operations (opt-out via PULSAR_THREAD_SAFE=0 for bare-metal)
 *
 * @section components Component Headers
 *
 * | Header               | Purpose                                             |
 * |----------------------|-----------------------------------------------------|
 * | version.h            | Version macros and constants                        |
 * | config.h             | Compile-time feature flags (PULSAR_THREAD_SAFE,     |
 * |                      | PULSAR_ENABLE_THREAD, PULSAR_LOG_CONNECTION_ISSUES) |
 * | platform.h           | platform::Mutex, SharedMutex (PULSAR_THREAD_SAFE)   |
 * | pulsar_fwd.h         | Forward declarations (standalone; included          |
 * |                      | transitively by all component headers)              |
 * | object.h             | Object base class                                   |
 * | event.h              | Event<Args...> core event class                     |
 * | private_event.h      | PrivateEvent<Owner, Args...> (PEvent alias)         |
 * | event_loop.h         | EventLoop (auto-processed and manual-processed)     |
 * | connection.h         | Connection, ConnectionBase                          |
 * | inspection_info.h    | ConnectionInfo, EventInfo data structures           |
 * |                      | (included transitively via event.h)                 |
*/

// version and platform
#include "pulsar/version.h"
#include "pulsar/config.h"
#include "pulsar/platform.h"

// core components
#include "pulsar/connection.h"
#include "pulsar/event_loop.h"
#include "pulsar/object.h"

// event implementations
#include "pulsar/event.h"
#include "pulsar/private_event.h"

#endif // KMAC_PULSAR_H
