#ifndef BSSH_HANG_WATCHDOG_H
#define BSSH_HANG_WATCHDOG_H

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
 * Whether the watchdog should dump: the app was FOREGROUND at the last
 * beat and the main loop has not beaten for at least the threshold. A
 * pure function so the gate -- the whole risk -- is tested rather than
 * reasoned about. A backgrounded app whose loop Qt has suspended is not a
 * hang, which is why foreground is a precondition rather than a detail.
 */
bool bssh_hang_should_dump(bool foreground_at_last_beat, qint64 idle_ms,
                            qint64 threshold_ms);

/*
 * Starts the watchdog on Android and does nothing anywhere else -- on a
 * desktop SIGQUIT's default action is to dump core and terminate, so this
 * must never arm off the platform whose ART turns it into a stack dump.
 * Called once, after QApplication exists, from the thread that owns it.
 */
void bssh_start_hang_watchdog();

#endif /* BSSH_HANG_WATCHDOG_H */
