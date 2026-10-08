#pragma once
#ifndef KMAC_STELLYRA_CONNECTION_TYPE_H
#define KMAC_STELLYRA_CONNECTION_TYPE_H

/**
 * @file connection_type.h
 * @brief Declared and resolved connection dispatch types.
 *
 * @section declared ConnectionType (declared - user-facing)
 *
 * Set at connect() time and never changes.  Auto resolves based on the
 * sender/receiver EventLoop topology at connect time and whenever either
 * loop changes via setEventLoop().
 *
 * @section resolved ResolvedConnectionType (internal)
 *
 * Computed from the declared type and the current loop topology.
 * None is a resolved-only state: the Connection remains live and
 * isConnected() returns true, but events are silently dropped.  It
 * occurs when the topology cannot support Direct or Deferred dispatch:
 *
 *   senderLoop == receiverLoop (nullptr == nullptr) -> Direct
 *   both non-null and different                     -> Deferred
 *   any other mismatch                              -> None
 */

#include <cstdint>
#include <type_traits>

namespace kmac {
namespace stellyra {

/**
 * @brief User-facing declared connection type to specify how handlers should
 * be processed when an event is triggered.
 *
 * Passed to connect() overloads.
 */
enum class ConnectionType : uint8_t
{
	Direct   = 0,  ///< always dispatch synchronously inside trigger()
	Deferred = 1,  ///< always enqueue to the receiver's EventLoop
	Auto     = 2,  ///< (default) resolve from loop topology at connect / migration time
};

/**
 * @brief Internal resolved dispatch mode to determine how handlers should be
 * processed.  Not exposed in the connect() API.
 */
enum class ResolvedConnectionType : uint8_t
{
	Direct   = 0,  ///< dispatch synchronously inside trigger()
	Deferred = 1,  ///< enqueue to the receiver's EventLoop
	None     = 2,  ///< incompatible loop topology - events silently dropped
};

/**
 * @brief Specify the context for when a predicate is evaluated (default is Receiver).
 *
 * For Direct connections both contexts behave identically: the predicate
 * is always evaluated synchronously inside trigger() since sender and
 * receiver share the same call path.  The distinction only affects
 * Deferred connections:
 *
 *   Receiver - The task is always posted, the predicate is evaluated inside
 *              invokeDeferred() on the receiver's EventLoop drain, immediately
 *              before the handler is called.  A false result prevents the
 *              handler from being executed during the drain.  This is the
 *              natural default because the handler is what actually does the
 *              work when an event fires, so the decision belongs with it:
 *              filter events based on receiver state at the time of handling.
 *
 *   Sender   - Evaluated in trigger() before the task is posted.  A false
 *              result suppresses the post entirely; the EventLoop never
 *              sees the task.  Use this to filter events based on sender
 *              state at emission time.
 *
 * Predicates are supplied via ConnParams (e.g. ConnParams::when(), or the
 * ConnParams(pred, context) constructor) and accepted by any connect(),
 * connectFree(), connectLambda(), or forwardTo() overload that takes a
 * ConnParams argument.
 */
enum class PredicateContext : uint8_t
{
	Sender   = 0,  ///< evaluated in trigger()
	Receiver = 1,  ///< (default) evaluated in trigger() for Direct, in EventLoop::drain() for Deferred connections
};

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_CONNECTION_TYPE_H
