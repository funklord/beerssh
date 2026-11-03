/*
 * beersshd on Android (project.md sec 19): two processes from one library.
 *
 * The launcher opens the window, in the app's process. The server runs in
 * the :server process, where SshdService (a QtService) runs this same
 * main with --serve: a keeper and no window, reporting through two files
 * the window reads (bssh_sshd_remote). The desktop's main.cpp is a
 * command line instead.
 */

#include <QApplication>
#include <QCoreApplication>
#include <QJniObject>
#include <QStringList>
#include <QTimer>
#include <QWindow>
#include <QtCore/private/qandroidextras_p.h>

#include "sshd/sshd_demo.h"
#include "sshd/sshd_keeper.h"
#include "sshd/sshd_platform.h"
#include "sshd/sshd_remote.h"
#include "sshd/sshd_window.h"

namespace {

void name_the_application() {
	QCoreApplication::setOrganizationName(QStringLiteral("vibes.se"));
	QCoreApplication::setApplicationName(QStringLiteral("beersshd"));
}

bool serve_mode(int argc, char *argv[]) {
	for (int i = 1; i < argc; ++i) {
		if (qstrcmp(argv[i], "--serve") == 0) return true;
	}
	return false;
}

/* The store_view extra of the launching intent, or empty (the play-store skill). */
QString store_view() {
	const QJniObject activity = QNativeInterface::QAndroidApplication::context();
	if (!activity.isValid()) return QString();
	const QJniObject intent = activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
	if (!intent.isValid()) return QString();
	return intent.callObjectMethod("getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;",
	                               QJniObject::fromString(QStringLiteral("store_view")).object<jstring>())
	        .toString();
}

/*
 * The device's status bar out of the listing: demo mode cannot be relied
 * on (Samsung ignores it), so the app hides it, and again whenever focus
 * moves, since Qt puts the system bars back as it shows a window.
 */
void hide_status_bar() {
	QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
		const QJniObject activity = QNativeInterface::QAndroidApplication::context();
		const QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
		const QJniObject controller =
		        window.callObjectMethod("getInsetsController", "()Landroid/view/WindowInsetsController;");
		const jint status_bars = QJniObject::callStaticMethod<jint>("android/view/WindowInsets$Type",
		                                                             "statusBars", "()I");
		if (controller.isValid()) controller.callMethod<void>("hide", "(I)V", status_bars);
	});
}

/* A demo window for one store view, and its ready line once it has drawn. */
int run_store_view(QApplication &app, const QString &view) {
	/*
	 * Two views: "server", serving with keys and a log, and "setup", the
	 * first screen a new user sees, asking for All files access.
	 */
	auto *platform = new bssh_sshd_demo_platform;
	auto *control = new bssh_sshd_demo_control;
	const bool setup = view == QStringLiteral("setup");
	platform->access = !setup;
	control->serving = !setup;
	bssh_sshd_window window(platform, control);
	window.show();
	if (!setup) control->tell();
	hide_status_bar();
	QObject::connect(&app, &QGuiApplication::focusWindowChanged, [] { hide_status_bar(); });

	QTimer::singleShot(1500, &window, [view] { qInfo("store-view-ready %s", qPrintable(view)); });
	return app.exec();
}

} /* namespace */

int main(int argc, char *argv[]) {
	if (serve_mode(argc, argv)) {
		/*
		 * QAndroidService, not QCoreApplication: QtService's onCreate waits,
		 * on Android's main thread, for a semaphore only QAndroidService
		 * releases. With QCoreApplication the server ran and that thread
		 * never came back -- so onStartCommand, and with it Stop, was
		 * never delivered (seen on the API 36 emulator: Stop pressed, the
		 * process listening on). No window either way.
		 */
		QAndroidService app(argc, argv);
		name_the_application();

		bssh_sshd_platform *platform = bssh_sshd_platform::instance();
		bssh_sshd_keeper keeper(platform);
		bssh_sshd_remote::report_from(&keeper, platform);

		/*
		 * Started while switched off -- a Stop that crossed a Start -- has
		 * nothing to serve, and the service is told to go.
		 */
		if (!platform->switched_on()) {
			platform->stop_server_process();
			return 0;
		}
		keeper.start_if_switched_on();
		return app.exec();
	}

	QApplication app(argc, argv);
	name_the_application();

	const QString view = store_view();
	if (!view.isEmpty()) return run_store_view(app, view);

	bssh_sshd_platform *platform = bssh_sshd_platform::instance();
	bssh_sshd_window window(platform, new bssh_sshd_remote(platform));
	window.show();
	return app.exec();
}
