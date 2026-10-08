#pragma once
#ifndef KMAC_STELLYRA_GEN_DATA_H
#define KMAC_STELLYRA_GEN_DATA_H

/**
 * @file gen_data.h
 * @brief GenData - handle/generation pair for stable handler identification.
 *
 * GenData is the canonical way to refer to a specific connection within an
 * EventImpl.  The index is a stable handle, independent of where the
 * corresponding entry currently sits in the (densely-compacted, always
 * correctly-ordered) handler array - EventImpl's slot-map resolves it to
 * the current physical position.  The generation distinguishes different
 * connections that have occupied the same handle over time (handles are
 * reused via a free-list once their connection is disconnected).
 *
 * A GenData is valid as long as the corresponding handle's generation
 * counter has not advanced past the stored value, i.e. as long as the
 * connection has not been disconnected and the handle not reused.
 *
 * index is uint16_t (rather than uint32_t) to pack both values into 4 bytes
 * with no padding, but this means that no single event can have more than
 * 65535 handlers connected at once (a high-water mark, since freed slots are
 * reused via the free list before EventImpl's handler array ever grows past
 * a prior peak - not a lifetime-cumulative count).  If more capacity is ever
 * needed, forwardTo() can fan a single source out across more than one event
 * rather than widening this type.
 *
 * Used by Trackable, Connection, and any code that performs bulk operations
 * on sets of handlers belonging to the same event.
 */

#include "stellyra_assert.h"

#include <cstdint>

namespace kmac {
namespace stellyra {

struct GenData
{
	uint16_t index;       ///< stable handle index into EventImpl's handler array
	uint16_t generation;  ///< generation stamped on the handle when this GenData was issued

	GenData() = default;

	/**
	 * @brief Narrows a wider handler index down to GenData's storage width,
	 * asserting rather than silently truncating if it doesn't fit - see
	 * the file doc above for why this cap exists.  Centralizes the check
	 * so every construction site gets it for free instead of repeating it.
	 */
	GenData( uint32_t idx, uint16_t gen );
};

inline GenData::GenData( uint32_t idx, uint16_t gen )
	: index( static_cast< uint16_t >( idx ) )
	, generation( gen )
{
	STELLYRA_ASSERT_ALWAYS( idx <= UINT16_MAX,
		"handler index exceeds GenData's 16-bit range - this event has more "
		"simultaneously connected handlers than a single event supports" );
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_GEN_DATA_H
