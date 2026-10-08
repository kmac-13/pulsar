#pragma once
#ifndef KMAC_STELLYRA_AUTO_DRAIN_THREAD_H
#define KMAC_STELLYRA_AUTO_DRAIN_THREAD_H

/**
 * @file auto_drain_thread.h
 * @brief Background thread that drains an EventLoop automatically.
 *
 * @section overview Overview
 *
 * AutoDrainThread wraps an externally-owned EventLoop with a dedicated
 * background thread.  The thread sleeps until work is posted, drains the
 * queue, then sleeps again - no polling.
 *
 * @section usage Usage
 *
 * @code
 * EventLoop loop;
 * AutoDrainThread drainer( loop );  // initializes and starts the drain thread
 *
 * // from any thread: post tasks or trigger Deferred events;
 * // the drain thread wakes automatically and processes them
 *
 * // drainer.stop() on scope exit (or let the destructor do it).
 * @endcode
 *
 * @section lifetime Lifetime
 *
 * The EventLoop must outlive the AutoDrainThread.  stop() (or the
 * destructor) wakes the drain thread, has it drain one final time so
 * nothing already posted is left behind, then joins it and clears the
 * notification hook from the EventLoop.
 *
 * @section single_loop One drainer per loop
 *
 * Only one AutoDrainThread may be attached to a given EventLoop at a time.
 * Attaching a second one would overwrite the notification hook silently.
 *
 * @section threading Thread safety
 *
 * post() and migratePendingTo() are thread-safe.  stop() may be called
 * from any thread; subsequent calls after the first are no-ops.
 */

#include "platform.h"

#include "callable.h"
#include "event_loop.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace kmac {
namespace stellyra {

class AutoDrainThread
{
private:
	EventLoop& _loop;                        ///< the loop this thread drains; must outlive this object
	std::atomic< bool > _running { false };  ///< false once stop() has been called; makes stop() idempotent

	// guards _notified and the condition variable
	std::mutex _cvMutex;
	std::condition_variable _cv;
	bool _notified = false;  // set by notify(), cleared by threadLoop()

	std::thread _thread;     ///< the dedicated drain thread; joined by stop()

public:
	/**
	 * @brief Attach to loop and start the drain thread immediately.
	 *
	 * Registers a post-notification hook on loop so that every post()
	 * call wakes the drain thread.  Also calls loop.setDrainThread() so
	 * that Auto connections resolve correctly.
	 *
	 * @param loop the EventLoop to drain, must outlive this object
	 */
	explicit AutoDrainThread( EventLoop& loop );

	/**
	 * @brief Stops the drain thread and detaches from the EventLoop.
	 *
	 * Also drains the loop.
	 */
	~AutoDrainThread();

	AutoDrainThread( const AutoDrainThread& ) = delete;
	AutoDrainThread& operator=( const AutoDrainThread& ) = delete;
	AutoDrainThread( AutoDrainThread&& ) = delete;
	AutoDrainThread& operator=( AutoDrainThread&& ) = delete;

	// -------------------------------------------------------------------------

	/**
	 * @brief True until stop() has been called (or is in progress).
	 */
	bool isRunning() const noexcept;

	/**
	 * @brief Stop the drain thread and detach from the EventLoop.
	 *
	 * Signals the drain thread to wake, drain one final time (picking up
	 * anything still pending, so nothing posted before stop() is silently
	 * abandoned), then exit; joins it, then clears the notification hook.
	 * Safe to call from any thread.  Subsequent calls are no-ops.
	 *
	 * Note: a post() that races with stop() - arriving after this final
	 * drain has already started - is not guaranteed to be picked up before
	 * the thread exits.  Don't post to a loop you are concurrently stopping.
	 */
	void stop();

private:
	/**
	 * @brief Called by EventLoop::post() via the _postNotify hook.
	 */
	void notify();

	/**
	 * @brief Drain thread entry point.
	 */
	void threadLoop();
};

// ---------------------------------------------------------------------------

inline AutoDrainThread::AutoDrainThread( EventLoop& loop )
	: _loop( loop )
{
	_running.store( true, std::memory_order_relaxed );

	// install the notification hook before starting the thread so no post()
	// call between construction and thread start is missed
	_loop.setPostNotify(
		Callable< void() >::create< &AutoDrainThread::notify >( this ) );

	_thread = std::thread( &AutoDrainThread::threadLoop, this );
}

inline AutoDrainThread::~AutoDrainThread()
{
	stop();
}

inline bool AutoDrainThread::isRunning() const noexcept
{
	return _running.load( std::memory_order_relaxed );
}

inline void AutoDrainThread::stop()
{
	// check if already stopped
	if ( ! _running.exchange( false, std::memory_order_acq_rel ) )
	{
		return;
	}

	// wake the thread so it sees _running == false and exits
	{
		std::lock_guard< std::mutex > lock( _cvMutex );
		_notified = true;
	}
	_cv.notify_one();

	if ( _thread.joinable() )
	{
		_thread.join();
	}

	// detach the notification hook and drain thread registration so future
	// post() calls and operator() affinity checks behave as if no drainer
	// is attached - preventing silent task accumulation after stop()
	_loop.clearPostNotify();
	_loop.clearDrainThread();
}

inline void AutoDrainThread::notify()
{
	{
		std::lock_guard< std::mutex > lock( _cvMutex );
		_notified = true;
	}
	_cv.notify_one();
}

inline void AutoDrainThread::threadLoop()
{
	// register from within the thread so the ID reflects the actual drain
	// thread, not the constructing thread
	_loop.setDrainThread( platform::currentThreadId() );

	while ( true )
	{
		{
			std::unique_lock< std::mutex > lock( _cvMutex );
			_cv.wait( lock, [ this ] { return _notified; } );
			_notified = false;
		}

		// always drain on wake, whether the notification came from a real
		// post() or from stop() - this guarantees any tasks still pending
		// when stop() is called get processed before the thread exits,
		// rather than being silently abandoned in the loop's queue
		_loop.drain();

		if ( ! _running.load( std::memory_order_relaxed ) )
		{
			break;
		}
	}
}

} // namespace stellyra
} // namespace kmac

#endif // KMAC_STELLYRA_AUTO_DRAIN_THREAD_H
