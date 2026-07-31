#include <QApplication>
#include <QString>
#include <QTimer>

#include "ui/main_window.h"

/*
 * BeerSSH -- SSH terminal for Android and desktop (project.md sec 0.1).
 *
 * Usage: beerssh [user@host[:port]]
 *
 * --self-test constructs the window, pumps the event loop once and exits
 * 0. It is what CI runs under the offscreen QPA plugin (sec 12.4): a GUI
 * that cannot be launched headlessly is a GUI CI cannot check at all, and
 * this is the cheapest possible smoke test that the widget tree, the
 * platform implementation and the font metrics all come up.
 */
int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	QCoreApplication::setOrganizationDomain(QStringLiteral("vibes.se"));
	QCoreApplication::setApplicationName(QStringLiteral("beerssh"));
	QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
	/* Freedesktop app ID, so the window matches its .desktop entry (sec 0.1). */
	QGuiApplication::setDesktopFileName(QStringLiteral("se.vibes.beerssh"));

	bool self_test = false;
	QString target;
	for (int i = 1; i < argc; ++i) {
		const QString arg = QString::fromLocal8Bit(argv[i]);
		if (arg == QLatin1String("--self-test")) {
			self_test = true;
		} else if (!arg.startsWith(QLatin1Char('-'))) {
			target = arg;
		}
	}

	/*
	 * A [user@]host[:port] on the command line is M1's way in, and the
	 * only one: host profiles (sec 10) arrive at M6. --self-test never
	 * takes a target, since CI has no server to reach.
	 */
	bssh_main_window window(self_test ? QString() : target);
	window.resize(900, 600);
	window.show();

	if (self_test) {
		QTimer::singleShot(0, &app, &QCoreApplication::quit);
	}

	return app.exec();
}
