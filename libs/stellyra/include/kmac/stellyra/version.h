#pragma once
#ifndef KMAC_STELLYRA_VERSION_H
#define KMAC_STELLYRA_VERSION_H

/**
 * @file version.h
 * @brief Compile-time library version: integer components for preprocessor
 * comparisons, plus constexpr and string-literal forms for runtime use.
 */

#include <cstddef>

// NOLINT NOTE: integer macros required for preprocessor version
// comparisons (#if STELLYRA_VERSION_MAJOR_INT >= 1) and stringification
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define STELLYRA_VERSION_MAJOR_INT 0
#define STELLYRA_VERSION_MINOR_INT 1
#define STELLYRA_VERSION_PATCH_INT 0

// two-level stringify forces macro expansion before stringification
#define STELLYRA_VERSION_STRINGIFY( x ) #x
#define STELLYRA_VERSION_TOSTRING( x ) STELLYRA_VERSION_STRINGIFY( x )
#define STELLYRA_VERSION_STRING_LITERAL \
	STELLYRA_VERSION_TOSTRING( STELLYRA_VERSION_MAJOR_INT ) "." \
	STELLYRA_VERSION_TOSTRING( STELLYRA_VERSION_MINOR_INT ) "." \
	STELLYRA_VERSION_TOSTRING( STELLYRA_VERSION_PATCH_INT )
// NOLINTEND(cppcoreguidelines-macro-usage)

inline constexpr std::size_t STELLYRA_VERSION_MAJOR = STELLYRA_VERSION_MAJOR_INT;        ///< breaking / incompatible API changes
inline constexpr std::size_t STELLYRA_VERSION_MINOR = STELLYRA_VERSION_MINOR_INT;        ///< backward-compatible feature additions
inline constexpr std::size_t STELLYRA_VERSION_PATCH = STELLYRA_VERSION_PATCH_INT;        ///< backward-compatible fixes only
inline constexpr const char* STELLYRA_VERSION_STRING = STELLYRA_VERSION_STRING_LITERAL;  ///< "MAJOR.MINOR.PATCH"

#endif // KMAC_STELLYRA_VERSION_H
