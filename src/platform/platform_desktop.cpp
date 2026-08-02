#include "platform/platform.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QNetworkInformation>
#include <QSystemTrayIcon>

/*
 * Desktop implementation of bssh_platform (Linux primary; macOS and
 * Windows best-effort, per project.md sec 1).
 *
 * M0 is intentionally thin. What lands here later is the desktop half of
 * project.md sec 5.5: staying resident with no window open, the tray icon,
 * the prompt before backgrounding with live sessions, desktop
 * notifications, and treating suspend/resume as a network-change event.
 */
class bssh_platform_desktop : public bssh_platform {
public:
	bssh_platform_desktop() : m_tray(nullptr) {
		/*
		 * Reachability changes are the desktop stand-in for Android's
		 * connectivity callbacks. The case that actually matters is a
		 * laptop waking from suspend: every TCP connection is already
		 * dead and looks precisely like a network flap (sec 5.5).
		 *
		 * Loaded best-effort -- a machine with no backend for this is a
		 * machine that simply reconnects on keepalive timeout instead,
		 * which is slower but not broken.
		 */
		if (QNetworkInformation::loadDefaultBackend()) {
			QNetworkInformation *info = QNetworkInformation::instance();
			if (info) {
				connect(info, &QNetworkInformation::reachabilityChanged,
				         this, [this](QNetworkInformation::Reachability) {
					emit network_changed();
				});
			}
		}
	}

	QString name() const override {
		/*
		 * The QPA plugin name ("wayland", "xcb", "offscreen", ...) is
		 * worth surfacing rather than a flat "desktop": the two Linux
		 * session types differ on which key chords ever reach the app
		 * (project.md sec 6.1), so a bug report wants to say which one
		 * was in use.
		 */
		return QStringLiteral("desktop/") + QGuiApplication::platformName();
	}

	void set_keep_alive(bool keep_alive) override {
		/*
		 * The whole desktop mechanism: stop Qt quitting when the last
		 * window closes, and give the user a way back. Sessions belong
		 * to the registry, not to a window (sec 3), so closing the
		 * window must not disturb them.
		 */
		QApplication::setQuitOnLastWindowClosed(!keep_alive);

		if (keep_alive) {
			ensure_tray();
		} else if (m_tray) {
			m_tray->hide();
		}
	}

	void set_session_count(int count) override {
		if (!m_tray) return;

		m_tray->setToolTip(count == 1 ? QStringLiteral("BeerSSH -- 1 session connected")
		                              : QStringLiteral("BeerSSH -- %1 sessions connected").arg(count));
	}

	void notify(const QString &title, const QString &body) override {
		/*
		 * Qt routes this through the platform's own notification service
		 * where one exists, and falls back to a balloon otherwise. A
		 * tray icon is required for it either way, which is why this
		 * creates one rather than silently doing nothing.
		 */
		ensure_tray();
		if (m_tray) m_tray->showMessage(title, body);
	}

	QString font_directory() const override {
		/*
		 * Three places, in the order that makes a developer's checkout
		 * behave like an installation rather than a special case:
		 *
		 *   next to the binary   -- a build tree, and any self-contained
		 *                           layout that ships fonts beside the app
		 *   ../share/beerssh     -- an installed prefix
		 *   BSSH_FONT_DIR        -- an override, for running against a
		 *                           font set somewhere else entirely
		 *
		 * The override goes first because that is what an override means.
		 * Nothing here is fatal if missing: sec 9.4 lets the catalog fall
		 * back to families the distribution already installed, which on a
		 * Linux desktop is usually most of the set.
		 */
		const QByteArray override_dir = qgetenv("BSSH_FONT_DIR");
		if (!override_dir.isEmpty()) return QString::fromLocal8Bit(override_dir);

		const QString app_dir = QCoreApplication::applicationDirPath();
		const QStringList candidates = {
			app_dir + QStringLiteral("/fonts"),
			app_dir + QStringLiteral("/../share/beerssh/fonts"),
			app_dir + QStringLiteral("/../../fonts")
		};

		for (const QString &candidate : candidates) {
			if (QDir(candidate).exists()) return QDir(candidate).canonicalPath();
		}

		return QString();
	}

	QString launch_document() const override {
		/*
		 * A file named on the command line, which is what a desktop file
		 * manager passes when BeerSSH is the handler for a .ini, and
		 * what a shell passes when someone types it. Only .ini is
		 * accepted: the first non-option argument is otherwise a
		 * [user@]host[:port] target (sec 14, M1), and mistaking a
		 * hostname for a file to import would be a strange way to fail.
		 */
		for (const QString &argument : QCoreApplication::arguments().mid(1)) {
			if (argument.startsWith(QLatin1Char('-'))) continue;
			if (!argument.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)) continue;
			if (QFileInfo::exists(argument)) return argument;
		}

		return QString();
	}

	bool is_touch_primary() const override {
		/*
		 * False for now, not forever. A convertible in tablet mode is a
		 * real desktop case for the on-screen key row, so this becomes a
		 * query of the available input devices (and of the user's
		 * three-state auto/always/never setting) at M3, when the key row
		 * itself exists.
		 */
		return false;
	}

private:
	void ensure_tray() {
		if (m_tray || !QSystemTrayIcon::isSystemTrayAvailable()) return;

		m_tray = new QSystemTrayIcon(this);
		m_tray->setToolTip(QStringLiteral("BeerSSH"));

		QMenu *menu = new QMenu();
		menu->addAction(QStringLiteral("Quit BeerSSH"), qApp, &QCoreApplication::quit);
		m_tray->setContextMenu(menu);

		m_tray->show();
	}

	QSystemTrayIcon *m_tray;
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_desktop platform;
	return &platform;
}
