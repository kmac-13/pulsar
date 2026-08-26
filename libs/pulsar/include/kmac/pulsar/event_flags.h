#pragma once
#ifndef KMAC_PULSAR_EVENT_FLAGS_H
#define KMAC_PULSAR_EVENT_FLAGS_H

/**
 * @file event_flags.h
 * @brief Per-handler bit-field types for HandlerEntry.
 *
 * Two separate uint8_t wrappers keep the two concerns cleanly separated:
 *
 *   EventFlags      - single-bit boolean state (active, predicates, one-shot)
 *   ConnectionTypes - two 2-bit fields for declared and resolved connection types
 *
 * Both are non-template so the bit-layout knowledge lives in one place
 * outside the HandlerEntry template.
 *
 * EventFlags bit layout:
 *   bit 0  active
 *   bit 1  hasPredicate
 *   bit 2  isReceiverContextPredicate
 *   bit 3  isSingleShot
 *   bit 4  hasOwner
 *   bit 5  isBlocked
 *   bits 6:7  (reserved)
 *
 * ConnectionTypes bit layout:
 *   bits 0:1  declared ConnectionType
 *   bits 2:3  resolved ResolvedConnectionType
 *   bits 7:4  (reserved)
 */

#include "connection_type.h"

#include <cstdint>

namespace kmac {
namespace pulsar {

// ===========================================================================
// EventFlags - single-bit boolean flags
// ===========================================================================

struct EventFlags
{
	static constexpr uint8_t ACTIVE_BIT = 0x01;
	static constexpr uint8_t HAS_PREDICATE_BIT = 0x02;
	static constexpr uint8_t RECEIVER_CONTEXT_PRED_BIT = 0x04;
	static constexpr uint8_t SINGLE_SHOT_BIT = 0x08;
	static constexpr uint8_t HAS_OWNER_BIT = 0x10;
	static constexpr uint8_t BLOCKED_BIT = 0x20;

	/**
	 * @brief Combined mask: slot is active (bit 0 set) and not blocked
	 * (bit 5 clear) - single AND + compare vs one per flag.
	 */
	static constexpr uint8_t DISPATCH_READY_MASK = ACTIVE_BIT | BLOCKED_BIT;

	uint8_t value = 0;

	/**
	 * @brief True if the slot is connected and eligible for dispatch (before
	 * also checking isBlocked() - see isDispatchReady()).  Cleared when the
	 * connection is disconnected; the slot then becomes a hole available for
	 * reuse via the free list.
	 */
	bool isActive() const;
	void setActive( bool v );

	/**
	 * @brief True if this connection has a conditional predicate attached.
	 * When set, the predicate is evaluated (in the context given by
	 * isReceiverContextPredicate()) before the handler runs; a false result
	 * skips the handler for that trigger.
	 */
	bool hasPredicate() const;
	void setHasPredicate( bool v );

	/**
	 * @brief True if the predicate (see hasPredicate()) evaluates in
	 * Receiver context, false for Sender context - mirrors
	 * PredicateContext::Receiver / ::Sender (see connection_type.h) as a
	 * single bit rather than storing the full enum.
	 */
	bool isReceiverContextPredicate() const;
	void setReceiverContextPredicate( bool v );

	/**
	 * @brief True if this connection auto-disconnects after its handler
	 * fires once (set via ConnParams::once()).
	 */
	bool isSingleShot() const;
	void setIsSingleShot( bool v );

	/**
	 * @brief True if the handler was bound to a receiver/owner object
	 * (connect<Method>, connect(receiver, method)), as opposed to a free
	 * function or an arbitrary lambda/functor (connectFree, connectLambda).
	 * Set by the connect-site, which always knows this unconditionally -
	 * unlike Callable, whose internal representation can't distinguish an
	 * owning-functor lambda from a genuine receiver pointer after
	 * construction.  Used only by EventInspector's "method" vs "free
	 * function" display.
	 */
	bool hasOwner() const;
	void setHasOwner( bool v );

	/**
	 * @brief Per-connection block state, set via Connection::block()/
	 * unblock().  Checked at the same point as isActive() when deciding
	 * whether to dispatch - matching EventImpl's own block()/unblock()
	 * timing model, a blocked connection only suppresses trigger()s that
	 * occur while it is blocked; it does not retroactively cancel a
	 * Deferred invocation already posted to an EventLoop before blocking.
	 */
	bool isBlocked() const;
	void setBlocked( bool v );

	/**
	 * @brief True if the slot should be dispatched: active and not blocked.
	 * Uses a single mask test rather than two separate flag checks.
	 */
	bool isDispatchReady() const;

	/**
	 * @brief Read a single bit: (bits & mask) != 0.
	 */
	static bool getBit( const uint8_t bits, const uint8_t mask );

	/**
	 * @brief Return bits with mask set or cleared according to val, leaving
	 * every other bit untouched.
	 */
	static uint8_t setBit( const uint8_t bits, const uint8_t mask, bool val );
};

// ===========================================================================
// ConnectionTypes - declared and resolved connection type pair
// ===========================================================================

struct ConnectionTypes
{
	static constexpr uint8_t DECLARED_MASK = 0x03;  // bits 0:1
	static constexpr uint8_t RESOLVED_MASK = 0x0C;  // bits 2:3

	uint8_t value = 0;

	/**
	 * @brief The declared ConnectionType set at connect() time (Direct,
	 * Deferred, or Auto) - never changes for the life of the connection.
	 */
	ConnectionType declaredConnType() const;
	void setDeclaredConnType( ConnectionType v );

	/**
	 * @brief The current ResolvedConnectionType (Direct, Deferred, or None),
	 * recomputed from the declared type and loop topology whenever either
	 * side's EventLoop changes.
	 */
	ResolvedConnectionType resolvedConnType() const;
	void setResolvedConnType( ResolvedConnectionType v );
};

// ===========================================================================
// EventFlags definitions
// ===========================================================================

inline bool EventFlags::getBit( const uint8_t bits, const uint8_t mask )
{
	return ( bits & mask ) != 0;
}

inline uint8_t EventFlags::setBit( const uint8_t bits, const uint8_t mask, bool val )
{
	return val ? ( bits | mask ) : ( bits & ~mask );
}

inline bool EventFlags::isActive() const
{
	return getBit( value, ACTIVE_BIT );
}

inline void EventFlags::setActive( bool v )
{
	value = setBit( value, ACTIVE_BIT, v );
}

inline bool EventFlags::hasPredicate() const
{
	return getBit( value, HAS_PREDICATE_BIT );
}

inline void EventFlags::setHasPredicate( bool v )
{
	value = setBit( value, HAS_PREDICATE_BIT, v );
}

inline bool EventFlags::isReceiverContextPredicate() const
{
	return getBit( value, RECEIVER_CONTEXT_PRED_BIT );
}

inline void EventFlags::setReceiverContextPredicate( bool v )
{
	value = setBit( value, RECEIVER_CONTEXT_PRED_BIT, v );
}

inline bool EventFlags::isSingleShot() const
{
	return getBit( value, SINGLE_SHOT_BIT );
}

inline void EventFlags::setIsSingleShot( bool v )
{
	value = setBit( value, SINGLE_SHOT_BIT, v );
}

inline bool EventFlags::hasOwner() const
{
	return getBit( value, HAS_OWNER_BIT );
}

inline void EventFlags::setHasOwner( bool v )
{
	value = setBit( value, HAS_OWNER_BIT, v );
}

inline bool EventFlags::isBlocked() const
{
	return getBit( value, BLOCKED_BIT );
}

inline void EventFlags::setBlocked( bool v )
{
	value = setBit( value, BLOCKED_BIT, v );
}

inline bool EventFlags::isDispatchReady() const
{
	return ( value & DISPATCH_READY_MASK ) == ACTIVE_BIT;
}



// ===========================================================================
// ConnectionTypes definitions
// ===========================================================================

inline ConnectionType ConnectionTypes::declaredConnType() const
{
	return static_cast< ConnectionType >( value & DECLARED_MASK );
}

inline void ConnectionTypes::setDeclaredConnType( ConnectionType v )
{
	value = ( value & ~uint8_t( DECLARED_MASK ) )
		| ( static_cast< uint8_t >( v ) & DECLARED_MASK );
}

inline ResolvedConnectionType ConnectionTypes::resolvedConnType() const
{
	return static_cast< ResolvedConnectionType >( ( value & RESOLVED_MASK ) >> 2 );
}

inline void ConnectionTypes::setResolvedConnType( ResolvedConnectionType v )
{
	value = ( value & ~uint8_t( RESOLVED_MASK ) )
		| ( ( static_cast< uint8_t >( v ) << 2 ) & RESOLVED_MASK );
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_FLAGS_H
