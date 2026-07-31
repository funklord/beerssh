#include "platform/platform.h"

#include <QGuiApplication>

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
	bssh_platform_desktop() {
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
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_desktop platform;
	return &platform;
}
