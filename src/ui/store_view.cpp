#include "ui/store_view.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include "model/profile.h"
#include "model/profile_store.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
#include "ui/main_window.h"

namespace {

/*
 * Made-up hosts. example.com/org and 192.0.2.0/24 are reserved for
 * documentation (RFC 2606, RFC 5737), so none of these can be anybody's.
 */
struct demo_host_t {
	const char *name;
	const char *host;
	const char *user;
};

const demo_host_t DEMO_HOSTS[] = {
	{ "build-server", "build.example.com", "ci" },
	{ "web-01", "web01.example.org", "deploy" },
	{ "raspberry-pi", "192.0.2.10", "pi" },
	{ "backup", "backup.example.net", "admin" },
	{ "router", "192.0.2.1", "root" },
};

bssh_profile_t demo_profile(const demo_host_t &host) {
	bssh_profile_t profile;
	profile.name = QString::fromLatin1(host.name);
	profile.host = QString::fromLatin1(host.host);
	profile.user = QString::fromLatin1(host.user);
	profile.port = 22;
	return profile;
}

/* A plausible stretch of a shell, in colour, as a server would send it. */
QByteArray demo_shell(const char *user, const char *host) {
	const QByteArray prompt = QByteArray("\033[1;32m") + user + '@' + host
	        + "\033[0m:\033[1;34m~\033[0m$ ";
	QByteArray out;
	out += prompt + "uptime\r\n";
	out += " 12:00:01 up 41 days,  3:12,  2 users,  load average: 0.42, 0.37, 0.31\r\n";
	out += prompt + "ls\r\n";
	out += "\033[1;34mprojects\033[0m  \033[1;34mnotes\033[0m  "
	       "\033[1;32mdeploy.sh\033[0m  README.md  \033[1;31mbackup.tar.gz\033[0m\r\n";
	out += prompt + "git -C projects/site log --oneline -4\r\n";
	out += "\033[33m4f2c1a9\033[0m Fix the login redirect\r\n";
	out += "\033[33m9b07e3d\033[0m Cache the image thumbnails\r\n";
	out += "\033[33m1d5a6f0\033[0m Add a dark theme\r\n";
	out += "\033[33m77c2b18\033[0m First version of the site\r\n";
	out += prompt + "df -h /\r\n";
	out += "Filesystem      Size  Used Avail Use% Mounted on\r\n";
	out += "/dev/sda2        62G   21G   38G  36% /\r\n";
	out += prompt;
	return out;
}

/*
 * A session that never connects, its screen filled in directly. Opened
 * through the window like any other, so it is a real pane with a real
 * tab, and shown exactly as one is.
 */
bssh_session *open_demo(bssh_main_window *window, const demo_host_t &host) {
	bssh_session *session = window->registry()->create(demo_profile(host));
	window->adopt_session(session);
	session->emulator()->feed(demo_shell(host.user, host.name));
	return session;
}

/*
 * Logged once the window is on screen and has been painted with the
 * view in it. Asked rather than slept: the target waits for this line,
 * and a fixed delay is too short on a slow device and wasted on a fast
 * one.
 */
void report_when_drawn(bssh_main_window *window, const QString &name) {
	QPointer<bssh_main_window> held = window;
	QTimer *poll = new QTimer(window);
	poll->setInterval(100);
	QObject::connect(poll, &QTimer::timeout, window, [held, name, poll] {
		if (!held || !held->windowHandle() || !held->windowHandle()->isExposed()) return;
		poll->stop();
		poll->deleteLater();
		held->repaint();
		qInfo("store-view-ready %s", qPrintable(name));
	});
	poll->start();
}

} /* namespace */

QStringList bssh_store_view_names() {
	return { QStringLiteral("hosts"), QStringLiteral("terminal"),
	         QStringLiteral("split"), QStringLiteral("settings") };
}

bool bssh_store_view_isolate() {
	const QStandardPaths::StandardLocation kinds[] = {
		QStandardPaths::AppConfigLocation,
		QStandardPaths::AppDataLocation,
		QStandardPaths::CacheLocation,
	};

	QStringList real;
	for (QStandardPaths::StandardLocation kind : kinds)
		real.append(QStandardPaths::writableLocation(kind));

	QStandardPaths::setTestModeEnabled(true);

	/*
	 * Every one has to have MOVED, and into a test-mode directory --
	 * proved here rather than assumed, because whether test mode reaches
	 * a platform's paths is Qt's business and a location that did not
	 * move is the user's own data about to be photographed.
	 */
	QStringList isolated;
	for (QStandardPaths::StandardLocation kind : kinds) {
		const QString path = QStandardPaths::writableLocation(kind);
		if (path.isEmpty() || real.contains(path) || !path.contains(QStringLiteral("qttest"))) {
			qWarning("store view: %s did not move out of the real data (%s); refusing",
			         qPrintable(path), qPrintable(real.join(QLatin1Char(' '))));
			return false;
		}
		isolated.append(path);
	}

	/*
	 * Emptied, so every run starts from the same nothing. Each path was
	 * just checked to be test mode's own rather than the app's.
	 */
	for (const QString &path : isolated) QDir(path).removeRecursively();
	return true;
}

void bssh_store_view_show(bssh_main_window *window, const QString &name) {
	if (!window) return;

	if (name == QStringLiteral("hosts")) {
		for (const demo_host_t &host : DEMO_HOSTS)
			window->profile_store()->save(demo_profile(host));
		window->show_front_door();
	} else if (name == QStringLiteral("terminal")) {
		open_demo(window, DEMO_HOSTS[0]);
	} else if (name == QStringLiteral("split")) {
		/*
		 * Show beside current, as the tab menu offers it: side by side
		 * where the window is wide, stacked where it is a phone held
		 * upright. An edge drop made the second a TAB on a phone, whose
		 * layout keeps new sessions as tabs until asked otherwise.
		 */
		open_demo(window, DEMO_HOSTS[0]);
		window->split_with(open_demo(window, DEMO_HOSTS[2]));
	} else if (name == QStringLiteral("settings")) {
		window->open_settings_section(QStringLiteral("Appearance"));
	} else {
		qWarning("store view: no view named %s", qPrintable(name));
		return;
	}

	report_when_drawn(window, name);
}
