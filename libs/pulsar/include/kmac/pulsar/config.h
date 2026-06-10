#ifndef KMAC_PULSAR_CONFIG_H
#define KMAC_PULSAR_CONFIG_H

/**
 * @file config.h
 * @brief Pulsar library configuration: logging tags, macros, and optional Nova logging
 * integration.
 *
 * This header centralises all compile-time configuration for the Pulsar event
 * library.  Include it before any other Pulsar header if you need to customise
 * logging behaviour; it is also included automatically by pulsar_fwd.h.
 *
 * @section nova Nova Integration
 *
 * To route Pulsar log messages through the Nova logging library, define
 * @c PULSAR_USE_NOVA before including any Pulsar header (typically via
 * a compiler flag or your CMakeLists.txt):
 *
 * @code
 * // CMakeLists.txt
 * target_compile_definitions( app PRIVATE PULSAR_USE_NOVA )
 * @endcode
 *
 * When Nova is not available, @c PULSAR_LOG routes to @c std::cerr with a
 * @c [TagName] prefix.
 *
 * @section connection_logging Connection Issue Logging
 *
 * Warnings about connection misconfiguration (Direct connections with
 * mismatched event loops, Deferred connections with no receiver loop) are
 * disabled by default.  Enable them by defining
 * @c PULSAR_LOG_CONNECTION_ISSUES before including any Pulsar header:
 *
 * @code
 * // CMakeLists.txt
 * target_compile_definitions( app PRIVATE PULSAR_LOG_CONNECTION_ISSUES )
 * @endcode
 *
 * When disabled, all connection-issue logging compiles to nothing - zero
 * runtime overhead.
 *
 * @section tags Logging Tags
 *
 * Two logging domains are defined:
 *
 * - @c PulsarTag - general Pulsar library messages
 * - @c PulsarConnectionTag - connection misconfiguration warnings
 *
 * With Nova, each tag can be bound to a different sink:
 * @code
 * kmac::nova::ScopedConfigurator config;
 * config.bind< kmac::pulsar::PulsarTag >( &generalSink );
 * config.bind< kmac::pulsar::PulsarConnectionTag >( &warningSink );
 * @endcode
 */

#include "platform.h"

// ============================================================================
// Connection issue logging - disabled by default
// ============================================================================

/**
 * @brief Define to enable connection misconfiguration warnings.
 *
 * When defined (typically via compiler flag), Pulsar will log warnings when:
 * - a Direct connection is made between objects on different event loops
 * - a Direct connection's loop state changes to mismatched after migration
 * - a Direct connection's loop state resolves back to matched after migration
 * - a Deferred connection is made when the receiver has no event loop
 * - a Deferred connection loses its event loop via migration
 * - a Deferred connection gains an event loop via migration
 *
 * Defaults to disabled (0) if not defined externally.
 */
#ifndef PULSAR_LOG_CONNECTION_ISSUES
#	define PULSAR_LOG_CONNECTION_ISSUES 0
#endif

// convenience bool for use in Nova traits enabled field
#if PULSAR_LOG_CONNECTION_ISSUES
#	define PULSAR_CONNECTION_LOGGING_ENABLED true
#else
#	define PULSAR_CONNECTION_LOGGING_ENABLED false
#endif

// ============================================================================
// Logging tag types
// Always defined regardless of Nova availability, so call sites compile
// uniformly in both Nova and non-Nova builds.
// ============================================================================

namespace kmac {
namespace pulsar {

/**
 * @brief Nova logging domain tag for general Pulsar library messages.
 */
struct PulsarTag {};

/**
 * @brief Nova logging domain tag for Pulsar connection misconfiguration warnings.
 *
 * Controlled by @c PULSAR_LOG_CONNECTION_ISSUES.  When that macro is
 * disabled, the Nova traits mark this tag as disabled so logging compiles
 * to nothing with zero overhead.
 */
struct PulsarConnectionTag {};

} // namespace pulsar
} // namespace kmac

// ============================================================================
// Nova integration
// ============================================================================

#ifdef PULSAR_USE_NOVA

#include <kmac/nova.h>

// register Nova logger traits for PulsarTag, always enabled when Nova is in use
NOVA_LOGGER_TRAITS( kmac::pulsar::PulsarTag, PULSAR, true, kmac::nova::TimestampHelper::systemNanosecs );

// register Nova logger traits for PulsarConnectionTag, enabled
// field is driven by PULSAR_LOG_CONNECTION_ISSUES so that disabled
// logging compiles to nothing via Nova's if constexpr elimination
NOVA_LOGGER_TRAITS( kmac::pulsar::PulsarConnectionTag, PULSAR_CONN, PULSAR_CONNECTION_LOGGING_ENABLED, kmac::nova::TimestampHelper::systemNanosecs );

/**
 * @brief Log a Pulsar message via Nova.
 *
 * Usage:
 * @code
 * PULSAR_LOG( kmac::pulsar::PulsarConnectionTag ) << "message\n";
 * @endcode
 */
#define PULSAR_LOG( Tag ) NOVA_LOG( Tag )

#else // PULSAR_USE_NOVA not defined - fall back to std::cerr

#include <iostream>

namespace kmac {
namespace pulsar {
namespace detail {

/**
 * @brief Write a @c [TagName] prefix to @c std::cerr and return the stream.
 *
 * Used by the @c PULSAR_LOG fallback macro so that call sites can chain
 * @c operator<< naturally:
 * @code
 * PULSAR_LOG( PulsarConnectionTag ) << "message\n";
 * // output: [PulsarConnectionTag] message
 * @endcode
 */
inline std::ostream& pulsarLogStream( const char* tag )
{
	return std::cerr << "[" << tag << "] ";
}

} // namespace detail
} // namespace pulsar
} // namespace kmac

/**
 * @brief Log a Pulsar message to @c std::cerr with a @c [TagName] prefix.
 *
 * The tag name is the stringified token passed as @p Tag, e.g.
 * @c PulsarConnectionTag becomes @c "[PulsarConnectionTag]".
 *
 * For structured, routable logging, consider enabling Nova via
 * @c PULSAR_USE_NOVA.
 */
#define PULSAR_LOG( Tag ) kmac::pulsar::detail::pulsarLogStream( #Tag )

#endif // PULSAR_USE_NOVA

// ============================================================================
// Convenience macro for connection issue logging at call sites.
// Compiles to nothing when PULSAR_LOG_CONNECTION_ISSUES is 0,
// regardless of whether Nova is in use.
// ============================================================================

/**
 * @brief Conditionally log a connection issue warning.
 *
 * Expands to a @c PULSAR_LOG( PulsarConnectionTag ) stream expression when
 * @c PULSAR_LOG_CONNECTION_ISSUES is non-zero, and to nothing otherwise.
 *
 * Usage:
 * @code
 * PULSAR_CONNECTION_LOG()
 *    << "Direct connection with mismatched loops: "
 *    << senderTypeName << " -> " << receiverTypeName << "\n";
 * @endcode
 */
#if PULSAR_LOG_CONNECTION_ISSUES
#	define PULSAR_CONNECTION_LOG \
	PULSAR_LOG( kmac::pulsar::PulsarConnectionTag )
#else
// Expands to a discarded-value expression so call sites can chain <<
// without referencing std::cerr or any other stream.
#	define PULSAR_CONNECTION_LOG \
		if ( true ) { } else std::cerr
#endif

#endif // KMAC_PULSAR_CONFIG_H
