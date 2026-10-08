#pragma once
#ifndef KMAC_STELLYRA_RECORDABLE_EVENT_H
#define KMAC_STELLYRA_RECORDABLE_EVENT_H

/**
 * @file recordable_event.h
 *
 * @brief Convenience wrapper for basic_recordable_event.h inclusion along
 * with common aliases for BasicRecordableEvent using specific mutex types.
 */

#include "basic_recordable_event.h"

namespace kmac {
namespace stellyra {

/** @brief Default RecordableEvent uses RecursiveMutex, matching Event<Args...>. */
template< typename... Args >
using RecordableEvent = BasicRecordableEvent< platform::RecursiveMutex, Args... >;

/** @brief SharedMutex variant matching SharedEvent<Args...>. */
template< typename... Args >
using SharedRecordableEvent = BasicRecordableEvent< platform::SharedMutex, Args... >;

/** @brief NullMutex variant matching SingleThreadedEvent<Args...>. */
template< typename... Args >
using SingleThreadedRecordableEvent = BasicRecordableEvent< platform::NullMutex, Args... >;

/** @brief Alias for users that prefer signal/emit terminology. */
template< typename... Args >
using RecordableSignal = RecordableEvent< Args... >;

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_RECORDABLE_EVENT_H
