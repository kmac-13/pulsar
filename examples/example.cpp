#include <kmac/pulsar_extras.h>

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

namespace pulsar = kmac::pulsar;

// ============================================================================
// Example 1: Basic Event/Handler
// ============================================================================

class Button : public pulsar::Object
{
public:
	pulsar::Event< int, int > clicked { this };
	pulsar::PEvent< Button, int, int > pClicked { this };

	void click( int x, int y )
	{
		std::cout << "Button clicked at (" << x << ", " << y << ")\n";
		clicked( x, y );  // operator() - idiomatic C++
	}
	void pClick( int x, int y )
	{
		std::cout << "Button (private) clicked at (" << x << ", " << y << ")\n";
		pClicked( x, y );
	}
};

class ClickHandler : public pulsar::Object
{
public:
	void onClicked( int x, int y )
	{
		std::cout << "  Handler received: (" << x << ", " << y << ")\n";
	}
};

void example1_basic()
{
	std::cout << "\n=== Example 1: Basic Event ===\n";
	std::cout << "Creating button and handler...\n";

	auto button = std::make_shared< Button >();
	auto handler = std::make_shared< ClickHandler >();

	std::cout << "Button: " << button.get() << "\n";
	std::cout << "Handler: " << handler.get() << "\n";
	std::cout << "Handler use_count: " << handler.use_count() << "\n";

	std::cout << "Calling connect...\n";
	auto conn = button->clicked.connect( handler, &ClickHandler::onClicked );

	std::cout << "Connection created\n";
	std::cout << "Connection isConnected: " << conn.isConnected() << "\n";
	// use_count stays at 1 - Pulsar stores a weak_ptr to the receiver so
	// connecting does not extend its lifetime; the connection is severed
	// automatically when the receiver is destroyed.
	std::cout
		<< "Handler use_count after connect: " << handler.use_count()
		<< " (connecting does not extend receiver lifetime)\n";

	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	auto info = inspector.getEventInfo();
	std::cout << "Event info:\n";
	std::cout << "  Total connections: " << info.connectionCount << "\n";
	std::cout << "  Active connections: " << info.activeConnectionCount << "\n";
	std::cout << "  Direct connections: " << info.directConnectionCount << "\n";

	inspector.dumpConnections( std::cout );

	// demonstrate
	auto pInspector = kmac::pulsar::EventInspector( button->pClicked );
	auto pInfo = pInspector.getEventInfo();
	std::cout << "PrivateEvent info (before connection):\n";
	std::cout << "  Total connections: " << pInfo.connectionCount << "\n";
	std::cout << "  Active connections: " << pInfo.activeConnectionCount << "\n";
	std::cout << "  Direct connections: " << pInfo.directConnectionCount << "\n";
	auto pConn = button->pClicked.connect( handler, &ClickHandler::onClicked );
	pInfo = pInspector.getEventInfo();
	std::cout << "PrivateEvent info (after connection):\n";
	std::cout << "  Total connections: " << pInfo.connectionCount << "\n";
	std::cout << "  Active connections: " << pInfo.activeConnectionCount << "\n";
	std::cout << "  Direct connections: " << pInfo.directConnectionCount << "\n";

	std::cout << "\nCalling button->click(10, 20)...\n";
	button->click( 10, 20 );

	std::cout << "After first click\n";
	std::cout << "Handler use_count: " << handler.use_count() << "\n";

	std::cout << "\nCalling button->click(30, 40)...\n";
	button->click( 30, 40 );

	std::cout << "Freeing receiver\n";
	handler.reset();

	std::cout << "\nCalling button->click(50, 60) (should not handle)...\n";
	button->click( 50, 60 );

	std::cout << "Example 1 complete\n";
}

// ============================================================================
// Example 2: Lambda Connections
// ============================================================================

void example2_lambdas()
{
	std::cout << "\n=== Example 2: Lambdas ===\n";

	auto button = std::make_shared< Button >();
	auto receiver = std::make_shared< pulsar::Object >();

	// both lambdas are connected with receiver as the lifetime anchor -
	// when receiver is reset() below, both connections are severed automatically
	button->clicked.connect( receiver, []( int x, int y ) {
		std::cout << "  Lambda 1: " << x << ", " << y << "\n";
	} );

	// count is captured by value so it outlives this scope safely
	int count = 0;
	button->clicked.connect( receiver, [ count ]( int, int ) mutable {
		std::cout << "  Lambda 2: Click #" << ++count << "\n";
	} );

	// debug: check connections were created
	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	auto info = inspector.getEventInfo();
	std::cout << "Connections: " << info.connectionCount << ", Active: " << info.activeConnectionCount << "\n";

	// trigger events
	button->click( 1, 2 );
	button->click( 3, 4 );

	std::cout << "Freeing receiver\n";
	receiver.reset();

	std::cout << "\nCalling button->click(5, 6) (should not handle)...\n";
	button->click( 5, 6 );

	// count was captured by value into the lambda, so it cannot be observed here
}

// ============================================================================
// Example 3: Single-Shot Connections
// ============================================================================

void example3_singleshot()
{
	std::cout << "\n=== Example 3: Single-Shot ===\n";

	auto button = std::make_shared< Button >();
	auto receiver = std::make_shared< pulsar::Object >();

	button->clicked.connect( receiver, []( int, int ) {
		std::cout << "  Regular: Called every time\n";
	} );

	button->clicked.connectOnce( receiver, []( int, int ) {
		std::cout << "  Once: Called only once!\n";
	} );

	std::cout << "Click 1:\n";
	button->click( 1, 1 );

	std::cout << "Click 2:\n";
	button->click( 2, 2 );

	std::cout << "Freeing receiver\n";
	receiver.reset();

	std::cout << "\nCalling button->click(5, 6) (should not handle)...\n";
	button->click( 5, 6 );
}

// ============================================================================
// Example 4: Conditional Connections
// ============================================================================

void example4_conditional()
{
	std::cout << "\n=== Example 4: Conditional ===\n";

	auto button = std::make_shared< Button >();
	auto receiver = std::make_shared< pulsar::Object >();

	button->clicked.connectIf(
		receiver,
		[]( int x, int y ) {
			std::cout << "  First quadrant: " << x << ", " << y << "\n";
		},
		[]( int x, int y ) { return x > 0 && y > 0; }
	);

	button->click( 10, 20 );   // processed
	button->click( -5, 10 );   // ignored
	button->click( 15, 25 );   // processed

	std::cout << "Freeing receiver\n";
	receiver.reset();

	std::cout << "\nCalling button->click(20, 30) (should not handle)...\n";
	button->click( 20, 30 );
}

// ============================================================================
// Example 5: Priority Connections
// ============================================================================

void example5_priority()
{
	std::cout << "\n=== Example 5: Priority ===\n";

	auto button = std::make_shared< Button >();
	auto receiver = std::make_shared< pulsar::Object >();

	std::cout << "Connecting LOW priority...\n";
	button->clicked.connectWithPriority(
		receiver,
		[]( int, int ) { std::cout << "  Priority LOW (-100)\n"; },
		-100
	);

	std::cout << "Connecting NORMAL priority...\n";
	button->clicked.connectWithPriority(
		receiver,
		[]( int, int ) { std::cout << "  Priority NORMAL (0)\n"; },
		0
	);

	std::cout << "Connecting HIGH priority...\n";
	button->clicked.connectWithPriority(
		receiver,
		[]( int, int ) { std::cout << "  Priority HIGH (100)\n"; },
		100
	);

	std::cout << "Clicking (notice execution order):\n";
	button->click( 42, 42 );

	std::cout << "Example 5 complete\n";
}

// ============================================================================
// Example 6: Event with Recording
// ============================================================================

void example6_recording()
{
	std::cout << "\n=== Example 6: Recording ===\n";

	auto button = std::make_shared< Button >();
	auto handler = std::make_shared< ClickHandler >();

	// use RecordableEvent for recording capabilities
	pulsar::RecordableEvent< int, int > recordableClicked{ button.get() };

	recordableClicked.connect( handler, &ClickHandler::onClicked );

	std::cout << "Recording clicks:\n";
	recordableClicked.recorder().startRecording();

	recordableClicked( 10, 10 );
	recordableClicked( 20, 20 );
	recordableClicked( 30, 30 );

	recordableClicked.recorder().stopRecording();

	std::cout << "\nRecorded " << recordableClicked.recorder().recordingCount() << " emissions\n";

	std::cout << "\nReplaying:\n";
	recordableClicked.recorder().replay();
}

// ============================================================================
// Example 7: Queued Connections
// ============================================================================

class Worker : public pulsar::Object
{
public:
	void processData( int x, int y )
	{
		std::cout
			<< "  Worker on thread " << std::this_thread::get_id()
			<< ": (" << x << ", " << y << ")\n";
	}
};

void example7_queued()
{
	std::cout << "\n=== Example 7: Queued Connections ===\n";
	std::cout << "Main thread: " << std::this_thread::get_id() << "\n";

	pulsar::EventLoop loop = pulsar::EventLoop::makeAutoProcessed();
	loop.start();

	auto button = std::make_shared< Button >();
	auto worker = std::make_shared< Worker >();
	worker->setEventLoop( &loop );

	// auto connection - will be queued
	button->clicked.connect( worker, &Worker::processData, pulsar::ConnectionType::Auto );

	button->click( 100, 200 );
	button->click( 300, 400 );

	std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

	loop.stop();
}

// ============================================================================
// Example 8: Combining Events
// ============================================================================

class Validator : public pulsar::Object
{
public:
	pulsar::CombiningEvent< bool, pulsar::Combiners::LogicalAnd<>, std::string > validate{ this };
};

class LengthChecker : public pulsar::Object
{
public:
	bool checkLength( std::string input )
	{
		bool valid = input.length() >= 3;
		std::cout << "  Length check: " << ( valid ? "PASS" : "FAIL" ) << "\n";
		return valid;
	}
};

class ContentChecker : public pulsar::Object
{
public:
	bool checkContent( std::string input )
	{
		bool valid = !input.empty() && std::isalpha( input[ 0 ] );
		std::cout << "  Content check: " << ( valid ? "PASS" : "FAIL" ) << "\n";
		return valid;
	}
};

void example8_combining()
{
	std::cout << "\n=== Example 8: Combining Events ===\n";

	auto validator = std::make_shared< Validator >();
	auto lengthChecker = std::make_shared< LengthChecker >();
	auto contentChecker = std::make_shared< ContentChecker >();

	validator->validate.connect( lengthChecker, &LengthChecker::checkLength );
	validator->validate.connect( contentChecker, &ContentChecker::checkContent );

	std::cout << "Validating 'Hi':\n";
	bool result1 = validator->validate.emit( "Hi" );
	std::cout << "Result: " << ( result1 ? "VALID" : "INVALID" ) << "\n";

	std::cout << "\nValidating 'Hello':\n";
	bool result2 = validator->validate.emit( "Hello" );
	std::cout << "Result: " << ( result2 ? "VALID" : "INVALID" ) << "\n";
}

// ============================================================================
// Example 9: Scoped Connections
// ============================================================================

void example9_scoped()
{
	std::cout << "\n=== Example 9: Scoped Connections ===\n";

	auto button = std::make_shared< Button >();
	auto handler = std::make_shared< ClickHandler >();

	std::cout << "Creating scoped connection:\n";
	{
		// create a scoped connection
		auto scoped = button->clicked.connect( handler, &ClickHandler::onClicked ).scoped();

		std::cout << "Inside scope:\n";
		button->click( 1, 1 );

		std::cout << "Leaving scope (scoped connection will disconnect)...\n";
		// scoped goes out of scope here and disconnects
	}

	std::cout << "Outside scope (connection disconnected, should not handle):\n";
	button->click( 2, 2 );  // Should not trigger handler

	// verify the connection is gone
	auto inspector = kmac::pulsar::EventInspector( button->clicked );
	auto info = inspector.getEventInfo();
	std::cout << "Active connections: " << info.activeConnectionCount << "\n";
}

// ============================================================================
// Example 10: Connection Groups (SIMPLER FIX)
// ============================================================================

class Widget : public pulsar::Object
{
private:
	pulsar::ConnectionGroup _connections;

public:
	Widget() = default;

	// setup connections after construction
	void connectToButton( std::shared_ptr< Button > button )
	{
		_connections += button->clicked.connect(
			shared_from_this(),
			[]( int x, int y ) {
				std::cout << "  Widget handler 1: " << x << ", " << y << "\n";
			}
		);

		_connections += button->clicked.connect(
			shared_from_this(),
			[]( int x, int y ) {
				std::cout << "  Widget handler 2: " << x << ", " << y << "\n";
			}
		);
	}

	~Widget()
	{
		std::cout << "  Widget destroyed - auto-disconnecting\n";
	}
};

void example10_groups()
{
	std::cout << "\n=== Example 10: Connection Groups ===\n";

	auto button = std::make_shared< Button >();

	{
		auto widget = std::make_shared< Widget >();
		widget->connectToButton( button );  // connect after construction

		std::cout << "Widget alive:\n";
		button->click( 10, 10 );
	}

	std::cout << "\nWidget destroyed (should not handle):\n";
	button->click( 20, 20 );
}

// ============================================================================
// Main
// ============================================================================

int main()
{
	std::cout << "=================================================\n";
	std::cout << "Pulsar Event Library Examples\n";
	std::cout << "Version: " << PULSAR_VERSION_STRING << "\n";
	std::cout << "=================================================\n";

	example1_basic();
	example2_lambdas();
	example3_singleshot();
	example4_conditional();
	example5_priority();
	example6_recording();
	example7_queued();
	example8_combining();
	example9_scoped();
	example10_groups();

	std::cout << "\n=================================================\n";
	std::cout << "All examples completed!\n";
	std::cout << "=================================================\n";

	return 0;
}
