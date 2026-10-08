#pragma once
#ifndef KMAC_STELLYRA_ASSERT_H
#define KMAC_STELLYRA_ASSERT_H

/**
 * @file stellyra_assert.h
 * @brief STELLYRA_ASSERT_ALWAYS - a hard assertion that survives NDEBUG.
 *
 * Plain assert() compiles to nothing at all when NDEBUG is defined, which
 * is the standard convention for release builds - meaning a plain assert()
 * guarding a genuine misuse (as opposed to a debugging aid) silently
 * disappears in exactly the build configuration most deployed code runs
 * in, and the program proceeds straight into whatever the assert was
 * meant to catch.  STELLYRA_ASSERT_ALWAYS checks its condition
 * unconditionally, in every build, and terminates immediately via
 * std::abort() on failure - std::abort() itself takes no message, so the
 * message is printed to stderr first.  There is no way to catch or
 * recover from this: use it only for conditions that indicate a genuine
 * API misuse with no well-defined way to continue.
 */

#include <cstdio>
#include <cstdlib>

#define STELLYRA_ASSERT_ALWAYS( condition, message ) \
	do \
	{ \
		if ( ! ( condition ) ) \
		{ \
			std::fprintf( stderr, \
				"Stellyra: fatal error at %s:%d\n  condition: %s\n  %s\n", \
				__FILE__, __LINE__, #condition, message ); \
			std::abort(); \
		} \
	} while ( false )

#endif // KMAC_STELLYRA_ASSERT_H
