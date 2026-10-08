#ifndef KMAC_STELLYRA_CONFIG_H
#define KMAC_STELLYRA_CONFIG_H

/**
 * @file config.h
 * @brief Stellyra library configuration: logging tags, macros, and optional Nova logging
 * integration.
 *
 * This header centralises all compile-time configuration for the Stellyra event
 * library.  Include it before any other Stellyraheader if you need to customise
 * logging behaviour; it is also included automatically by stellyra_fwd.h.
 *
 * @section nova Nova Integration
 *
 * To route Stellyra log messages through the Nova logging library, define
 * @c STELLYRA_USE_NOVA before including any Stellyra header (typically via
 * a compiler flag or your CMakeLists.txt):
 *
 * @code
 * // CMakeLists.txt
 * target_compile_definitions( app PRIVATE STELLYRA_USE_NOVA )
 * @endcode
 *
 * When Nova is not available, @c STELLYRA_LOG routes to @c std::cerr with a
 * @c [TagName] prefix.
 *
 * @section connection_logging Connection Issue Logging
 *
 * Warnings about connection misconfiguration (Direct connections with
 * mismatched event loops, Deferred connections with no receiver loop) are
 * disabled by default.  Enable them by defining
 * @c STELLYRA_LOG_CONNECTION_ISSUES before including any Stellyra header:
 *
 * @code
 * // CMakeLists.txt
 * target_compile_definitions( app PRIVATE STELLYRA_LOG_CONNECTION_ISSUES )
 * @endcode
 *
 * When disabled, all connection-issue logging compiles to nothing - zero
 * runtime overhead.
 *
 * @section tags Logging Tags
 *
 * Two logging domains are defined:
 *
 * - @c StellyraTag - general Stellyra library messages
 * - @c StellyraConnectionTag - connection misconfiguration warnings
 *
 * With Nova, each tag can be bound to a different sink:
 * @code
 * kmac::nova::ScopedConfigurator config;
 * config.bind< kmac::stellyra::StellyraTag >( &generalSink );
 * config.bind< kmac::stellyra::StellyraConnectionTag >( &warningSink );
 * @endcode
 */

#include "platform.h"

// ============================================================================
// Connection issue logging - disabled by default
// ============================================================================

/**
 * @brief Define to enable connection misconfiguration warnings.
 *
 * When defined (typically via compiler flag), Stellyra will log warnings when:
 * - a Direct connection is made between objects on different event loops
 * - a Direct connection's loop state changes to mismatched after migration
 * - a Direct connection's loop state resolves back to matched after migration
 * - a Deferred connection is made when the receiver has no event loop
 * - a Deferred connection loses its event loop via migration
 * - a Deferred connection gains an event loop via migration
 *
 * Defaults to disabled (0) if not defined externally.
 */
#ifndef STELLYRA_LOG_CONNECTION_ISSUES
#	define STELLYRA_LOG_CONNECTION_ISSUES 0
#endif

// convenience bool for use in Nova traits enabled field
#if STELLYRA_LOG_CONNECTION_ISSUES
#	define STELLYRA_CONNECTION_LOGGING_ENABLED true
#else
#	define STELLYRA_CONNECTION_LOGGING_ENABLED false
#endif

// ============================================================================
// Logging tag types
// Always defined regardless of Nova availability, so call sites compile
// uniformly in both Nova and non-Nova builds.
// ============================================================================

namespace kmac {
namespace stellyra {

/**
 * @brief Nova logging domain tag for general Stellyra library messages.
 */
struct StellyraTag {};

/**
 * @brief Nova logging domain tag for Stellyra connection misconfiguration warnings.
 *
 * Controlled by `STELLYRA_LOG_CONNECTION_ISSUES`.  When that macro is
 * disabled, the Nova traits mark this tag as disabled so logging compiles
 * to nothing with zero overhead.
 */
struct StellyraConnectionTag {};

} // namespace stellyra
} // namespace kmac

// ============================================================================
// Nova integration
// ============================================================================

#ifdef STELLYRA_USE_NOVA

#include <kmac/nova.h>

// register Nova logger traits for StellyraTag, always enabled when Nova is in use
NOVA_LOGGER_TRAITS( kmac::stellyra::StellyraTag, STELLYRA, true, kmac::nova::TimestampHelper::systemNanosecs );

// register Nova logger traits for StellyraConnectionTag, enabled
// field is driven by STELLYRA_LOG_CONNECTION_ISSUES so that disabled
// logging compiles to nothing via Nova's if constexpr elimination
NOVA_LOGGER_TRAITS( kmac::stellyra::StellyraConnectionTag, STELLYRA_CONN, STELLYRA_CONNECTION_LOGGING_ENABLED, kmac::nova::TimestampHelper::systemNanosecs );

/**
 * @brief Log a Stellyra message via Nova.
 *
 * Usage:
 * @code
 * STELLYRA_LOG( kmac::stellyra::StellyraConnectionTag ) << "message\n";
 * @endcode
 */
#define STELLYRA_LOG( Tag ) NOVA_LOG( Tag )

#else // STELLYRA_USE_NOVA not defined - fall back to std::cerr

#include <iostream>

namespace kmac {
namespace stellyra {
namespace detail {

/**
 * @brief Write a `[TagName]` prefix to `std::cerr` and return the stream.
 *
 * Used by the `STELLYRA_LOG` fallback macro so that call sites can chain
 * `operator<<` naturally:
 * @code
 * STELLYRA_LOG( StellyraConnectionTag ) << "message\n";
 * // output: [StellyraConnectionTag] message
 * @endcode
 */
inline std::ostream& stellyraLogStream( const char* tag )
{
	return std::cerr << "[" << tag << "] ";
}

} // namespace detail
} // namespace stellyra
} // namespace kmac

/**
 * @brief Log a Stellyra message to `std::cerr` with a `[TagName]` prefix.
 *
 * The tag name is the stringified token passed as `Tag`, e.g.
 * `StellyraConnectionTag` becomes `"[StellyraConnectionTag]"`.
 *
 * For structured, routable logging, consider enabling Nova via
 * @c STELLYRA_USE_NOVA.
 */
#define STELLYRA_LOG( Tag ) kmac::stellyra::detail::stellyraLogStream( #Tag )

#endif // STELLYRA_USE_NOVA

// ============================================================================
// Convenience macro for connection issue logging at call sites.
// Compiles to nothing when STELLYRA_LOG_CONNECTION_ISSUES is 0,
// regardless of whether Nova is in use.
// ============================================================================

/**
 * @brief Conditionally log a connection issue warning.
 *
 * Expands to a @c STELLYRA_LOG( StellyraConnectionTag ) stream expression when
 * @c STELLYRA_LOG_CONNECTION_ISSUES is non-zero, and to nothing otherwise.
 *
 * Usage:
 * @code
 * STELLYRA_CONNECTION_LOG()
 *    << "Direct connection with mismatched loops: "
 *    << senderTypeName << " -> " << receiverTypeName << "\n";
 * @endcode
 */
#if STELLYRA_LOG_CONNECTION_ISSUES
#	define STELLYRA_CONNECTION_LOG \
	STELLYRA_LOG( kmac::stellyra::StellyraConnectionTag )
#else
// Expands to a discarded-value expression so call sites can chain <<
// without referencing std::cerr or any other stream.
#	define STELLYRA_CONNECTION_LOG \
		if ( true ) { } else std::cerr
#endif

#endif // KMAC_STELLYRA_CONFIG_H
