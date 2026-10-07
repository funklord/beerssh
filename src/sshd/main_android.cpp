/*
 * beersshd on Android: the window, and nothing else (project.md sec 19).
 * The desktop's main.cpp is a command line; this is what the launcher
 * opens.
 */

#include <QApplication>

#include "sshd/sshd_platform.h"
#include "sshd/sshd_window.h"

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("vibes.se"));
	QCoreApplication::setApplicationName(QStringLiteral("beersshd"));

	bssh_sshd_window window(bssh_sshd_platform::instance());
	window.show();
	return app.exec();
}
