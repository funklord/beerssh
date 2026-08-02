#include "platform/platform.h"

/*
 * Android implementation of bssh_platform.
 *
 * M0 is intentionally thin, and this file is where the Android-specific
 * machinery is allowed to accumulate: the JNI bridge to the foreground
 * service (project.md sec 5.1/5.2), the notification text, the
 * ConnectivityManager callbacks, and Keystore-wrapped secrets.
 *
 * The JVM side stays as small as the platform forces -- a manifest-
 * registered Service, its mandatory notification, and two JNI calls. No
 * networking, protocol, session or UI logic in Java/Kotlin, per
 * project.md sec 5.2.
 */
class bssh_platform_android : public bssh_platform {
public:
	bssh_platform_android() {
	}

	QString name() const override {
		return QStringLiteral("android");
	}

	void set_keep_alive(bool keep_alive) override {
		Q_UNUSED(keep_alive);
		/*
		 * NOT IMPLEMENTED, and deliberately not stubbed to look done.
		 *
		 * This is where the foreground service starts and stops: a JNI
		 * call into the manifest-registered Service, which builds its
		 * mandatory notification and keeps the process from being killed
		 * (sec 5.1/5.2). Until it exists, an Android build backgrounds
		 * like any other app -- which is to say its sessions die.
		 *
		 * It cannot be written blind: the Play foreground-service type
		 * is still [OPEN] in sec 5.1, and the whole area has to be
		 * tested on a device that this was developed without.
		 */
	}

	void set_session_count(int count) override {
		Q_UNUSED(count);
		/* Notification text, via the same JNI bridge. See set_keep_alive. */
	}

	void notify(const QString &title, const QString &body) override {
		Q_UNUSED(title);
		Q_UNUSED(body);
		/* NotificationCompat through the service. See set_keep_alive. */
	}

	bool is_touch_primary() const override {
		/*
		 * True as the M0 default, but this is not a constant either: an
		 * Android device with a Bluetooth keyboard docked is the case
		 * that makes a hardcoded "Android means touch" wrong. Same M3
		 * follow-up as the desktop implementation.
		 */
		return true;
	}
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_android platform;
	return &platform;
}
