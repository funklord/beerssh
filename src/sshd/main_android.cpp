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
#include <QStringList>
#include <QtCore/private/qandroidextras_p.h>

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

	bssh_sshd_platform *platform = bssh_sshd_platform::instance();
	bssh_sshd_window window(platform, new bssh_sshd_remote(platform));
	window.show();
	return app.exec();
}
