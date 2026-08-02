#include "platform/platform.h"

#include <QCoreApplication>
#include <QJniObject>

namespace {

/*
 * The one JNI bridge in the project (project.md sec 5.2). Three calls
 * into one Java class, and nothing else crosses: no networking, no
 * session state, no protocol. If this list ever grows, the thing being
 * added almost certainly belongs in C++.
 */
const char *const BSSH_SERVICE_CLASS = "se/vibes/beerssh/BsshService";

QJniObject android_context() {
	return QNativeInterface::QAndroidApplication::context();
}

} /* namespace */

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
	bssh_platform_android() : m_keep_alive(false), m_status(QStringLiteral("Connected")) {
	}

	QString name() const override {
		return QStringLiteral("android");
	}

	void set_keep_alive(bool keep_alive) override {
		/*
		 * Starts and stops the foreground service (sec 5.1). A
		 * backgrounded Android app cannot hold a socket open; this is
		 * what buys the process the right to keep running, and Android
		 * will not grant it without the notification the service shows.
		 */
		const QJniObject context = android_context();
		if (!context.isValid()) return;

		if (keep_alive) {
			const QJniObject status = QJniObject::fromString(m_status);
			QJniObject::callStaticMethod<void>(
			        BSSH_SERVICE_CLASS, "start",
			        "(Landroid/content/Context;Ljava/lang/String;)V",
			        context.object(), status.object<jstring>());
		} else {
			QJniObject::callStaticMethod<void>(
			        BSSH_SERVICE_CLASS, "stop", "(Landroid/content/Context;)V",
			        context.object());
		}

		m_keep_alive = keep_alive;
	}

	void set_session_count(int count) override {
		m_status = count == 1
		                   ? QStringLiteral("1 session connected")
		                   : QStringLiteral("%1 sessions connected").arg(count);

		/* Nothing to update while no service is running. */
		if (!m_keep_alive) return;

		const QJniObject context = android_context();
		if (!context.isValid()) return;

		const QJniObject status = QJniObject::fromString(m_status);
		QJniObject::callStaticMethod<void>(
		        BSSH_SERVICE_CLASS, "updateStatus",
		        "(Landroid/content/Context;Ljava/lang/String;)V",
		        context.object(), status.object<jstring>());
	}

	void notify(const QString &title, const QString &body) override {
		/*
		 * Folded into the service's own notification rather than posting
		 * a second one: two notifications from a terminal that is doing
		 * one thing is one too many, and the ongoing one is already on
		 * screen. A separate alert for a bell or a failed reconnect is
		 * worth revisiting once there is a device to judge it on.
		 */
		Q_UNUSED(title);
		set_status_text(body);
	}

	void set_status_text(const QString &text) {
		m_status = text;
		if (!m_keep_alive) return;

		const QJniObject context = android_context();
		if (!context.isValid()) return;

		const QJniObject status = QJniObject::fromString(m_status);
		QJniObject::callStaticMethod<void>(
		        BSSH_SERVICE_CLASS, "updateStatus",
		        "(Landroid/content/Context;Ljava/lang/String;)V",
		        context.object(), status.object<jstring>());
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

private:
	bool m_keep_alive;
	QString m_status;
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_android platform;
	return &platform;
}
