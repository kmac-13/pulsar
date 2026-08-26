#pragma once
#ifndef KMAC_PULSAR_EVENT_INSPECTOR_H
#define KMAC_PULSAR_EVENT_INSPECTOR_H

/**
 * @file event_inspector.h
 * @brief Read-only inspection and debugging tools for Event connections.
 *
 * EventInspector provides a comprehensive API for introspecting an Event's
 * connection list at runtime without triggering any events.  It is the
 * preferred way to debug connection issues, verify configuration, and monitor
 * performance.
 *
 * An inspector can be constructed directly from an Event reference:
 * @code
 * auto inspector = EventInspector( event );
 * inspector.dumpConnectionGraph( std::cout );
 * @endcode
 *
 * For PrivateEvent, an inspector is the only way to inspect from outside the
 * owning class (PrivateEvent's trigger methods are private, but inspection
 * remains public).
 *
 * All methods acquire the event's internal mutex before reading, so they are
 * safe to call while other threads are emitting or connecting.
 *
 * @note Per-connection receiver address and type are not available in this
 *   version of the library because Callable does not expose its bound object
 *   pointer.  Connections are shown as "method" or "function/lambda" based on
 *   EventFlags::hasOwner().  Full receiver type info may be added in a future
 *   update via an optional per-slot metadata field in HandlerEntry.
 */

#include <kmac/pulsar/pulsar_fwd.h>
#include <kmac/pulsar/config.h>
#include <kmac/pulsar/event_detail.h>
#include <kmac/pulsar/event_storage.h>
#include <kmac/pulsar/inspection_info.h>

#include <sstream>
#include <vector>

namespace kmac {
namespace pulsar {

/**
 * @brief Read-only inspector for an Event's connection state.
 *
 * @tparam MutexType the mutex strategy of the inspected event
 * @tparam Args the argument types of the associated Event
 */
template< typename MutexType, typename... Args >
class EventInspector
{
private:
	/// convenience accessor for the heap-allocated impl
	using Impl = EventImpl< MutexType, Args... >;

	/// raw pointer to the inspected event's storage; never null after construction -
	/// the caller is responsible for ensuring the event outlives the inspector
	EventStorage< MutexType, Args... >* _storage;

public:
	/**
	 * @brief Construct an inspector for `event`.
	 *
	 * @param event event to inspect; must outlive this inspector
	 */
	EventInspector( BasicEvent< MutexType, Args... >& event );

	/**
	 * @brief Construct an inspector for a private event.
	 *
	 * EventInspector is declared a friend of BasicPrivateEvent so this
	 * constructor can reach the EventStorage base without exposing trigger
	 * methods through an Event*.
	 *
	 * @param event private event to inspect; must outlive this inspector
	 */
	template< typename FriendType >
	EventInspector( BasicPrivateEvent< FriendType, MutexType, Args... >& event );

	// default copy/move - the raw pointer is cheap to copy
	EventInspector( const EventInspector& ) = default;
	EventInspector( EventInspector&& ) = default;
	EventInspector& operator=( const EventInspector& ) = default;
	EventInspector& operator=( EventInspector&& ) = default;

	// =========================================================================
	// connection information
	// =========================================================================

	/**
	 * @brief Snapshot the state of every connection.
	 *
	 * Returns one ConnectionInfo entry per slot in the handler table.
	 * Inactive (disconnected) slots are included with isConnected == false
	 * so the caller can see the full table layout.  The snapshot is taken
	 * atomically under the event's mutex.
	 *
	 * @return vector of connection snapshots in slot order
	 */
	std::vector< ConnectionInfo > getConnectionInfo() const;

	/**
	 * @brief Snapshot aggregate statistics for the entire event.
	 *
	 * @return EventInfo snapshot
	 */
	EventInfo getEventInfo() const;

	/**
	 * @brief Returns the total number of slots in the handler table.
	 *
	 * Includes inactive slots not yet reclaimed by the free-list.
	 * Use getEventInfo().activeConnectionCount for live connections only.
	 */
	std::size_t connectionCount() const;

	// =========================================================================
	// dumps and visualisation
	// =========================================================================

	/**
	 * @brief Print a human-readable connection list to `out`.
	 *
	 * Each slot is printed with its index, declared/resolved type, priority,
	 * status (Connected / Single-shot / Predicate), and whether it is a
	 * method or free-function connection.
	 *
	 * @param out any output stream (std::cout, std::ostringstream, etc.)
	 */
	template< typename OStream >
	void dumpConnections( OStream& out ) const;

	/**
	 * @brief Return the connection list as a string.
	 */
	std::string dumpConnectionsToString() const;

	/**
	 * @brief Print an ASCII connection graph to `out`.
	 *
	 * Renders a tree showing the sender, event, and all receivers:
	 * @code
	 * [Sender: MyClass]
	 *        |
	 *    [Event: Event<int>]
	 *        |
	 *        +--[Direct]---> [method]
	 *        |
	 *        +--[Auto -> Deferred]---> [function/lambda]
	 * @endcode
	 */
	template< typename OStream >
	void dumpConnectionGraph( OStream& out ) const;

	/**
	 * @brief Write a Graphviz DOT representation of the connection graph.
	 *
	 * Produces a standalone `digraph` renderable by Graphviz (`dot -Tpng`),
	 * VS Code extensions, or online tools such as
	 * https://dreampuf.github.io/GraphvizOnline.
	 *
	 * @param out output stream to write the DOT source to
	 */
	template< typename OStream >
	void toDot( OStream& out ) const;

	/**
	 * @brief Return the Graphviz DOT representation as a string.
	 */
	std::string toDotString() const;

	/**
	 * @brief Return a compact event summary as a string.
	 *
	 * Includes sender type, connection counts, and a breakdown by type.
	 */
	std::string getSummary() const;

private:
	const platform::SharedPtr< Impl >& impl() const;
};


//
// IMPLEMENTATION
//

template< typename MutexType, typename... Args >
inline EventInspector< MutexType, Args... >::EventInspector(
	BasicEvent< MutexType, Args... >& event )
	: _storage( &event )
{
}

template< typename MutexType, typename... Args >
template< typename FriendType >
inline EventInspector< MutexType, Args... >::EventInspector(
	BasicPrivateEvent< FriendType, MutexType, Args... >& event )
	// static_cast is safe: BasicPrivateEvent inherits BasicEvent which inherits
	// EventStorage; EventInspector is a friend with knowledge of this hierarchy
	: _storage( static_cast< EventStorage< MutexType, Args... >* >( &event ) )
{
}


// ===========================================================================
// internal locking helper
// ===========================================================================

namespace detail {

// invoke body() under the appropriate lock for MutexType;
// SharedMutex uses SharedLock so concurrent inspections don't block each
// other, all other mutex types use an exclusive LockGuard
template< typename MutexType, typename Mutex, typename Body >
void withImplLock( Mutex& mutex, Body&& body )
{
	if constexpr ( HasLockShared_v< MutexType > )
	{
		platform::SharedLock< MutexType > lock( mutex );
		body();
	}
	else
	{
		platform::LockGuard< MutexType > lock( mutex );
		body();
	}
}

} // namespace detail


// ===========================================================================
// getConnectionInfo
// ===========================================================================

template< typename MutexType, typename... Args >
inline std::vector< ConnectionInfo >
EventInspector< MutexType, Args... >::getConnectionInfo() const
{
	std::vector< ConnectionInfo > infoList;

	detail::withImplLock< MutexType >( impl()->mutex, [ & ]()
	{
		const auto& handlers = impl()->handlers;
		infoList.reserve( handlers.size() );

		const Trackable* owner = impl()->owner;

		for ( const auto& entry : handlers )
		{
			ConnectionInfo info;

			info.senderAddress  = const_cast< Trackable* >( owner );
			info.senderTypeName = owner
				? demangle( PULSAR_TYPE_NAME( *owner ) )
				: "<None>";
			info.senderEventLoop = owner ? owner->eventLoop() : nullptr;
			info.receiverEventLoop = entry.receiverLoop;
			info.receiverAddress = nullptr;  // not exposed by Callable
			info.receiverTypeName = entry.flags.hasOwner()
				? "<method>"
				: "<function/lambda>";
			info.type = entry.connTypes.declaredConnType();
			info.priority = static_cast< int >( entry.priority );
			info.isConnected = entry.flags.isActive();
			info.isSingleShot = entry.flags.isSingleShot();
			info.isBlocked = entry.flags.isBlocked();

			infoList.push_back( info );
		}
	} );

	return infoList;
}


// ===========================================================================
// getEventInfo
// ===========================================================================

template< typename MutexType, typename... Args >
inline EventInfo
EventInspector< MutexType, Args... >::getEventInfo() const
{
	EventInfo info;
	info.eventAddress = _storage;
	info.senderAddress = nullptr;
	info.senderTypeName = "<None>";
	info.eventTypeName = demangle( PULSAR_TYPE_NAME( *_storage ) );
	info.connectionCount = 0;
	info.activeConnectionCount = 0;
	info.directConnectionCount = 0;
	info.deferredConnectionCount = 0;
	info.blockedConnectionCount = 0;

	detail::withImplLock< MutexType >( impl()->mutex, [ & ]()
	{
		const Trackable* owner = impl()->owner;
		info.senderAddress = const_cast< Trackable* >( owner );
		info.senderTypeName = owner
			? demangle( PULSAR_TYPE_NAME( *owner ) )
			: "<None>";

		const auto& handlers = impl()->handlers;
		info.connectionCount = handlers.size();

		for ( const auto& entry : handlers )
		{
			if ( ! entry.flags.isActive() )
			{
				continue;
			}
			++info.activeConnectionCount;

			if ( entry.flags.isBlocked() )
			{
				++info.blockedConnectionCount;
			}

			if ( entry.connTypes.resolvedConnType() == ResolvedConnectionType::Direct )
			{
				++info.directConnectionCount;
			}
			else
			{
				++info.deferredConnectionCount;
			}
		}
	} );

	return info;
}


// ===========================================================================
// connectionCount
// ===========================================================================

template< typename MutexType, typename... Args >
inline std::size_t
EventInspector< MutexType, Args... >::connectionCount() const
{
	std::size_t count = 0;
	detail::withImplLock< MutexType >( impl()->mutex, [ & ]() {
		count = impl()->handlers.size();
	} );
	return count;
}


// ===========================================================================
// dumpConnections
// ===========================================================================

template< typename MutexType, typename... Args >
template< typename OStream >
inline void EventInspector< MutexType, Args... >::dumpConnections( OStream& out ) const
{
	detail::withImplLock< MutexType >( impl()->mutex, [ & ]()
	{
		const Trackable* owner = impl()->owner;
		const auto& handlers = impl()->handlers;

		out << "Event @ " << static_cast< const void* >( _storage ) << "\n";
		out << "  Type:   " << demangle( PULSAR_TYPE_NAME( *_storage ) ) << "\n";
		out << "  Sender: ";
		if ( owner )
		{
			out << demangle( PULSAR_TYPE_NAME( *owner ) )
				<< " @ " << static_cast< const void* >( owner ) << "\n";
		}
		else
		{
			out << "None\n";
		}
		out << "  Total slots: " << handlers.size() << "\n\n";

		if ( handlers.empty() )
		{
			out << "  No connections\n";
			return;
		}

		for ( std::size_t i = 0; i < handlers.size(); ++i )
		{
			const auto& entry = handlers[ i ];

			out << "  Slot #" << i << ":\n";

			// declared vs resolved type
			const ConnectionType declared = entry.connTypes.declaredConnType();
			const ResolvedConnectionType resolved = entry.connTypes.resolvedConnType();

			out << "    Type:     ";
			switch ( declared )
			{
			case ConnectionType::Direct:
				out << "Direct";
				break;

			case ConnectionType::Deferred:
				out << "Deferred";
				break;

			case ConnectionType::Auto:
				out << "Auto -> ";
				out << ( resolved == ResolvedConnectionType::Direct
					? "Direct" : "Deferred" );
				break;
			}
			out << "\n";

			out << "    Priority: " << static_cast< int >( entry.priority ) << "\n";

			out << "    Status:   "
				<< ( entry.flags.isActive() ? "Connected" : "Disconnected" );
			if ( entry.flags.isSingleShot() )
			{
				out << " (Single-shot)";
			}
			if ( entry.flags.hasPredicate() )
			{
				out << " (Predicate)";
			}
			if ( entry.flags.isBlocked() )
			{
				out << " (Blocked)";
			}
			out << "\n";

			out << "    Handler:  "
				<< ( entry.flags.hasOwner() ? "method" : "function/lambda" )
				<< "\n\n";
		}
	} );
}

template< typename MutexType, typename... Args >
inline std::string
EventInspector< MutexType, Args... >::dumpConnectionsToString() const
{
	std::ostringstream oss;
	dumpConnections( oss );
	return oss.str();
}


// ===========================================================================
// dumpConnectionGraph
// ===========================================================================

template< typename MutexType, typename... Args >
template< typename OStream >
inline void EventInspector< MutexType, Args... >::dumpConnectionGraph( OStream& out ) const
{
	detail::withImplLock< MutexType >( impl()->mutex, [ & ]()
	{
		const Trackable* owner = impl()->owner;
		const auto& handlers = impl()->handlers;

		out << "Connection Graph for Event @ "
			<< static_cast< const void* >( _storage ) << "\n";
		out << "================================================================================\n\n";

		if ( owner )
		{
			out << "[Sender: " << demangle( PULSAR_TYPE_NAME( *owner ) ) << "]\n";
		}
		else
		{
			out << "[No Sender]\n";
		}
		out << "       |\n";
		out << "   [Event: " << demangle( PULSAR_TYPE_NAME( *_storage ) ) << "]\n";

		bool anyActive = false;
		for ( const auto& entry : handlers )
		{
			if ( ! entry.flags.isActive() )
			{
				continue;
			}
			anyActive = true;

			out << "       |\n";
			out << "       +--[";

			const ConnectionType declared = entry.connTypes.declaredConnType();
			const ResolvedConnectionType resolved = entry.connTypes.resolvedConnType();
			switch ( declared )
			{
			case ConnectionType::Direct:
				out << "Direct";
				break;

			case ConnectionType::Deferred:
				out << "Deferred";
				break;

			case ConnectionType::Auto:
				out << "Auto -> ";
				out << ( resolved == ResolvedConnectionType::Direct
					? "Direct" : "Deferred" );
				break;
			}

			out << "]---> ";
			out << ( entry.flags.hasOwner() ? "[method]\n" : "[function/lambda]\n" );
		}

		if ( ! anyActive )
		{
			out << "       |\n";
			out << "    (no active connections)\n";
		}

		out << "\n";
	} );
}


// ===========================================================================
// toDot
// ===========================================================================

template< typename MutexType, typename... Args >
template< typename OStream >
inline void EventInspector< MutexType, Args... >::toDot( OStream& out ) const
{
	detail::withImplLock< MutexType >( impl()->mutex, [ & ]()
	{
		auto ptrId = []( const void* p ) -> std::string {
			std::ostringstream oss;
			oss << std::hex << reinterpret_cast< std::uintptr_t >( p );
			return oss.str();
		};

		const Trackable* owner = impl()->owner;
		const auto& handlers = impl()->handlers;
		const std::string eventId = "event_" + ptrId( _storage );
		const std::string eventTypeName = demangle( PULSAR_TYPE_NAME( *_storage ) );

		out << "digraph PulsarConnections {\n";
		out << "\trankdir=LR;\n";
		out << "\tnode [shape=box, style=filled, fillcolor=lightgrey, fontname=\"Helvetica\"];\n";
		out << "\tedge [fontname=\"Helvetica\", fontsize=10];\n";
		out << "\n";

		if ( owner )
		{
			const std::string senderId = "sender_" + ptrId( owner );
			const std::string senderTypeName = demangle( PULSAR_TYPE_NAME( *owner ) );

			out << "\t" << senderId
				<< " [label=\"" << senderTypeName
				<< "\\n" << ptrId( owner )
				<< "\", fillcolor=lightblue];\n";

			out << "\t" << eventId
				<< " [label=\"" << eventTypeName
				<< "\\n" << ptrId( _storage )
				<< "\", shape=ellipse, fillcolor=lightyellow];\n";

			out << "\n";
			out << "\t" << senderId << " -> " << eventId << ";\n";
		}
		else
		{
			out << "\t" << eventId
				<< " [label=\"" << eventTypeName
				<< "\\n" << ptrId( _storage )
				<< "\", shape=ellipse, fillcolor=lightyellow];\n";
			out << "\n";
		}

		// one handler node per slot; synthesise unique IDs by slot index
		int freeFuncCount = 0;
		for ( std::size_t i = 0; i < handlers.size(); ++i )
		{
			const auto& entry = handlers[ i ];

			// build edge label from declared + resolved types
			std::ostringstream label;
			const ConnectionType declared = entry.connTypes.declaredConnType();
			const ResolvedConnectionType resolved = entry.connTypes.resolvedConnType();
			switch ( declared )
			{
			case ConnectionType::Direct:
				label << "Direct";
				break;

			case ConnectionType::Deferred:
				label << "Deferred";
				break;

			case ConnectionType::Auto:
				label << "Auto";
				if ( resolved != ResolvedConnectionType::Direct )
				{
					label << " -> Deferred";
				}
				break;
			}

			if ( entry.priority != 0 )
			{
				label << " | pri=" << static_cast< int >( entry.priority );
			}
			if ( entry.flags.isSingleShot() )
			{
				label << " | once";
			}
			if ( ! entry.flags.isActive() )
			{
				label << " | DEAD";
			}
			else if ( entry.flags.isBlocked() )
			{
				label << " | BLOCKED";
			}

			// edge style: dashed for dead slots, dotted orange for blocked,
			// bold for high priority
			std::string edgeStyle;
			if ( ! entry.flags.isActive() )
			{
				edgeStyle = ", style=dashed, color=grey";
			}
			else if ( entry.flags.isBlocked() )
			{
				edgeStyle = ", style=dotted, color=orangered";
			}
			else if ( entry.priority > 0 )
			{
				edgeStyle = ", style=bold, color=darkgreen";
			}

			if ( entry.flags.hasOwner() )
			{
				// method connection - node ID is slot-index based since we
				// don't have the receiver address
				const std::string recvId = "method_" + ptrId( _storage ) + "_" + std::to_string( i );
				out << "\t" << recvId
					<< " [label=\"method\\nslot " << i << "\"];\n";
				out << "\t" << eventId << " -> " << recvId
					<< " [label=\"" << label.str() << "\""
					<< edgeStyle << "];\n";
			}
			else
			{
				// free function or lambda - both grouped under one node
				// kind since Callable doesn't distinguish them
				const std::string freeId =
					"free_" + ptrId( _storage )
					+ "_" + std::to_string( freeFuncCount++ );
				out << "\t" << freeId
					<< " [label=\"function/lambda\\nslot " << i
					<< "\", shape=diamond];\n";
				out << "\t" << eventId << " -> " << freeId
					<< " [label=\"" << label.str() << "\""
					<< edgeStyle << "];\n";
			}
		}

		out << "}\n";
	} );
}

template< typename MutexType, typename... Args >
inline std::string
EventInspector< MutexType, Args... >::toDotString() const
{
	std::ostringstream oss;
	toDot( oss );
	return oss.str();
}


// ===========================================================================
// getSummary
// ===========================================================================

template< typename MutexType, typename... Args >
inline std::string EventInspector< MutexType, Args... >::getSummary() const
{
	auto info = getEventInfo();

	std::ostringstream oss;
	oss << "Event Summary:\n";
	oss << "  Address: " << info.eventAddress << "\n";
	oss << "  Type:    " << info.eventTypeName << "\n";
	oss << "  Sender:  " << info.senderTypeName;
	if ( info.senderAddress )
	{
		oss << " @ " << info.senderAddress;
	}
	oss << "\n";
	oss << "\nConnections:\n";
	oss << "  Total slots: " << info.connectionCount << "\n";
	oss << "  Active:      " << info.activeConnectionCount << "\n";
	oss << "  Direct:      " << info.directConnectionCount << "\n";
	oss << "  Deferred:    " << info.deferredConnectionCount << "\n";

	return oss.str();
}

template< typename MutexType, typename... Args >
inline const platform::SharedPtr< typename EventInspector< MutexType, Args... >::Impl >&
	EventInspector< MutexType, Args... >::impl() const
{
	return _storage->_impl;
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_INSPECTOR_H
