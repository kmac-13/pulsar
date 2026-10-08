#ifndef KMAC_STELLYRA_INSPECTION_INFO_H
#define KMAC_STELLYRA_INSPECTION_INFO_H

/**
 * @file inspection_info.h
 * @brief Data structures and utilities for inspecting event connections.
 *
 * Provides:
 * - demangle()      : portable ABI type-name demangling
 * - ConnectionInfo  : snapshot of a single connection's state
 * - EventInfo       : aggregate statistics for an entire event
 *
 * These types are populated by EventInspector.  User code typically reads
 * them but does not construct them directly.
 */

#include "stellyra_fwd.h"
#include "connection_type.h"

#include <cstdlib>
#include <string>

#ifdef __GNUG__
#include <cxxabi.h>
#include <memory>
#endif

namespace kmac {
namespace stellyra {

/**
 * @brief Demangle a C++ ABI type name to a human-readable string.
 *
 * On GCC/Clang this calls abi::__cxa_demangle.  On MSVC, typeid().name()
 * already returns a readable name so the raw string is returned as-is.
 * When RTTI is disabled (-fno-rtti / /GR-) the name pointer
 * is null and "[RTTI disabled]" is returned.
 *
 * @param name raw name from typeid(x).name(), or nullptr if RTTI is off
 * @return human-readable type name, or the raw name if demangling fails
 */
inline std::string demangle( const char* name )
{
	if ( ! name )
	{
		return "[RTTI disabled]";
	}
#ifdef __GNUG__
	int status = 0;
	std::unique_ptr< char, void(*)( void* ) > res {
		abi::__cxa_demangle( name, nullptr, nullptr, &status ),
		std::free
	};
	return ( status == 0 ) ? res.get() : name;
#else
	// MSVC: type_info::name() already returns demangled names
	return name;
#endif
}

/**
 * @brief Yield the ABI type name of x, or nullptr when RTTI is disabled.
 *
 * Use this instead of typeid(x).name() directly so that code guarded by
 * __cpp_rtti compiles cleanly under -fno-rtti / /GR-.
 * demangle() accepts nullptr and returns "[RTTI disabled]".
 */
#ifdef __cpp_rtti
#	define STELLYRA_TYPE_NAME( x ) typeid( x ).name()
#else
#	define STELLYRA_TYPE_NAME( x ) nullptr
#endif

/**
 * @brief Snapshot of a single connection's runtime state.
 *
 * Returned by EventInspector::getConnectionInfo().  All fields are set at
 * the time of the call; the snapshot is not kept in sync with subsequent
 * changes.
 */
struct ConnectionInfo
{
	void* senderAddress;           ///< address of the sender's owning Trackable (nullptr if unknown)
	void* receiverAddress;         ///< address of the receiver object (nullptr for free functions)
	std::string senderTypeName;    ///< demangled type name of the sender
	std::string receiverTypeName;  ///< demangled type name of the receiver
	ConnectionType type;           ///< stored connection type (Direct, Deferred, or Auto)
	EventLoop* senderEventLoop;    ///< sender's current event loop at snapshot time (nullptr if none)
	EventLoop* receiverEventLoop;  ///< receiver's current event loop at snapshot time (nullptr if none)
	int priority;                  ///< execution priority - higher values execute first
	bool isConnected;              ///< false if the connection has been disconnected
	bool isBlocked;                ///< true if the connection is temporarily blocked
	bool isSingleShot;             ///< true if the connection auto-disconnects after first invocation
};

/**
 * @brief Aggregate statistics for an entire Event.
 *
 * Returned by EventInspector::getEventInfo().  Counts are computed at
 * snapshot time and reflect the state of the connection list at that moment.
 */
struct EventInfo
{
	void* eventAddress;              ///< address of the Event object
	void* senderAddress;             ///< address of the sender's owning Trackable (nullptr if none)
	std::string senderTypeName;      ///< demangled type name of the sender
	std::string eventTypeName;       ///< demangled type name of the Event instantiation
	size_t connectionCount;          ///< total entries in the connection list (including dead ones not yet pruned)
	size_t directConnectionCount;    ///< number of Direct connections
	size_t deferredConnectionCount;  ///< number of Deferred connections
	size_t activeConnectionCount;    ///< connections that are currently connected (not disconnected)
	size_t blockedConnectionCount;   ///< connections that are currently blocked
};

/**
 * @brief Alias for users that prefer signal/emit terminology.
 */
using SignalInfo = EventInfo;

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_INSPECTION_INFO_H
