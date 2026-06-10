#ifndef KMAC_PULSAR_EVENT_LOOP_H
#define KMAC_PULSAR_EVENT_LOOP_H

/**
 * @file event_loop.h
 * @brief Thread-safe event queue with self-managed and externally-managed modes.
 *
 * EventLoop is the backbone of Pulsar's cross-thread dispatch.  When a Deferred
 * connection fires, the handler invocation is packaged as a CallableBase and
 * posted to the receiver's EventLoop.  The loop then executes it on whatever
 * thread drains the queue.
 *
 * Two operating modes are available via the static factory methods:
 *
 * - **makeAutoProcessed()**:
 *   spawns a dedicated background thread; call start() / stop() to control it
 *
 * - **makeManualProcessed()**:
 *   no background thread; the owner is responsible for calling processEvents() regularly
 *
 * In both modes, postEvent() is safe to call from any thread.
 */

#include "pulsar_fwd.h"
#include "config.h"

#include <atomic>
#include <memory>
#include <queue>
#include <vector>

// std::thread and std::condition_variable are only needed in self-managed
// (auto-processed) mode.  Define PULSAR_ENABLE_THREAD=0 to exclude them,
// which allows compilation on targets where <thread> is unavailable
// (bare-metal, some embedded toolchains).  makeAutoProcessed() is then
// disabled via static_assert.
#ifndef PULSAR_ENABLE_THREAD
#	define PULSAR_ENABLE_THREAD 1
#endif

#if PULSAR_ENABLE_THREAD
#	include <condition_variable>
#	include <thread>
#endif

namespace kmac {
namespace pulsar {

// ============================================================================
// CallableBase / CallableWrapper
// ============================================================================

/**
 * @brief Abstract unit of work stored in an EventLoop queue.
 *
 * Each deferred handler invocation is wrapped in a CallableWrapper<Func> that
 * derives from this class.  EventLoop stores and invokes them through this
 * interface, keeping the queue type-erased.
 */
class CallableBase
{
public:
	virtual ~CallableBase() = default;

	/**
	 * @brief Returns the Object that will receive this callable, or nullptr for
	 * free-function callables.
	 *
	 * Used by Object::setEventLoop() to extract pending callables that belong to
	 * a specific receiver during loop migration.
	 */
	virtual Object* getReceiver() const;

	/**
	 * @brief Execute the stored callable.
	 */
	virtual void invoke() = 0;
};

/**
 * @brief Concrete callable wrapper that stores a functor and its receiver.
 *
 * @tparam Func any callable type compatible with void()
 */
template< typename Func >
class CallableWrapper : public CallableBase
{
private:
	Func _func;
	Object* _receiver;  ///< raw pointer - only used for identity comparison during migration

public:
	/**
	 * @param func callable to invoke, moved/forwarded into storage
	 * @param receiver Object that owns this handler (nullptr for free functions)
	 */
	CallableWrapper( Func&& func, Object* receiver = nullptr );

	Object* getReceiver() const override;

	void invoke() override;
};

// ============================================================================
// EventLoop
// ============================================================================

/**
 * @brief Thread-safe event queue with two operating modes.
 *
 * **Self-managed mode** (created via makeAutoProcessed()):
 *   A dedicated background thread is spawned when start() is called.  It
 *   blocks on an internal condition variable and drains the queue as events
 *   arrive.  Call stop() to shut it down and join the thread.  Use this for
 *   worker objects that should live permanently on their own thread.
 *
 * **Externally-managed mode** (created via makeManualProcessed()):
 *   No background thread is created.  postEvent() pushes to the queue but
 *   does not notify any condition variable.  The owner is responsible for
 *   calling processEvents() at appropriate points (e.g. every frame, after a
 *   platform event pump yields, etc.).  Use this for (e.g.) the main thread,
 *   UI thread, render thread, or any existing thread that already has its own
 *   scheduling loop.
 *
 * In both modes the queue is fully thread-safe: postEvent() may be called from
 * any thread at any time.  processEvents() should only be called from the
 * owning thread in external mode; in self-managed mode it is called internally
 * and should not be called from user code while the loop is running.
 *
 * **Auto connection resolution** uses EventLoop pointer identity: two objects
 * are considered "on the same loop" if and only if their eventLoop() pointers
 * are equal and non-null.
 *
 * @note EventLoop is move-constructible but not copyable or move-assignable.
 * Moving is only safe before start() is called.
 *
 * @code
 * // self-managed (dedicated thread)
 * auto loop = EventLoop::makeAutoProcessed();
 * loop.start();
 * receiver->setEventLoop( &loop );
 * // ... work ...
 * loop.stop();
 *
 * // externally-managed (caller drives processing)
 * auto loop = EventLoop::makeManualProcessed();
 * receiver->setEventLoop( &loop );
 * while ( running ) {
 *     loop.processEvents();
 *     // ... other per-frame work ...
 * }
 * @endcode
 */
class EventLoop
{
public:
#if PULSAR_ENABLE_THREAD
	/**
	 * @brief Create an EventLoop that manages its own background thread that
	 * handles draining the queue.
	 *
	 * Call start() to begin processing and stop() to shut down.
	 * Use this for worker objects that should live on a dedicated thread.
	 *
	 * @note Only available when PULSAR_ENABLE_THREAD is set (the default).
	 */
	static EventLoop makeAutoProcessed();
#endif // PULSAR_ENABLE_THREAD

	/**
	 * @brief Create an EventLoop driven by the caller.
	 *
	 * No background thread is spawned.  Call processEvents() from your
	 * owning thread whenever you want to drain deferred events - e.g. once
	 * per frame, after a platform event pump yields, etc.
	 *
	 * Use this for (e.g.) the main thread, UI thread, render thread, or any
	 * existing thread that already has its own scheduling loop.
	 */
	static EventLoop makeManualProcessed();

private:
	std::queue< std::unique_ptr< CallableBase > > _eventQueue;
	platform::Mutex _queueMutex;

#if PULSAR_ENABLE_THREAD
	const bool _selfManaged;         ///< true for auto-processed mode
	std::atomic< bool > _running;
	std::thread _thread;
	std::condition_variable _cv;
#endif

	explicit EventLoop( bool selfManaged );

public:
	/**
	 * @brief Move constructor, only safe to call before start().
	 */
	EventLoop( EventLoop&& other ) noexcept;

	EventLoop( const EventLoop& ) = delete;
	EventLoop& operator=( const EventLoop& ) = delete;
	EventLoop& operator=( EventLoop&& ) = delete;

	/**
	 * @brief Destructor, calls stop() automatically in self-managed mode.
	 */
	~EventLoop();

#if PULSAR_ENABLE_THREAD
	// -------------------------------------------------------------------------
	// Self-managed mode API (requires PULSAR_ENABLE_THREAD)
	// -------------------------------------------------------------------------

	/**
	 * @brief Spawn the background thread and begin draining the queue.
	 *
	 * Only valid in self-managed mode (makeAutoProcessed()).  Calling start()
	 * on an already-running loop is a no-op.
	 */
	void start();

	/**
	 * @brief Signal the background thread to stop and join it.
	 *
	 * Only valid in self-managed mode.  Blocks until the thread exits.
	 * Calling stop() when not running is a no-op.
	 */
	void stop();

	/**
	 * @brief Returns true if this loop was created in self-managed mode.
	 */
	bool isManagedInternally() const;
#endif // PULSAR_ENABLE_THREAD

	// -------------------------------------------------------------------------
	// Shared API
	// -------------------------------------------------------------------------

	/**
	 * @brief Post a callable to the queue from any thread.
	 *
	 * In self-managed mode, the background thread is notified via the internal
	 * condition variable.  In external mode, the task is deferred silently;
	 * the owner must call processEvents() to drain it.
	 *
	 * @param task callable to execute on the loop's thread
	 * @param receiver Object that owns this task (used during loop migration)
	 */
	template< typename Func >
	void postEvent( Func&& task, Object* receiver = nullptr );

	/**
	 * @brief Drain all currently deferred tasks on the calling thread.
	 *
	 * Swaps the internal queue to a local queue under the lock, then executes
	 * all pending callables outside the lock.  New events posted while
	 * executing will be processed on the next call.
	 *
	 * In external mode: call this regularly from your owning thread.
	 * In self-managed mode: safe to call when the loop is not running (e.g.
	 * before start() or after stop()), but do not call it while the background
	 * thread is active.
	 */
	void processEvents();

	// -------------------------------------------------------------------------
	// Event migration (used internally by Object::setEventLoop)
	// -------------------------------------------------------------------------

	/**
	 * @brief Remove and return all deferred events belonging to @p receiver.
	 *
	 * Called by Object::setEventLoop() during loop migration to transfer
	 * pending tasks from the old loop to the new one.  All remaining events
	 * (belonging to other receivers) stay in the queue.
	 *
	 * @param receiver the Object whose tasks should be extracted
	 * @return vector of extracted tasks in original queue order
	 */
	std::vector< std::unique_ptr< CallableBase > > extractEventsFor( Object* receiver );

	/**
	 * @brief Append a batch of pre-built tasks to the queue.
	 *
	 * Used by Object::setEventLoop() to insert migrated tasks into the new
	 * loop.  In self-managed mode the background thread is notified if any
	 * tasks were appended.
	 *
	 * @param tasks tasks to append; nullptrs in the vector are skipped
	 */
	void appendEvents( std::vector< std::unique_ptr< CallableBase > > tasks );

private:
	/**
	 * @brief Execute all tasks in @p queue sequentially.  Called without holding any lock.
	 */
	void executeEvents( std::queue< std::unique_ptr< CallableBase > >& queue );

#if PULSAR_ENABLE_THREAD
	/**
	 * @brief Background thread entry point (self-managed mode only).
	 */
	void run();
#endif
};


// ============================================================================
// Drain context tracking
// ============================================================================

/**
 * @brief Pointer to the EventLoop currently draining on this thread.
 *
 * Set to the loop's address for the duration of executeEvents() and cleared
 * (restored) on exit.  Event::operator() compares the sender's associated
 * loop against this value to decide whether to emit directly or defer:
 *
 * - nullptr or a different loop pointer -> defer to sender's loop
 * - same pointer as sender's loop       -> emit directly (already in context)
 *
 * thread_local gives each thread its own independent value, so two loops
 * draining simultaneously on different threads never interfere.
 */
inline thread_local EventLoop* tls_drainingLoop = nullptr;

//
// IMPLEMENTATION
//

Object* CallableBase::getReceiver() const
{
	return nullptr;
}

template< typename Func >
CallableWrapper< Func >::CallableWrapper( Func&& func, Object* receiver )
	: _func( std::forward< Func >( func ) )
	, _receiver( receiver )
{
}

template< typename Func >
void CallableWrapper< Func >::invoke()
{
	_func();
}

template< typename Func >
Object* CallableWrapper< Func >::getReceiver() const
{
	return _receiver;
}


//
// EVENT LOOP
//

#if PULSAR_ENABLE_THREAD
EventLoop EventLoop::makeAutoProcessed()
{
	return EventLoop( true );
}
#endif // PULSAR_ENABLE_THREAD

EventLoop EventLoop::makeManualProcessed()
{
	return EventLoop( false );
}

#if PULSAR_ENABLE_THREAD
EventLoop::EventLoop( bool selfManaged )
	: _selfManaged( selfManaged )
	, _running( false )
{
}

EventLoop::EventLoop( EventLoop&& other ) noexcept
	: _selfManaged( other._selfManaged )
	, _running( false )
{
	// other must not be running - moving a live loop is undefined
}

EventLoop::~EventLoop()
{
	if ( _selfManaged )
	{
		stop();
	}
}
#else
EventLoop::EventLoop( bool )
{
}

EventLoop::EventLoop( EventLoop&& ) noexcept
{
}

EventLoop::~EventLoop()
{
}
#endif // PULSAR_ENABLE_THREAD

#if PULSAR_ENABLE_THREAD
void EventLoop::start()
{
	if ( ! _selfManaged )
	{
		return;
	}

	if ( ! _running.exchange( true ) )
	{
		_thread = std::thread( &EventLoop::run, this );
	}
}

void EventLoop::stop()
{
	if ( ! _selfManaged )
	{
		return;
	}

	if ( _running.exchange( false ) )
	{
		_cv.notify_one();
		if ( _thread.joinable() )
		{
			_thread.join();
		}
	}
}

bool EventLoop::isManagedInternally() const
{
	return _selfManaged;
}
#endif // PULSAR_ENABLE_THREAD

template< typename Func >
void EventLoop::postEvent( Func&& task, Object* receiver )
{
	{
		platform::LockGuard< platform::Mutex > lock( _queueMutex );
		_eventQueue.push( std::make_unique< CallableWrapper< Func > >( std::forward< Func >( task ), receiver ) );
	}

#if PULSAR_ENABLE_THREAD
	if ( _selfManaged )
	{
		_cv.notify_one();
	}
#endif
}

void EventLoop::processEvents()
{
	std::queue< std::unique_ptr< CallableBase > > localQueue;
	{
		platform::LockGuard< platform::Mutex > lock( _queueMutex );
		localQueue = std::move( _eventQueue );
	}

	executeEvents( localQueue );
}

std::vector< std::unique_ptr< CallableBase > > EventLoop::extractEventsFor( Object* receiver )
{
	std::vector< std::unique_ptr< CallableBase > > extracted;
	std::queue< std::unique_ptr< CallableBase > > remaining;

	platform::LockGuard< platform::Mutex > lock( _queueMutex );

	while ( ! _eventQueue.empty() )
	{
		auto& task = _eventQueue.front();
		if ( task && task->getReceiver() == receiver )
		{
			extracted.push_back( std::move( task ) );
		}
		else
		{
			remaining.push( std::move( task ) );
		}
		_eventQueue.pop();
	}

	_eventQueue = std::move( remaining );
	return extracted;
}

void EventLoop::appendEvents( std::vector< std::unique_ptr< CallableBase > > tasks )
{
	{
		platform::LockGuard< platform::Mutex > lock( _queueMutex );
		for ( auto& task : tasks )
		{
			if ( task )
			{
				_eventQueue.push( std::move( task ) );
			}
		}
	}

#if PULSAR_ENABLE_THREAD
	if ( _selfManaged && ! tasks.empty() )
	{
		_cv.notify_one();
	}
#endif
}

void EventLoop::executeEvents( std::queue< std::unique_ptr< CallableBase > >& queue )
{
	// RAII guard: record this loop as the draining context for this thread
	// for the duration of the drain; saves and restores the previous value
	// so nested calls (e.g. an event handler that manually calls processEvents
	// on another loop) work correctly
	EventLoop* previous = tls_drainingLoop;
	tls_drainingLoop = this;
	struct RestoreOnExit
	{
		EventLoop*& slot;
		EventLoop* saved;
		~RestoreOnExit() { slot = saved; }
	} guard { tls_drainingLoop, previous };

	while ( ! queue.empty() )
	{
		auto& task = queue.front();
		if ( task )
		{
			task->invoke();
		}
		queue.pop();
	}
}

#if PULSAR_ENABLE_THREAD
void EventLoop::run()
{
	while ( _running )
	{
		std::queue< std::unique_ptr< CallableBase > > localQueue;

		{
			platform::UniqueLock< platform::Mutex > lock( _queueMutex );
			_cv.wait( lock, [ this ] { return ! _eventQueue.empty() || ! _running; } );

			if ( ! _running )
			{
				break;
			}

			localQueue = std::move( _eventQueue );
		}

		executeEvents( localQueue );
	}
}
#endif // PULSAR_ENABLE_THREAD

} // namespace pulsar
} // namespace kmac

#endif // KMAC_PULSAR_EVENT_LOOP_H
