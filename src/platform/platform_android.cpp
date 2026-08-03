#include "platform/platform.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStyleHints>

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

/*
 * Android's own answer to "is the system in night mode", read from the
 * activity's current Configuration.
 *
 * Qt is asked first everywhere else, and on Android it cannot be trusted
 * to stay current: measured on an emulator running Qt 6.11.1, switching
 * the system to dark delivers a ThemeChange to the application while
 * QStyleHints::colorScheme() goes on reporting Light. It is right at
 * startup and stale from then on, which is the worst of both -- the app
 * looks correct until the moment the user changes their mind.
 *
 * uiMode & UI_MODE_NIGHT_MASK is what the platform itself consults, so it
 * is right whenever it is asked. No Java is added for it (sec 17): this
 * is a field read through JNI, not glue that would be shorter in Kotlin.
 */
bssh_appearance_t android_night_mode() {
	const QJniObject context = QNativeInterface::QAndroidApplication::context();
	if (!context.isValid()) return BSSH_APPEARANCE_UNKNOWN;

	const QJniObject resources =
		      context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
	if (!resources.isValid()) return BSSH_APPEARANCE_UNKNOWN;

	const QJniObject config =
		      resources.callObjectMethod("getConfiguration", "()Landroid/content/res/Configuration;");
	if (!config.isValid()) return BSSH_APPEARANCE_UNKNOWN;

	const int ui_mode = config.getField<jint>("uiMode");

	/* Configuration.UI_MODE_NIGHT_MASK / _YES / _NO. */
	switch (ui_mode & 0x30) {
	case 0x20: return BSSH_APPEARANCE_DARK;
	case 0x10: return BSSH_APPEARANCE_LIGHT;
	default:   return BSSH_APPEARANCE_UNKNOWN;
	}
}

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
		m_last_scheme = system_appearance();
		if (qApp) qApp->installEventFilter(this);
	}

	QString name() const override {
		return QStringLiteral("android");
	}

	void set_keep_alive(bool keep_alive) override {
		/*
		 * Only on a real change. The window recomputes this on every
		 * session event, so without it a stop is sent for a service that
		 * was never started -- and since a stop is delivered as a start
		 * (see BsshService), that would flash a notification for a
		 * service nobody asked for.
		 */
		if (keep_alive == m_keep_alive) return;

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

	QString font_directory() const override {
		/*
		 * The APK's own assets, reached through Qt's "assets:" scheme --
		 * an Android asset is not a file on disk and has no path a
		 * QFileInfo would recognise without it.
		 *
		 * Only the base set is here. sec 9.4 delivers the rest through
		 * Play asset packs, which unpack into the app's data directory
		 * rather than into assets, so this becomes two directories the
		 * day that lands. It is one for now because shipping a picker
		 * that offers downloads before the download exists is worse than
		 * shipping three fonts that work.
		 */
		return QStringLiteral("assets:/fonts");
	}

	QString launch_document() const override {
		/*
		 * The URI the activity was started with, straight out of the
		 * launch intent. Qt's file engine opens a content:// URI as
		 * happily as a path, so what comes back here can be handed to
		 * QFile unchanged and never has to be turned into a filename --
		 * which is fortunate, because a content URI does not have one.
		 */
		QJniObject activity = QNativeInterface::QAndroidApplication::context();
		if (!activity.isValid()) return QString();

		QJniObject intent = activity.callObjectMethod(
		        "getIntent", "()Landroid/content/Intent;");
		if (!intent.isValid()) return QString();

		QJniObject data = intent.callObjectMethod("getData", "()Landroid/net/Uri;");

		/*
		 * ACTION_SEND puts the URI in an extra instead of in the data,
		 * and sharing a file to an app is the gesture most Android file
		 * managers offer, so both are read.
		 */
		if (!data.isValid()) {
			QJniObject key = QJniObject::fromString(QStringLiteral("android.intent.extra.STREAM"));
			data = intent.callObjectMethod(
			        "getParcelableExtra",
			        "(Ljava/lang/String;)Landroid/os/Parcelable;", key.object<jstring>());
		}

		if (!data.isValid()) return QString();

		return data.callObjectMethod("toString", "()Ljava/lang/String;").toString();
	}

	/*
	 * Qt maps Android's UI_MODE_NIGHT onto colorScheme, so the hint is
	 * the answer here and there is no portal to fall back to. The palette
	 * check remains as a floor for the case where the style has themed
	 * the app but the hint has not arrived yet.
	 */
	/*
	 * Android delivers a uiMode change as a configuration change rather
	 * than restarting us -- the manifest lists uiMode in configChanges,
	 * deliberately -- and Qt does not turn that into a colorSchemeChanged
	 * we ever saw. Measured on an emulator: switching the system to dark
	 * left the app light until it was restarted, which is exactly the
	 * "only right at startup" failure this signal exists to prevent.
	 *
	 * So the application's own theme/palette events are watched instead,
	 * and the answer is re-read when one arrives. Only a real change is
	 * reported: bssh_theme applies a palette of its own, which raises the
	 * same events, and re-emitting on those would be a loop.
	 */
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event && (event->type() == QEvent::ThemeChange ||
		               event->type() == QEvent::ApplicationPaletteChange)) {
			const bssh_appearance_t now = system_appearance();
			if (now != m_last_scheme) {
				m_last_scheme = now;
				emit appearance_changed();
			}
		}
		return QObject::eventFilter(watched, event);
	}

	/*
	 * Android has no OpenSSH directory. It does have a writable home --
	 * the app's own sandbox -- so anything that probes for ~/.ssh finds
	 * it creatable and puts files where no ssh client will look.
	 */
	QString openssh_directory() const override {
		return QString();
	}

	bssh_appearance_t system_appearance() const override {
		/*
		 * Android's Configuration first, because it is the only source
		 * here that is still correct after the user changes the setting
		 * -- see android_night_mode().
		 */
		const bssh_appearance_t night = android_night_mode();
		if (night != BSSH_APPEARANCE_UNKNOWN) return night;

		/*
		 * Qt's hint, where the Qt being built against has one:
		 * QStyleHints::colorScheme() arrived in 6.5 and the CI runner
		 * ships 6.4. Everything below still answers without it, which is
		 * why the chain has three sources rather than one.
		 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		const Qt::ColorScheme hinted = QGuiApplication::styleHints()->colorScheme();
		if (hinted == Qt::ColorScheme::Dark) return BSSH_APPEARANCE_DARK;
		if (hinted == Qt::ColorScheme::Light) return BSSH_APPEARANCE_LIGHT;
#endif

		const QColor window = startup_palette().color(QPalette::Window);
		return window.lightness() < 128 ? BSSH_APPEARANCE_DARK : BSSH_APPEARANCE_LIGHT;
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
	mutable bssh_appearance_t m_last_scheme = BSSH_APPEARANCE_UNKNOWN;
	bool m_keep_alive;
	QString m_status;
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_android platform;
	return &platform;
}
