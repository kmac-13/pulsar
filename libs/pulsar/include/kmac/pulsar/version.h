#pragma once
#ifndef KMAC_PULSAR_VERSION_H
#define KMAC_PULSAR_VERSION_H

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

inline constexpr std::size_t PULSAR_VERSION_MAJOR = PULSAR_VERSION_MAJOR_INT;
inline constexpr std::size_t PULSAR_VERSION_MINOR = PULSAR_VERSION_MINOR_INT;
inline constexpr std::size_t PULSAR_VERSION_PATCH = PULSAR_VERSION_PATCH_INT;
inline constexpr const char* PULSAR_VERSION_STRING = PULSAR_VERSION_STRING_LITERAL;

#endif // KMAC_PULSAR_VERSION_H
