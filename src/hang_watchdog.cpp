#include "hang_watchdog.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <signal.h>
#include <unistd.h>

#ifdef Q_OS_ANDROID
#include <android/log.h>
#endif

#include <QGuiApplication>
#include <QTimer>

#include "platform/platform.h"

namespace {

/*
 * 30 seconds -- deliberately past Android's own 10s foreground-service
 * ANR, so this speaks only for a freeze the system has already given up
 * on, never for a slow moment. The SSH transport runs on worker threads
 * (sec 5.2), so a main loop that has not beaten in 30s is wedged, not
 * busy.
 */
const qint64 BSSH_HANG_THRESHOLD_MS = 30000;

/* How often the main loop says it is alive, and how often the watchdog
 * looks -- the look is coarser than the beat so a single missed beat is
 * never mistaken for a stall. */
const int BSSH_HANG_BEAT_MS = 1000;
const int BSSH_HANG_POLL_MS = 2000;

/* The one the running app uses; see bssh_hang_state for who writes it. */
bssh_hang_state g_state;

/*
 * steady_clock is CLOCK_MONOTONIC, which stands still while the phone sleeps, so
 * a sleeping phone is not a silent loop.
 */
qint64 now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/*
 * Straight to the log and not through qWarning, whose handler takes a lock the
 * wedged thread could hold. The two lines give when a wedge began, how long it
 * lasted, and with the lifecycle beside them what ended it.
 */
void note_silence(const char *what, qint64 idle_ms, bool foreground) {
#ifdef Q_OS_ANDROID
	__android_log_print(ANDROID_LOG_WARN, "beerssh", "main loop %s after %lld ms, %s", what,
	                    static_cast<long long>(idle_ms),
	                    foreground ? "foreground" : "background");
#else
	Q_UNUSED(what);
	Q_UNUSED(idle_ms);
	Q_UNUSED(foreground);
#endif
}

} /* namespace */

bool bssh_hang_should_dump(qint64 idle_ms, qint64 threshold_ms) {
	return idle_ms >= threshold_ms;
}

void bssh_hang_state::beat(qint64 now_ms) {
	m_last_beat_ms.store(now_ms);
}

void bssh_hang_state::set_foreground(bool foreground, qint64 now_ms) {
	/*
	 * The clock before the flag: the log must never pair "foreground"
	 * with a beat from before the pause.
	 */
	if (foreground) m_last_beat_ms.store(now_ms);
	m_foreground.store(foreground);
}

bool bssh_hang_state::should_dump(qint64 now_ms, qint64 threshold_ms) const {
	return bssh_hang_should_dump(idle_ms(now_ms), threshold_ms);
}

qint64 bssh_hang_state::idle_ms(qint64 now_ms) const {
	return now_ms - m_last_beat_ms.load();
}

void bssh_start_hang_watchdog() {
	const bssh_platform *platform = bssh_platform::instance();
	if (!platform || platform->name() != QStringLiteral("android")) return;

	if (!QCoreApplication::instance()) return;

	/*
	 * Qt's state is read ONCE, here, for the starting value: this runs
	 * after show(), on a loop that is plainly running, so it is the one
	 * moment Qt's answer and the lifecycle's cannot disagree. Every
	 * change after it comes from the lifecycle -- see bssh_hang_state.
	 */
	g_state.set_foreground(QGuiApplication::applicationState() == Qt::ApplicationActive,
	                       now_ms());

	const bool watching = bssh_platform::instance()->watch_foreground([](bool foreground) {
		g_state.set_foreground(foreground, now_ms());
	});
	if (!watching) return;

	/*
	 * The heartbeat. Parented to the application so it lives as long as
	 * the loop it measures; when the loop is wedged it simply stops
	 * firing, which is exactly the signal.
	 */
	QTimer *beat = new QTimer(QCoreApplication::instance());
	beat->setInterval(BSSH_HANG_BEAT_MS);
	QObject::connect(beat, &QTimer::timeout, [] { g_state.beat(now_ms()); });
	beat->start();

	/*
	 * The watchdog. Detached and never joined: it is meant to outlive
	 * every other thread and stop only when the process does, which is
	 * its termination condition. It takes no lock and touches no Qt
	 * object, so a wedged main thread cannot wedge it too.
	 */
	std::thread([] {
		bool dumped = false;
		qint64 silent_since_ms = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(BSSH_HANG_POLL_MS));

			const qint64 now = now_ms();
			if (!g_state.should_dump(now, BSSH_HANG_THRESHOLD_MS)) {
				/* Recovered, or never stalled: re-arm for the next time. */
				if (dumped) {
					note_silence("beat again", now - silent_since_ms,
					             g_state.foreground());
				}
				dumped = false;
				continue;
			}

			if (dumped) continue;
			dumped = true;
			silent_since_ms = now - g_state.idle_ms(now);
			note_silence("silent", now - silent_since_ms, g_state.foreground());

			/*
			 * ART's SignalCatcher answers SIGQUIT by dumping every
			 * thread's stack to logcat and /data/anr and then letting
			 * the process continue -- so this captures the holder line
			 * without killing anything. The _exit() that would recover
			 * the process is deliberately not here yet (sec 5.1).
			 */
			kill(getpid(), SIGQUIT);
		}
	}).detach();
}
