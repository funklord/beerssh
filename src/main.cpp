#include <QApplication>
#include <QTimer>

#include "ui/main_window.h"

/*
 * BeerSSH -- SSH terminal for Android and desktop (project.md sec 0.1).
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
	for (int i = 1; i < argc; ++i) {
		if (qstrcmp(argv[i], "--self-test") == 0) self_test = true;
	}

	bssh_main_window window;
	window.resize(900, 600);
	window.show();

	if (self_test) {
		QTimer::singleShot(0, &app, &QCoreApplication::quit);
	}

	return app.exec();
}
