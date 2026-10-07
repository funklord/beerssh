#include "sshd/sshd_platform.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QNetworkInterface>

namespace {

const char *const SERVICE_CLASS = "se/vibes/beersshd/SshdService";
const char *const STORAGE_CLASS = "se/vibes/beersshd/SshdStorage";

QJniObject android_context() {
	return QNativeInterface::QAndroidApplication::context();
}

class android_platform;
android_platform *g_platform = nullptr;

/*
 * From SshdService.onTimeout, on Android's main thread. The window lives
 * on Qt's, so the news is queued across rather than acted on here.
 */
void timed_out(JNIEnv *, jclass);

class android_platform : public bssh_sshd_platform {
public:
	android_platform() {
		g_platform = this;

		const JNINativeMethod method = {
			"nativeTimedOut", "()V", reinterpret_cast<void *>(timed_out)
		};
		QJniEnvironment env;
		env.registerNativeMethods(SERVICE_CLASS, &method, 1);
	}

	QString storage_root() const override {
		return QJniObject::callStaticObjectMethod(STORAGE_CLASS, "root", "()Ljava/lang/String;")
		        .toString();
	}

	bool has_storage_access() const override {
		return QJniObject::callStaticMethod<jboolean>(STORAGE_CLASS, "hasAccess", "()Z") == JNI_TRUE;
	}

	void request_storage_access() override {
		const QJniObject context = android_context();
		if (!context.isValid()) return;
		QJniObject::callStaticMethod<void>(STORAGE_CLASS, "requestAccess",
		                                   "(Landroid/content/Context;)V", context.object());
		QJniEnvironment().checkAndClearExceptions();
	}

	/*
	 * The Wi-Fi interface's IPv4 address. Android names it wlan0 on every
	 * phone this was looked at on; Qt's interface type is not used to
	 * find it, since on Android it reports Ethernet for Wi-Fi as often as
	 * not. A hotspot the phone itself provides is a different interface
	 * and is deliberately not matched -- serving to everyone on a tether
	 * is a separate decision.
	 */
	QHostAddress listen_address() const override {
		for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
			if (!interface.name().startsWith(QStringLiteral("wlan"))) continue;
			const auto flags = interface.flags();
			if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) continue;
			for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
				if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) return entry.ip();
			}
		}
		return QHostAddress();
	}

	void request_notifications() override {
		const QJniObject context = android_context();
		if (!context.isValid()) return;
		QJniObject::callStaticMethod<void>(SERVICE_CLASS, "askToNotify",
		                                   "(Landroid/content/Context;)V", context.object());
		QJniEnvironment().checkAndClearExceptions();
	}

	void set_serving(bool serving, const QString &status) override {
		const QJniObject context = android_context();
		if (!context.isValid()) return;

		if (serving == m_serving) {
			if (serving) {
				QJniObject::callStaticMethod<void>(
				        SERVICE_CLASS, "updateStatus", "(Landroid/content/Context;Ljava/lang/String;)V",
				        context.object(), QJniObject::fromString(status).object<jstring>());
			}
		} else if (serving) {
			QJniObject::callStaticMethod<void>(
			        SERVICE_CLASS, "start", "(Landroid/content/Context;Ljava/lang/String;)V",
			        context.object(), QJniObject::fromString(status).object<jstring>());
		} else {
			QJniObject::callStaticMethod<void>(SERVICE_CLASS, "stop", "(Landroid/content/Context;)V",
			                                   context.object());
		}
		m_serving = serving;
		QJniEnvironment().checkAndClearExceptions();
	}

	void report_timeout() {
		/* The service has already stopped itself; this only has to agree. */
		m_serving = false;
		emit time_limit_reached();
	}

private:
	bool m_serving = false;
};

void timed_out(JNIEnv *, jclass) {
	QMetaObject::invokeMethod(qApp, [] {
		if (g_platform) g_platform->report_timeout();
	}, Qt::QueuedConnection);
}

} /* namespace */

bssh_sshd_platform *bssh_sshd_platform::instance() {
	static android_platform platform;
	return &platform;
}
