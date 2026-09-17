#include "hang_watchdog.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <signal.h>
#include <unistd.h>

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

/*
 * Written by the main thread's timer, read by the watchdog thread. Atomic
 * rather than locked, because a lock is the one thing a watchdog must not
 * take: the wedged thread could be holding it.
 */
std::atomic<qint64> g_last_beat_ms{0};
std::atomic<bool> g_foreground{false};

qint64 now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} /* namespace */

bool bssh_hang_should_dump(bool foreground_at_last_beat, qint64 idle_ms,
                            qint64 threshold_ms) {
	return foreground_at_last_beat && idle_ms >= threshold_ms;
}

void bssh_start_hang_watchdog() {
	const bssh_platform *platform = bssh_platform::instance();
	if (!platform || platform->name() != QStringLiteral("android")) return;

	if (!QCoreApplication::instance()) return;

	g_last_beat_ms.store(now_ms());
	g_foreground.store(QGuiApplication::applicationState() == Qt::ApplicationActive);

	/*
	 * The heartbeat. Parented to the application so it lives as long as
	 * the loop it measures; when the loop is wedged it simply stops
	 * firing, which is exactly the signal.
	 */
	QTimer *beat = new QTimer(QCoreApplication::instance());
	beat->setInterval(BSSH_HANG_BEAT_MS);
	QObject::connect(beat, &QTimer::timeout, [] {
		g_last_beat_ms.store(now_ms());
		g_foreground.store(QGuiApplication::applicationState()
		                    == Qt::ApplicationActive);
	});
	beat->start();

	/*
	 * The watchdog. Detached and never joined: it is meant to outlive
	 * every other thread and stop only when the process does, which is
	 * its termination condition. It takes no lock and touches no Qt
	 * object, so a wedged main thread cannot wedge it too.
	 */
	std::thread([] {
		bool dumped = false;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(BSSH_HANG_POLL_MS));

			const qint64 idle = now_ms() - g_last_beat_ms.load();
			const bool foreground = g_foreground.load();

			if (!bssh_hang_should_dump(foreground, idle, BSSH_HANG_THRESHOLD_MS)) {
				/* Recovered, or never stalled: re-arm for the next time. */
				dumped = false;
				continue;
			}

			if (dumped) continue;
			dumped = true;

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
