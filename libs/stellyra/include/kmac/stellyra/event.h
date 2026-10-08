#pragma once
#ifndef KMAC_STELLYRA_EVENT_H
#define KMAC_STELLYRA_EVENT_H

/**
 * @file event.h
 * @brief BasicEvent<MutexType, Args...> - the triggerable event type.
 *
 * BasicEvent inherits all connection management from EventStorage and adds
 * operator() / trigger() / emit() for dispatching to connected handlers.
 *
 * Public API is focused here.  Implementation details live in:
 *   event_storage.h : EventStorage<MutexType, Args...> - connect/disconnect/block
 *   event_flags.h   : EventFlags                       - bit-field flag wrapper
 *   event_impl.h    : EventImpl<MutexType, Args...>    - dispatch, mutex, handlers
 *   handler_entry.h : HandlerEntry<Args...>            - per-connection slot
 */

#include "basic_event.h"
#include "platform.h"

namespace kmac {
namespace stellyra {

/**
 * @brief Thread-safe event using a recursive mutex - the default choice.
 * connect()/disconnect()/trigger() may be called from any thread, and a
 * handler may safely reentrantly connect, disconnect, or trigger on this
 * same event while it fires.  See SharedEvent for concurrent-reader
 * dispatch, or SingleThreadedEvent when the event is confined to one
 * thread and the locking overhead isn't needed at all.
 */
template< typename... Args >
using Event = BasicEvent< platform::RecursiveMutex, Args... >;

/**
 * @brief Thread-safe event using a shared (reader-writer) mutex: multiple
 * threads may trigger() concurrently and dispatch in parallel, at the cost
 * of connect()/disconnect() taking the exclusive lock (blocking until any
 * in-progress triggers finish) and single-shot connections being
 * unsupported (ConnParams::once() static_asserts) - a handler
 * disconnecting itself while a SharedLock is held would deadlock.
 */
template< typename... Args >
using SharedEvent = BasicEvent< platform::SharedMutex, Args... >;

/**
 * @brief Single-threaded event using a no-op mutex: no locking overhead at
 * all.  Only safe when the event and every one of its connections and
 * triggers are confined to a single thread - using it from more than one
 * thread is undefined behaviour, not a runtime-checked error.
 */
template< typename... Args >
using SingleThreadedEvent = BasicEvent< platform::NullMutex, Args... >;

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_EVENT_H
