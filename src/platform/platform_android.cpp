#include "platform/platform.h"
#include "platform/range_source_android.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QScreen>
#include <QPalette>
#include <QStyleHints>

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

/*
 * The request code handed to requestPermissions. Nothing reads the result
 * -- the answer is checked by asking again next time rather than by
 * catching a callback -- but the framework requires one.
 */
const int BSSH_NOTIFICATION_PERMISSION_REQUEST = 1;

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

/*
 * The biometric prompt's answer, coming back from Java (project.md
 * sec 11).
 *
 * This is the first callback INTO native code in this project -- every
 * other JNI call here is one-way -- and the machinery is here rather
 * than shared because one caller does not make a framework.
 *
 * A TOKEN rather than a pointer. The Java side is handed an integer and
 * hands it back; nothing about a C++ object crosses the fence, so a
 * prompt still on screen when the app tears down resolves to a token
 * nobody is waiting for rather than to freed memory. The map is guarded
 * because the callback arrives on Android's UI thread, which is not the
 * Qt thread.
 */
QMutex g_biometric_lock;
QHash<qint64, std::function<void(bool)>> g_biometric_waiting;
qint64 g_biometric_next_token = 1;

void biometric_result(JNIEnv *, jclass, jlong token, jboolean authenticated) {
	std::function<void(bool)> done;

	{
		QMutexLocker held(&g_biometric_lock);
		done = g_biometric_waiting.take(qint64(token));
	}

	if (!done) return;

	/*
	 * Queued onto the Qt thread, because this is Android's UI thread and
	 * the callback ends up touching the credential store and the
	 * session. Calling straight through would be a data race on
	 * everything the continuation reaches -- and the symptom of that is
	 * not a crash here but corruption somewhere else entirely.
	 */
	const bool ok = authenticated == JNI_TRUE;
	QMetaObject::invokeMethod(qApp, [done, ok] { done(ok); }, Qt::QueuedConnection);
}

/*
 * Registered once, lazily, so a build that never asks for a prompt never
 * touches the class -- and so a device whose Java half is missing fails
 * at the capability check rather than at load time.
 */
bool biometric_bridge_ready() {
	static const bool ready = [] {
		const JNINativeMethod method = {
			"nativeResult", "(JZ)V", reinterpret_cast<void *>(biometric_result)
		};

		QJniEnvironment env;
		return env.registerNativeMethods("se/vibes/beerssh/BsshBiometric", &method, 1);
	}();

	return ready;
}

/*
 * The window's lifecycle, coming back from Java for the hang watchdog
 * (project.md sec 5.1). Set once, before the Java side is registered,
 * and called on Android's UI thread -- deliberately NOT queued onto the
 * Qt thread the way the biometric answer is, because the one time this
 * matters is when that thread has stopped. The watchdog's side is two
 * atomics, which is what makes calling straight through safe.
 */
std::function<void(bool)> g_foreground_changed;

void foreground_changed(JNIEnv *, jclass, jboolean foreground) {
	if (g_foreground_changed) g_foreground_changed(foreground == JNI_TRUE);
}

/*
 * Asks for permission to post the notification the foreground service
 * shows (project.md sec 5.1, sec 13).
 *
 * The manifest has declared POST_NOTIFICATIONS since the service was
 * written, and on Android 13 and later a declaration is not a grant: the
 * permission is denied until the user is asked, and nothing here ever
 * asked. The service still runs -- Android does not kill it -- so this
 * failed in the quietest way available: no error, no crash, just an
 * ongoing notification that never appears, for the one thing sec 5.2
 * says the user must be able to see. Play also checks for the flow.
 *
 * Done in C++ rather than by adding Java. sec 5.2 wants exactly one Java
 * file holding "boilerplate plus one notification plus two calls", and
 * a permission request is neither boilerplate nor a notification.
 *
 * Qt has no typed permission for notifications -- QPermission covers
 * camera, microphone, location and the like -- so this goes through the
 * Activity's own methods, which are the ones the platform documents.
 */
void request_notification_permission() {
	/*
	 * Below 33 the permission does not exist. Asking anyway is not an
	 * error, but checkSelfPermission answers "denied" for a name the
	 * system has never heard of, so the request would be made on every
	 * connection forever and never granted.
	 */
	if (QNativeInterface::QAndroidApplication::sdkVersion() < 33) return;

	const QJniObject activity = android_context();
	if (!activity.isValid()) return;

	const QJniObject name =
	        QJniObject::fromString(QStringLiteral("android.permission.POST_NOTIFICATIONS"));

	/* PackageManager.PERMISSION_GRANTED */
	const jint granted = activity.callMethod<jint>(
	        "checkSelfPermission", "(Ljava/lang/String;)I", name.object<jstring>());
	if (granted == 0) return;

	/*
	 * On the Android main thread, because that is where the framework
	 * expects a permission request to originate; from anywhere else the
	 * dialog is the framework's to schedule and the call has been seen to
	 * be dropped.
	 */
	QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
		const QJniObject ui_activity = android_context();
		if (!ui_activity.isValid()) return;

		QJniEnvironment env;
		jclass string_class = env->FindClass("java/lang/String");
		if (!string_class) return;

		const QJniObject wanted =
		        QJniObject::fromString(QStringLiteral("android.permission.POST_NOTIFICATIONS"));
		jobjectArray names = env->NewObjectArray(1, string_class, wanted.object<jstring>());
		if (!names) return;

		ui_activity.callMethod<void>("requestPermissions", "([Ljava/lang/String;I)V",
		                              names, jint(BSSH_NOTIFICATION_PERMISSION_REQUEST));
		env->DeleteLocalRef(names);
	});
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

		/* Qt has no TLS here; see BsshHttp.java. */
		bssh_install_android_range_source();
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
		 * Asked here rather than at startup, and only when a session is
		 * actually going to hold the process open: a permission dialog
		 * on first launch, before the user has done anything, is the
		 * kind users dismiss on reflex. This is the moment the
		 * notification it governs is about to exist.
		 */
		if (keep_alive) request_notification_permission();

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

	bool send_to_background() override {
		/*
		 * Android's own answer to "out of sight, still running": the
		 * task goes behind whatever the user was doing before, exactly
		 * as the Home button does, and comes back from the launcher with
		 * its window intact.
		 *
		 * On the Android main thread, like the permission request above
		 * -- moveTaskToBack is an Activity method and the framework
		 * expects it from the thread that owns the Activity.
		 *
		 * True is returned before the call has run, and that is the safe
		 * way round: the caller's fallback is to hide its window, which
		 * is the very thing that leaves a blank Activity here. If this
		 * somehow fails the app stays on screen, which is wrong and
		 * recoverable rather than wrong and terminal.
		 */
		QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
			const QJniObject activity = android_context();
			if (!activity.isValid()) return;

			activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", jboolean(true));
		});
		return true;
	}

	bool can_hide_from_screenshots() const override {
		return true;
	}

	/*
	 * The status bar hidden and the navigation bar shown, and again
	 * whenever Qt may have undone it.
	 *
	 * Qt has its own idea here: on resume, on a window focus change and on
	 * the system keyboard showing or hiding, its QtWindowInsetsController
	 * treats a hidden status bar as "this window is full screen" and
	 * re-applies full screen -- which hides the navigation bar too. Read out of Qt 6.12's
	 * Qt6Android.jar (restoreFullScreenVisibility, isFullScreen). So this
	 * runs once and then on each of those moments, after Qt's own: the
	 * app turning active again covers the first two, the input method's
	 * visibility the third.
	 */
	void hide_status_bar() override {
		apply_status_bar();
		if (m_status_bar_hooked || !qApp) return;
		m_status_bar_hooked = true;
		connect(this, &bssh_platform::application_active_changed, this, [this] {
			if (is_application_active()) apply_status_bar();
		});
		connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged,
		         this, [this] { apply_status_bar(); });
	}

	void set_hide_from_screenshots(bool hide) override {
		/*
		 * FLAG_SECURE on the Activity's window (sec 11): the system then
		 * leaves this app out of screenshots, screen recordings and the
		 * recents thumbnail. For a terminal that is not a nicety -- the
		 * recents thumbnail is a picture of whatever was last on screen,
		 * and on this app that is somebody's shell.
		 *
		 * 0x2000 is WindowManager.LayoutParams.FLAG_SECURE, written as
		 * its value because reaching the constant through JNI costs a
		 * class lookup and a static field read to learn a number that
		 * has not changed since API 1.
		 *
		 * On the Android main thread, like every other window call here:
		 * the framework expects window flags from the thread that owns
		 * the Activity.
		 */
		QNativeInterface::QAndroidApplication::runOnAndroidMainThread([hide] {
			const QJniObject activity = android_context();
			if (!activity.isValid()) return;

			const QJniObject window =
			        activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
			if (!window.isValid()) return;

			const jint flag_secure = 0x2000;
			if (hide) {
				window.callMethod<void>("addFlags", "(I)V", flag_secure);
			} else {
				window.callMethod<void>("clearFlags", "(I)V", flag_secure);
			}
		});
	}

	/*
	 * No: a phone's screen going dark does not end a session -- the
	 * foreground service keeps the process (sec 5.1) -- and keeping the
	 * screen on is a different request from keeping a connection up.
	 */
	bool can_inhibit_sleep() const override { return false; }
	void platform_set_sleep_inhibited(bool) override {}

	QSize platform_screen_size() const override {
		/*
		 * Qt's logical pixels are dp on Android (sec 8.1), and the
		 * primary screen is the one the app is on: a folding phone
		 * reports its cover or its inner display as it is folded.
		 */
		const QScreen *screen = QGuiApplication::primaryScreen();
		return screen ? screen->size() : QSize();
	}

	bool watch_foreground(std::function<void(bool)> changed) override {
		const QJniObject context = android_context();
		if (!context.isValid()) return false;

		g_foreground_changed = std::move(changed);

		const JNINativeMethod method = {
			"nativeForeground", "(Z)V", reinterpret_cast<void *>(foreground_changed)
		};
		QJniEnvironment env;
		if (!env.registerNativeMethods("se/vibes/beerssh/BsshLifecycle", &method, 1)) {
			g_foreground_changed = nullptr;
			return false;
		}

		QJniObject::callStaticMethod<void>(
		        "se/vibes/beerssh/BsshLifecycle", "watch",
		        "(Landroid/content/Context;)V", context.object());
		return !env.checkAndClearExceptions();
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

	void platform_notify(const QString &title, const QString &body) override {
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

	QString platform_terminfo_directory() const override {
		/*
		 * Unpacked out of the package, because this path is read by
		 * open(2) rather than by Qt: an asset is a compressed member of
		 * the APK and has no filesystem path at all, so "assets:/..."
		 * is a name only Qt's file engine understands. ncurses, and
		 * anything the user runs, would find nothing there.
		 *
		 * Once, and then never again. The copy is skipped when the
		 * unpacked file is already there at the same size, so this
		 * costs a stat per entry after the first run -- and it still
		 * refreshes when an app update ships a different database,
		 * which a bare existence check would not.
		 *
		 * Nothing bundled is not an error: the build skips the
		 * terminfo target when ncurses is absent, exactly as it skips
		 * fonts, and the app then behaves as it did before this
		 * existed.
		 */
		static QString unpacked;
		static bool tried = false;
		static QMutex guard;
		QMutexLocker locked(&guard);
		if (tried) return unpacked;
		tried = true;

		const QString target = QStandardPaths::writableLocation(
		                               QStandardPaths::AppDataLocation)
		                        + QStringLiteral("/terminfo");

		QDirIterator entries(QStringLiteral("assets:/terminfo"),
		                      QDir::Files, QDirIterator::Subdirectories);
		int copied = 0, present = 0;
		while (entries.hasNext()) {
			const QString source = entries.next();
			const QString relative =
			        source.mid(QStringLiteral("assets:/terminfo/").size());
			const QString destination = target + QStringLiteral("/") + relative;

			QDir().mkpath(QFileInfo(destination).absolutePath());

			QFile packaged(source);
			if (!packaged.open(QIODevice::ReadOnly)) continue;
			const QByteArray bytes = packaged.readAll();

			QFile existing(destination);
			if (existing.exists() && existing.size() == bytes.size()) {
				++present;
				continue;
			}
			if (!existing.open(QIODevice::WriteOnly)) continue;
			existing.write(bytes);
			existing.close();
			++copied;
		}

		if (copied == 0 && present == 0) return unpacked; /* empty */
		unpacked = target;
		return unpacked;
	}

	QString font_directory() const override {
		/*
		 * The APK's own assets, reached through Qt's "assets:" scheme --
		 * an Android asset is not a file on disk and has no path a
		 * QFileInfo would recognise without it.
		 *
		 * Only the base set is here. sec 9.4 delivers the rest as a
		 * download into the app's data directory, which main.cpp loads
		 * after this one (bssh_downloaded_font_directory). It was one
		 * directory before that landed because shipping a picker
		 * that offers downloads before the download exists is worse than
		 * shipping three fonts that work.
		 */
		return QStringLiteral("assets:/fonts");
	}

	QString launch_extra(const QString &key) const override {
		QJniObject activity = QNativeInterface::QAndroidApplication::context();
		if (!activity.isValid()) return QString();

		QJniObject intent = activity.callObjectMethod(
		        "getIntent", "()Landroid/content/Intent;");
		if (!intent.isValid()) return QString();

		QJniObject name = QJniObject::fromString(key);
		return intent.callObjectMethod("getStringExtra",
		                                "(Ljava/lang/String;)Ljava/lang/String;",
		                                name.object<jstring>()).toString();
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

	double platform_system_font_scale() const override {
		/*
		 * Configuration.fontScale, the accessibility "Font size" the
		 * user set for the whole device. Qt ignores it (measured on a
		 * Note 9: chrome and grid both stay put across a change), so it
		 * is read here and applied to the interface font by the caller.
		 * A public float field on Configuration, read exactly as
		 * android_night_mode reads uiMode -- no Java added (sec 17).
		 */
		const QJniObject context = android_context();
		if (!context.isValid()) return 1.0;

		const QJniObject resources =
		        context.callObjectMethod("getResources",
		                                  "()Landroid/content/res/Resources;");
		if (!resources.isValid()) return 1.0;

		const QJniObject config =
		        resources.callObjectMethod("getConfiguration",
		                                    "()Landroid/content/res/Configuration;");
		if (!config.isValid()) return 1.0;

		const float scale = config.getField<jfloat>("fontScale");

		/*
		 * Clamped rather than trusted: a broken launcher or a future
		 * Android could report a scale that would make the interface
		 * unusable, and 0 would collapse it. The band is generous enough
		 * to cover Android's own largest accessibility step.
		 */
		if (scale < 0.5f || scale > 3.0f) return 1.0;
		return double(scale);
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

	/*
	 * Hardware present AND something enrolled, which Java asks in one
	 * call. Both halves matter: a reader with no finger registered
	 * raises a prompt that can only be cancelled, so offering the
	 * setting there would let a user make their own password
	 * unreachable. sec 11's rule is absent rather than faked.
	 *
	 * The bridge is required as well -- a build whose Java half did not
	 * ship has no prompt, whatever the hardware says.
	 */
	bool platform_has_biometric() const override {
		if (!biometric_bridge_ready()) return false;

		const QJniObject context = android_context();
		if (!context.isValid()) return false;

		return QJniObject::callStaticMethod<jboolean>(
		               "se/vibes/beerssh/BsshBiometric", "available",
		               "(Landroid/content/Context;)Z", context.object())
		       == JNI_TRUE;
	}

	void authenticate(const QString &reason, std::function<void(bool)> done) override {
		if (!done) return;

		/*
		 * False, so a caller that forgets the capability check fails
		 * CLOSED: it withholds a password rather than releasing one.
		 * That is the direction every failure here takes, including the
		 * ones below that look like plumbing.
		 */
		if (!biometric_bridge_ready()) {
			done(false);
			return;
		}

		const QJniObject activity = android_context();
		if (!activity.isValid()) {
			done(false);
			return;
		}

		qint64 token = 0;
		{
			QMutexLocker held(&g_biometric_lock);
			token = g_biometric_next_token++;
			g_biometric_waiting.insert(token, done);
		}

		const QJniObject title = QJniObject::fromString(QStringLiteral("BeerSSH"));
		const QJniObject description = QJniObject::fromString(reason);

		/*
		 * On the ANDROID UI thread, which is not this one.
		 * BiometricPrompt needs a Looper, and the Qt thread has none --
		 * called from here it throws before anything is drawn, and the
		 * caller waits for a callback that will never arrive.
		 */
		QNativeInterface::QAndroidApplication::runOnAndroidMainThread(
		        [activity, title, description, token] {
			QJniObject::callStaticMethod<void>(
			        "se/vibes/beerssh/BsshBiometric", "authenticate",
			        "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;J)V",
			        activity.object(), title.object(), description.object(),
			        jlong(token));
		});
	}

	bool platform_is_touch_primary() const override {
		/*
		 * True as the M0 default, but this is not a constant either: an
		 * Android device with a Bluetooth keyboard docked is the case
		 * that makes a hardcoded "Android means touch" wrong. Same M3
		 * follow-up as the desktop implementation.
		 */
		return true;
	}

private:
	static void apply_status_bar() {
		QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
			const QJniObject activity = android_context();
			if (!activity.isValid()) return;
			const QJniObject window =
			        activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
			if (!window.isValid()) return;

			const jint sdk = QJniObject::getStaticField<jint>("android/os/Build$VERSION",
			                                                   "SDK_INT");
			if (sdk >= 30) {
				/*
				 * WindowInsetsController, API 30. Behavior 2 is
				 * BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE: a swipe shows
				 * the bar over the window for a moment, and it hides
				 * again without the window being resized.
				 */
				const QJniObject controller = window.callObjectMethod(
				        "getInsetsController", "()Landroid/view/WindowInsetsController;");
				if (!controller.isValid()) return;
				const jint status = QJniObject::callStaticMethod<jint>(
				        "android/view/WindowInsets$Type", "statusBars", "()I");
				const jint navigation = QJniObject::callStaticMethod<jint>(
				        "android/view/WindowInsets$Type", "navigationBars", "()I");
				controller.callMethod<void>("setSystemBarsBehavior", "(I)V", jint(2));
				controller.callMethod<void>("show", "(I)V", navigation);
				controller.callMethod<void>("hide", "(I)V", status);
			} else {
				/*
				 * Before API 30 (minSdk is 26): SYSTEM_UI_FLAG_FULLSCREEN
				 * 0x4, with IMMERSIVE_STICKY 0x1000 for the same
				 * swipe-to-peek, as values for the reason FLAG_SECURE
				 * above is one.
				 */
				const QJniObject decor =
				        window.callObjectMethod("getDecorView", "()Landroid/view/View;");
				if (decor.isValid()) {
					decor.callMethod<void>("setSystemUiVisibility", "(I)V", jint(0x4 | 0x1000));
				}
			}
		});
	}

	bool m_status_bar_hooked = false;
	mutable bssh_appearance_t m_last_scheme = BSSH_APPEARANCE_UNKNOWN;
	bool m_keep_alive;
	QString m_status;
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_android platform;
	return &platform;
}
