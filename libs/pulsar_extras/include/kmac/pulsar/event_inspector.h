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
 * auto inspector = EventInspector(event);
 * inspector.dumpConnectionGraph(std::cout);
 * @endcode
 *
 * For PrivateEvent, an inspector is the only way to inspect from outside the
 * owning class (PrivateEvent's trigger methods are private, but inspection
 * remains public).
 *
 * All methods acquire the event's internal mutex before reading, so they are
 * safe to call while other threads are emitting or connecting.
 */

#include <kmac/pulsar/pulsar_fwd.h>
#include <kmac/pulsar/config.h>
#include <kmac/pulsar/inspection_info.h>

#include <mutex>
#include <sstream>
#include <vector>

namespace kmac {
namespace pulsar {

/**
 * @brief Read-only inspector for an Event's connection state.
 *
 * @tparam Args the argument types of the associated Event
 */
template< typename... Args >
class EventInspector
{
	// friend class Event< Args... >;

private:
	Event< Args... >* _event;

public:
	/**
	 * @brief Construct an inspector for @p event.
	 *
	 * The inspector holds a raw pointer to the event; the caller is
	 * responsible for ensuring the event outlives the inspector.
	 */
	EventInspector( Event< Args... >& event );

	/**
	 * @brief Construct an inspector for private @p event.
	 *
	 * The inspector holds a raw pointer to the event; the caller is
	 * responsible for ensuring the event outlives the inspector.
	 */
	template< typename FriendType >
	EventInspector( PrivateEvent< FriendType, Args... >& event );

	/**
	 * @brief Internal constructor used by Event and PrivateEvent.
	 *
	 * @param event raw pointer to the event to inspect (may be nullptr for a null inspector)
	 */
	// explicit EventInspector( Event< Args... >* event );

	// default copy/move - the raw pointer is cheap to copy
	EventInspector( const EventInspector& ) = default;
	EventInspector( EventInspector&& ) = default;
	EventInspector& operator=( const EventInspector& ) = default;
	EventInspector& operator=( EventInspector&& ) = default;

	// ======================================================================
	// Connection Information
	// ======================================================================

	/**
	 * @brief Snapshot the state of every connection.
	 *
	 * Returns one ConnectionInfo entry per connection in priority order.
	 * The snapshot is taken atomically under the event's mutex.
	 *
	 * @return vector of connection snapshots; empty if no event is associated
	 */
	std::vector< ConnectionInfo > getConnectionInfo() const;

	/**
	 * @brief Snapshot aggregate statistics for the entire event.
	 *
	 * Counts Direct vs Deferred, active vs blocked, etc.
	 *
	 * @return EventInfo snapshot, all counts zero if no event is associated
	 */
	EventInfo getEventInfo() const;

	/**
	 * @brief Returns the number of entries in the connection list.
	 *
	 * NOTE: This includes dead connections that have not yet been pruned.
	 * Use getEventInfo().activeConnectionCount for live connections only.
	 */
	size_t connectionCount() const;

	// ======================================================================
	// Dumps and Visualization
	// ======================================================================

	/**
	 * @brief Print a human-readable connection list to @p out.
	 *
	 * Each connection is printed with its address, type, priority, status
	 * (Connected / Blocked / Single-shot), sender, and receiver.
	 *
	 * @param out any output stream (std::cout, std::ostringstream, etc.)
	 */
	template< typename OStream >
	void dumpConnections( OStream& out ) const;

	/**
	 * @brief Return the connection list as a string.
	 *
	 * Equivalent to calling dumpConnections() into a std::ostringstream.
	 */
	std::string dumpConnectionsToString() const;

	/**
	 * @brief Print an ASCII connection graph to @p out.
	 *
	 * Renders a tree showing the sender, event, and all receivers with
	 * their connection types, e.g.:
	 * @code
	 * [Sender: Button]
	 *        |
	 *    [Event: Event<int>]
	 *        |
	 *        +--[Direct]---> [Receiver: Counter]
	 *        |
	 *        +--[Deferred, BLOCKED]---> [Receiver: Logger]
	 * @endcode
	 */
	template< typename OStream >
	void dumpConnectionGraph( OStream& out ) const;

	/**
	 * @brief Write a Graphviz DOT representation of the connection graph.
	 *
	 * Produces a complete standalone @c digraph that can be rendered by
	 * Graphviz (@c dot -Tpng), VS Code extensions, or online tools such as
	 * https://dreampuf.github.io/GraphvizOnline.
	 *
	 * Example output:
	 * @code
	 * digraph PulsarConnections {
	 *     rankdir=LR;
	 *     node [shape=box, style=filled, fillcolor=lightgrey];
	 *
	 *     sender_0x1a2b [label="Button\n0x1a2b", fillcolor=lightblue];
	 *     event_0x1a2c  [label="Event<int>\n0x1a2c", shape=ellipse];
	 *     recv_0x1a2d   [label="Handler\n0x1a2d"];
	 *
	 *     sender_0x1a2b -> event_0x1a2c;
	 *     event_0x1a2c  -> recv_0x1a2d  [label="Direct | pri=0"];
	 * }
	 * @endcode
	 *
	 * @param out output stream to write the DOT source to
	 */
	template< typename OStream >
	void toDot( OStream& out ) const;

	/**
	 * @brief Return the Graphviz DOT representation as a string.
	 *
	 * Convenience wrapper around toDot( OStream& ).
	 */
	std::string toDotString() const;

	/**
	 * @brief Return a complete event summary as a string.
	 *
	 * Includes address, type, sender, and connection counts.
	 */
	std::string getSummary() const;
};


//
// IMPLEMENTATION
//

template< typename... Args >
EventInspector< Args... >::EventInspector( Event< Args... >& event )
	: _event( &event )
{
}

template< typename... Args >
template< typename FriendType >
EventInspector< Args... >::EventInspector( PrivateEvent< FriendType, Args... >& event )
	// asEvent() was not implemented on PrivateEvent to avoid giving external
	// code an Event* through which operator() could be called, bypassing
	// access control, but static_cast on the pointer is safe here because
	// EventInspector is a friend class that knows the inheritance relationship
	: _event( static_cast< Event< Args... >* >( &event ) )
{
}

// template< typename... Args >
// EventInspector< Args... >::EventInspector( Event< Args... >* event )
// 	: _event( event )
// {
// }

template< typename... Args >
std::vector< ConnectionInfo > EventInspector< Args... >::getConnectionInfo() const
{
	if ( ! _event )
	{
		return {};
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );
	std::vector< ConnectionInfo > infoList;
	infoList.reserve( _event->_connections.size() );

	for ( const auto& conn : _event->_connections )
	{
		if ( conn )
		{
			infoList.push_back( conn->getInfo() );
		}
	}

	return infoList;
}

template< typename... Args >
EventInfo EventInspector< Args... >::getEventInfo() const
{
	if ( ! _event )
	{
		EventInfo info;
		info.eventAddress = nullptr;
		info.senderAddress = nullptr;
		info.senderTypeName = "None";
		info.eventTypeName = "None";
		info.connectionCount = 0;
		info.directConnectionCount = 0;
		info.deferredConnectionCount = 0;
		info.activeConnectionCount = 0;
		info.blockedConnectionCount = 0;
		return info;
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );

	EventInfo info;
	info.eventAddress = _event;
	info.senderAddress = _event->_senderObj;
	info.senderTypeName = _event->_senderObj
		? demangle( PULSAR_TYPE_NAME( *_event->_senderObj ) )
		: "<None>";
	info.eventTypeName = demangle( PULSAR_TYPE_NAME( *_event ) );
	info.connectionCount = _event->_connections.size();
	info.directConnectionCount = 0;
	info.deferredConnectionCount = 0;
	info.activeConnectionCount = 0;
	info.blockedConnectionCount = 0;

	for ( const auto& conn : _event->_connections )
	{
		if ( ! conn )
		{
			continue;
		}

		if ( conn->isConnected() )
		{
			info.activeConnectionCount++;
		}

		auto connInfo = conn->getInfo();
		if ( connInfo.isBlocked )
		{
			info.blockedConnectionCount++;
		}

		// mirror the Auto resolution logic from ConnectionImpl::invoke():
		// Auto resolves to Deferred when the receiver has a different non-null loop
		// from the sender, and to Direct otherwise
		ConnectionType effectiveType = connInfo.type;
		if ( effectiveType == ConnectionType::Auto )
		{
			if ( connInfo.receiverEventLoop != nullptr
				&& connInfo.receiverEventLoop != connInfo.senderEventLoop )
			{
				effectiveType = ConnectionType::Deferred;
			}
			else
			{
				effectiveType = ConnectionType::Direct;
			}
		}

		if ( effectiveType == ConnectionType::Direct )
		{
			info.directConnectionCount++;
		}
		else
		{
			info.deferredConnectionCount++;
		}
	}

	return info;
}

template< typename... Args >
size_t EventInspector< Args... >::connectionCount() const
{
	if ( ! _event )
	{
		return 0;
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );
	return _event->_connections.size();
}

template< typename... Args >
template< typename OStream >
void EventInspector< Args... >::dumpConnections( OStream& out ) const
{
	if ( ! _event )
	{
		out << "No event associated with inspector\n";
		return;
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );

	out << "Event @ " << static_cast< const void* >( _event ) << "\n";
	out << "  Type: " << demangle( PULSAR_TYPE_NAME( *_event ) ) << "\n";
	out << "  Sender: ";
	if ( _event->_senderObj )
	{
		out
			<< demangle( PULSAR_TYPE_NAME( *_event->_senderObj ) )
			<< " @ " << static_cast< void* >( _event->_senderObj ) << "\n";
	}
	else
	{
		out << "None\n";
	}
	out << "  Total Connections: " << _event->_connections.size() << "\n\n";

	if ( _event->_connections.empty() )
	{
		out << "  No connections\n";
		return;
	}

	int index = 1;
	for ( const auto& conn : _event->_connections )
	{
		if ( ! conn )
		{
			continue;
		}

		auto info = conn->getInfo();

		out << "  Connection #" << index++ << ":\n";
		out << "    Address: "  << info.connectionAddress << "\n";
		out << "    Type: ";
		switch ( info.type )
		{
		case ConnectionType::Direct:
			out << "Direct";
			break;
		case ConnectionType::Deferred:
			out << "Deferred";
			break;
		case ConnectionType::Auto:
			out << "Auto";
			break;
		}
		out << "\n";
		out << "    Priority: " << info.priority << "\n";
		out << "    Status: "   << ( info.isConnected ? "Connected" : "Disconnected" );
		if ( info.isBlocked )
		{
			out << " (Blocked)";
		}
		if ( info.isSingleShot )
		{
			out << " (Single-shot)";
		}
		out << "\n";

		out << "    Sender: ";
		if ( info.senderAddress )
		{
			out << info.senderTypeName << " @ " << info.senderAddress;
		}
		else
		{
			out << "None";
		}
		out << "\n";

		out << "    Receiver: ";
		if ( info.receiverAddress )
		{
			out << info.receiverTypeName << " @ " << info.receiverAddress;
		}
		else
		{
			out << "None (free function)";
		}
		out << "\n\n";
	}
}

template< typename... Args >
std::string EventInspector< Args... >::dumpConnectionsToString() const
{
	std::ostringstream oss;
	dumpConnections( oss );
	return oss.str();
}

template< typename... Args >
template< typename OStream >
void EventInspector< Args... >::dumpConnectionGraph( OStream& out ) const
{
	if ( ! _event )
	{
		out << "No event associated with inspector\n";
		return;
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );

	out << "Connection Graph for Event @ " << static_cast< const void* >( _event ) << "\n";
	out << "================================================================================\n\n";

	if ( _event->_senderObj )
	{
		out << "[Sender: " << demangle( PULSAR_TYPE_NAME( *_event->_senderObj ) ) << "]\n";
		out << "       |\n";
	}
	else
	{
		out << "[No Sender]\n";
		out << "       |\n";
	}

	out << "   [Event: " << demangle( PULSAR_TYPE_NAME( *_event ) ) << "]\n";

	if ( _event->_connections.empty() )
	{
		out << "       |\n";
		out << "    (no connections)\n";
	}
	else
	{
		for ( const auto& conn : _event->_connections )
		{
			if ( ! conn )
			{
				continue;
			}

			auto info = conn->getInfo();

			out << "       |\n";
			out << "       +--[";

			switch ( info.type )
			{
			case ConnectionType::Direct:
				out << "Direct";
				break;
			case ConnectionType::Deferred:
				out << "Deferred";
				break;
			case ConnectionType::Auto:
				out << "Auto";
				break;
			}

			if ( info.isBlocked )
			{
				out << ", BLOCKED";
			}

			out << "]---> ";

			if ( info.receiverAddress )
			{
				out << "[Receiver: " << info.receiverTypeName << "]\n";
			}
			else
			{
				out << "[Free Function]\n";
			}
		}
	}

	out << "\n";
}

template< typename... Args >
std::string EventInspector< Args... >::getSummary() const
{
	auto info = getEventInfo();

	std::ostringstream oss;
	oss << "Event Summary:\n";
	oss << "  Address: " << info.eventAddress << "\n";
	oss << "  Type: " << info.eventTypeName << "\n";
	oss << "  Sender: " << info.senderTypeName << "\n";
	oss << "\nConnections:\n";
	oss << "  Total: " << info.connectionCount << "\n";
	oss << "  Active: " << info.activeConnectionCount << "\n";
	oss << "  Blocked: " << info.blockedConnectionCount << "\n";
	oss << "  Direct: " << info.directConnectionCount << "\n";
	oss << "  Deferred: " << info.deferredConnectionCount << "\n";

	return oss.str();
}

template< typename... Args >
template< typename OStream >
void EventInspector< Args... >::toDot( OStream& out ) const
{
	if ( ! _event )
	{
		out << "// No event associated with inspector\n";
		return;
	}

	platform::SharedLock< platform::SharedMutex > lock( _event->_mutex );

	// Use pointer values as unique node IDs to avoid name collisions when
	// multiple graphs are generated in the same session.
	auto ptrId = []( const void* p ) -> std::string {
		std::ostringstream oss;
		oss << std::hex << reinterpret_cast< std::uintptr_t >( p );
		return oss.str();
	};

	const std::string eventId = "event_" + ptrId( _event );
	const std::string senderTypeName = _event->_senderObj
		? demangle( PULSAR_TYPE_NAME( *_event->_senderObj ) )
		: "";
	const std::string eventTypeName = demangle( PULSAR_TYPE_NAME( *_event ) );

	out << "digraph PulsarConnections {\n";
	out << "\trankdir=LR;\n";
	out << "\tnode [shape=box, style=filled, fillcolor=lightgrey, fontname=\"Helvetica\"];\n";
	out << "\tedge [fontname=\"Helvetica\", fontsize=10];\n";
	out << "\n";

	// Sender node
	if ( _event->_senderObj )
	{
		const std::string senderId = "sender_" + ptrId( _event->_senderObj );
		out
			<< "\t" << senderId
			<< " [label=\"" << senderTypeName
			<< "\\n" << ptrId( _event->_senderObj )
			<< "\", fillcolor=lightblue];\n";

		// Event node
		out
			<< "\t" << eventId
			<< " [label=\"" << eventTypeName
			<< "\\n" << ptrId( _event )
			<< "\", shape=ellipse, fillcolor=lightyellow];\n";

		out << "\n";
		out << "\t" << senderId << " -> " << eventId << ";\n";
	}
	else
	{
		// no sender - just show the event node
		out
			<< "\t" << eventId
			<< " [label=\"" << eventTypeName
			<< "\\n" << ptrId( _event )
			<< "\", shape=ellipse, fillcolor=lightyellow];\n";
		out << "\n";
	}

	// Connection edges
	int freeFuncCount = 0;
	for ( const auto& conn : _event->_connections )
	{
		if ( ! conn )
		{
			continue;
		}

		auto info = conn->getInfo();

		// build edge label
		std::ostringstream label;
		switch ( info.type )
		{
		case ConnectionType::Direct:
			label << "Direct";
			break;
		case ConnectionType::Deferred:
			label << "Deferred";
			break;
		case ConnectionType::Auto:
			label << "Auto";
			break;
		}
		if ( info.priority != 0 )
		{
			label << " | pri=" << info.priority;
		}
		if ( info.isBlocked )
		{
			label << " | BLOCKED";
		}
		if ( info.isSingleShot )
		{
			label << " | once";
		}
		if ( ! info.isConnected )
		{
			label << " | DEAD";
		}

		// edge style: dashed for blocked/dead, bold for high priority
		std::string edgeStyle;
		if ( ! info.isConnected || info.isBlocked )
		{
			edgeStyle = ", style=dashed, color=grey";
		}
		else if ( info.priority > 0 )
		{
			edgeStyle = ", style=bold, color=darkgreen";
		}
		else if ( info.priority < 0 )
		{
			edgeStyle = ", color=grey40";
		}

		if ( info.receiverAddress )
		{
			// named receiver node
			const std::string recvId = "recv_" + ptrId( info.receiverAddress );
			out
				<< "\t" << recvId
				<< " [label=\"" << info.receiverTypeName
				<< "\\n" << ptrId( info.receiverAddress )
				<< "\"];\n";
			out
				<< "\t" << eventId << " -> " << recvId
				<< " [label=\"" << label.str() << "\""
				<< edgeStyle << "];\n";
		}
		else
		{
			// free function - synthesise a unique node per connection
			const std::string freeId = "free_" + ptrId( info.connectionAddress )
				+ "_" + std::to_string( freeFuncCount++ );
			out
				<< "\t" << freeId
				<< " [label=\"free function\", shape=diamond];\n";
			out
				<< "\t" << eventId << " -> " << freeId
				<< " [label=\"" << label.str() << "\""
				<< edgeStyle << "];\n";
		}
	}

	out << "}\n";
}

template< typename... Args >
std::string EventInspector< Args... >::toDotString() const
{
	std::ostringstream oss;
	toDot( oss );
	return oss.str();
}

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_INSPECTOR_H
