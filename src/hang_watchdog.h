#ifndef BSSH_HANG_WATCHDOG_H
#define BSSH_HANG_WATCHDOG_H

#include <atomic>

#include <QtGlobal>

/*
 * A watchdog for a wedged main thread (project.md sec 5.1, sec 5.6).
 *
 * The failure is upstream Qt's AndroidDeadlockProtector wedging the UI
 * thread, which nothing on that thread can then clear -- which is why the
 * foreground service's STOP never runs and the process stays alive and
 * unstartable. This is the DIAGNOSTIC half of the fix that sec 5.1
 * designs: a thread that is NOT the wedged one notices the main loop has
 * stopped beating, and raises SIGQUIT at the process, which on Android is
 * ART's SignalCatcher dumping every thread's
 * stack to logcat and /data/anr -- finally capturing the "already locked
 * by '<HOLDER>'" line the manual repro could not provoke.
 *
 * It does NOT terminate the process. The _exit() that would make the hang
 * self-heal is the other half, held until this half has proven on a real
 * occurrence that the gate fires when it should and the stack names the
 * holder -- because a process-killer is the one addition where being
 * wrong is worse than the bug (sec 5.1's recorded decision).
 */

/*
 * Whether the watchdog should dump: the main loop has not beaten for at
 * least the threshold, in front of the user or not. This asked for the
 * foreground as well, because Qt used to stop the loop on suspend. Since
 * android.app.background_running it does not, so silence in the background
 * is a wedge too, and on the Fold (sec 5.6) that is where one hid: a
 * session's drop went unhandled with nothing logged. A dump harms nothing.
 * The _exit() half kills, and it must still ask for the foreground.
 */
bool bssh_hang_should_dump(qint64 idle_ms, qint64 threshold_ms);

/*
 * Whether the loop has been silent long enough to try freeing it from Qt's
 * wait for a surface (sec 5.6). Well before the dump, and asked again at every
 * poll, because this one is a cure and not a report: the main thread is what
 * handles a dropped link, and it is doing nothing until this works.
 */
bool bssh_hang_should_free_surface(qint64 idle_ms);

/*
 * The two facts the watchdog reads, written from two threads and read
 * from a third, so atomics rather than a lock: a lock is the one thing a
 * watchdog must not take, since the wedged thread could hold it.
 *
 * FOREGROUND IS THE PLATFORM'S LIFECYCLE, NOT QT'S STATE (sec 5.1). It
 * no longer gates the dump; it says which side a wedge happened on, and the
 * _exit() half will need it. Qt's state is no use for that: it reaches its
 * thread asynchronously, after the window work where the deadlock is
 * suspected, so a thread wedged there never records the resume.
 *
 * Android's ActivityLifecycleCallbacks fire on its own UI thread, from
 * super.onResume() -- before QtActivityBase does any of its resume work --
 * and from onPause(), with no Qt event loop in between.
 */
class bssh_hang_state {
public:
	/* The main loop is alive. From the heartbeat, on the Qt thread. */
	void beat(qint64 now_ms);

	/*
	 * The window went in front of the user or away from them. From the
	 * platform's lifecycle, on whatever thread it reports on.
	 *
	 * Coming forward also restarts the clock: a process Android froze
	 * while away has not beaten, and must not be charged for that on its
	 * way back.
	 */
	void set_foreground(bool foreground, qint64 now_ms);

	bool should_dump(qint64 now_ms, qint64 threshold_ms) const;

	/* How long the loop has been silent, for the log. */
	qint64 idle_ms(qint64 now_ms) const;

	bool foreground() const { return m_foreground.load(); }

private:
	std::atomic<qint64> m_last_beat_ms{0};
	std::atomic<bool> m_foreground{false};
};

/*
 * Starts the watchdog on Android and does nothing anywhere else -- on a
 * desktop SIGQUIT's default action is to dump core and terminate, so this
 * must never arm off the platform whose ART turns it into a stack dump.
 * It also does not arm where the platform cannot report its lifecycle
 * (bssh_platform::watch_foreground), since without that the gate has
 * nothing true to read. Called once, after QApplication exists and the
 * window is shown, from the thread that owns it.
 */
void bssh_start_hang_watchdog();

#endif /* BSSH_HANG_WATCHDOG_H */
