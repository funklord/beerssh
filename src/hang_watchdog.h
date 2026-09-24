#ifndef BSSH_HANG_WATCHDOG_H
#define BSSH_HANG_WATCHDOG_H

#include <atomic>

#include <QtGlobal>

/*
 * A watchdog for the last-terminal-close hang (project.md sec 5.1).
 *
 * The failure is upstream Qt's AndroidDeadlockProtector wedging the UI
 * thread, which nothing on that thread can then clear -- which is why the
 * foreground service's STOP never runs and the process stays alive and
 * unstartable. This is the DIAGNOSTIC half of the fix that sec 5.1
 * designs: a thread that is NOT the wedged one notices the main loop has
 * stopped beating while the app is foreground, and raises SIGQUIT at the
 * process, which on Android is ART's SignalCatcher dumping every thread's
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
 * Whether the watchdog should dump: the app is FOREGROUND and the main
 * loop has not beaten for at least the threshold. A pure function so the
 * gate -- the whole risk -- is tested rather than reasoned about. A
 * backgrounded app whose loop Qt has suspended is not a hang, which is
 * why foreground is a precondition rather than a detail.
 */
bool bssh_hang_should_dump(bool foreground, qint64 idle_ms, qint64 threshold_ms);

/*
 * The two facts the watchdog reads, written from two threads and read
 * from a third, so atomics rather than a lock: a lock is the one thing a
 * watchdog must not take, since the wedged thread could hold it.
 *
 * FOREGROUND IS THE PLATFORM'S LIFECYCLE, NOT QT'S STATE (sec 5.1). It
 * was read from QGuiApplication::applicationState() inside the heartbeat,
 * and on Android that is wrong in both directions:
 *
 *   - On pause, Qt stops delivering TIMERS before it posts the state
 *     change (QAndroidEventDispatcher::goingToStop), so the beat stopped
 *     with "foreground" as its last word -- and every trip to the
 *     background longer than the threshold read as a hang. Harmless as a
 *     stack dump; a live session killed once the _exit() half lands.
 *   - On resume, Qt's state reaches its thread asynchronously, after the
 *     window work where the deadlock is suspected. A thread wedged there
 *     never records the resume, so the one hang this exists for would
 *     read as "backgrounded" and never fire.
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
	 * Coming forward also restarts the clock. The loop has not beaten
	 * since the pause -- timers were stopped -- so without the restart a
	 * healthy resume after a long background would be read as that whole
	 * background spent wedged, and dumped before its first beat.
	 */
	void set_foreground(bool foreground, qint64 now_ms);

	bool should_dump(qint64 now_ms, qint64 threshold_ms) const;

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
