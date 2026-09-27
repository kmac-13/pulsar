#pragma once
#ifndef KMAC_PULSAR_VERSION_H
#define KMAC_PULSAR_VERSION_H

/**
 * @file version.h
 * @brief Compile-time library version: integer components for preprocessor
 * comparisons, plus constexpr and string-literal forms for runtime use.
 */

#include <cstddef>

// NOLINT NOTE: integer macros required for preprocessor version
// comparisons (#if PULSAR_VERSION_MAJOR_INT >= 1) and stringification
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define PULSAR_VERSION_MAJOR_INT 0
#define PULSAR_VERSION_MINOR_INT 1
#define PULSAR_VERSION_PATCH_INT 0

// two-level stringify forces macro expansion before stringification
#define PULSAR_VERSION_STRINGIFY( x ) #x
#define PULSAR_VERSION_TOSTRING( x ) PULSAR_VERSION_STRINGIFY( x )
#define PULSAR_VERSION_STRING_LITERAL \
	PULSAR_VERSION_TOSTRING( PULSAR_VERSION_MAJOR_INT ) "." \
	PULSAR_VERSION_TOSTRING( PULSAR_VERSION_MINOR_INT ) "." \
	PULSAR_VERSION_TOSTRING( PULSAR_VERSION_PATCH_INT )
// NOLINTEND(cppcoreguidelines-macro-usage)

inline constexpr std::size_t PULSAR_VERSION_MAJOR = PULSAR_VERSION_MAJOR_INT;        ///< breaking / incompatible API changes
inline constexpr std::size_t PULSAR_VERSION_MINOR = PULSAR_VERSION_MINOR_INT;        ///< backward-compatible feature additions
inline constexpr std::size_t PULSAR_VERSION_PATCH = PULSAR_VERSION_PATCH_INT;        ///< backward-compatible fixes only
inline constexpr const char* PULSAR_VERSION_STRING = PULSAR_VERSION_STRING_LITERAL;  ///< "MAJOR.MINOR.PATCH"

#endif // KMAC_PULSAR_VERSION_H
