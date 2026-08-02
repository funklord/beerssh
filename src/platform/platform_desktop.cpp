#include "platform/platform.h"

#include <QApplication>
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
