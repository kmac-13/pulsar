#pragma once
#ifndef KMAC_PULSAR_CONCURRENT_EVENT_H
#define KMAC_PULSAR_CONCURRENT_EVENT_H

/**
 * @file concurrent_event.h
 *
 * @brief Convenience wrapper for basic_concurrent_event.h inclusion along
 * with common aliases for BasicConcurrentEvent using specific mutex types.
 */

#include "basic_concurrent_event.h"

namespace kmac {
namespace pulsar {

/**
 * @brief Thread-safe BasicConcurrentEvent using a standard mutex for
 * connect/disconnect - the default choice, and the primary event type for
 * code interested in high-performance concurrent event dispatch.
 *
 * Dispatch itself is always lock-free (epoch-based) regardless of MutexType
 * - see basic_concurrent_event_impl.h; MutexType only ever governs
 * connect()/disconnect() contention.
 */
template< typename... Args >
using ConcurrentEvent = BasicConcurrentEvent< platform::Mutex, Args... >;

/**
 * @brief Single-threaded BasicConcurrentEvent using a no-op mutex: no
 * connect/disconnect locking overhead at all.  Only safe when the every one
 * of the event's connections is confined to a single thread.
 */
template< typename... Args >
using SingleThreadedConcurrentEvent = BasicConcurrentEvent< platform::NullMutex, Args... >;

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_CONCURRENT_EVENT_H
