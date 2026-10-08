#pragma once
#ifndef KMAC_STELLYRA_HANDLER_ENTRY_H
#define KMAC_STELLYRA_HANDLER_ENTRY_H

/**
 * @file handler_entry.h
 * @brief HandlerEntry<Args...> - per-connection data stored in EventImpl.
 *
 * HandlerEntry is parameterised only on Args..., not on MutexType, because
 * the mutex belongs to EventImpl, not to the individual entry.  This lets
 * all Event variants with the same argument types share the same HandlerEntry
 * layout regardless of their synchronisation strategy.
 *
 * HandlerEntry stores its own generation counter.  EventImpl gives every
 * connection a stable slot in its handler array that is never moved: a
 * disconnected slot becomes an inactive hole that keeps its HandlerEntry
 * (and its generation) in place until the slot is reused.  A Connection
 * refers to its slot directly, and the generation stored here is bumped on
 * each free so a stale Connection to a since-reused slot is detected.  See
 * event_impl.h.
 */

#include "callable.h"
#include "connection_type.h"
#include "event_flags.h"

#include <cstdint>

namespace kmac {
namespace stellyra {

// Forward declaration - full definition in event_loop.h
class EventLoop;

/**
 * @brief One handler instance inside an EventImpl's dense storage array.
 *
 * Memory layout (48 bytes on 64-bit, verified by static_assert below):
 *   offset  0-23:  HandlerType handler        (24B, align 8)
 *   offset 24-31:  uint64_t connectionTag     ( 8B, align 8)
 *   offset    32:  EventFlags flags           ( 1B)
 *   offset    33:  ConnectionTypes connTypes  ( 1B)
 *   offset 34-35:  uint16_t priority          ( 2B, align 2)
 *   offset 36-37:  uint16_t generation        ( 2B, align 2)
 *   offset 38-39:  (compiler padding)         ( 2B)
 *   offset 40-47:  EventLoop* receiverLoop    ( 8B, align 8)
 *
 * @tparam Args  The event argument types.  Must match the owning event.
 */
template< typename... Args >
struct HandlerEntry
{
	using HandlerType = Callable< void( Args... ) >;

	HandlerType handler;  ///< the callable invoked on dispatch

	/**
	 * @brief Migration tag inherited from this connection's receiver
	 * (Trackable::migrationTag()); used as the EventLoop task-migration
	 * tag so that when the receiver switches loops, all of its pending
	 * tasks migrate together in one call, regardless of which connection
	 * or event produced each one.  Untracked connections (no receiver) get 0.
	 */
	uint64_t connectionTag = 0;

	EventFlags flags;            ///< active / predicate / single-shot / owner / blocked bits - see event_flags.h
	ConnectionTypes connTypes;   ///< declared + resolved dispatch mode - see event_flags.h

	uint16_t priority = 0;  ///< user-defined ordering; values above 65535 are clamped at storage

	// bumped by EventImpl each time this slot is freed, so a Connection to a
	// since-reused slot is detected as stale; co-located here (in what was
	// padding) so the dispatch hot path reads it from the handler's own cache
	// line
	uint16_t generation = 0;

	EventLoop* receiverLoop = nullptr;

	// ---- initialisation ----------------------------------------------------

	/**
	 * @brief Initialise all writable fields in one call.
	 *
	 * Used by connectImpl to avoid duplicating the same assignment set for
	 * the reuse-slot and new-slot paths.
	 */
	void init(
		HandlerType&& h,
		uint64_t tag,
		uint16_t prio,
		ConnectionType declaredType,
		ResolvedConnectionType resolvedType,
		EventLoop* loop );
};

static_assert( sizeof( HandlerEntry< int > ) == 48,
	"HandlerEntry layout changed unexpectedly; review field layout" );

template< typename... Args >
inline void HandlerEntry< Args... >::init(
	HandlerType&& h,
	uint64_t tag,
	uint16_t prio,
	ConnectionType declaredType,
	ResolvedConnectionType resolvedType,
	EventLoop* loop )
{
	handler = std::move( h );
	connectionTag = tag;
	priority = prio;
	flags.setActive( true );
	connTypes.setDeclaredConnType( declaredType );
	connTypes.setResolvedConnType( resolvedType );
	receiverLoop = loop;
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_HANDLER_ENTRY_H
