#include <QApplication>
#include <QIcon>
#include <QString>
#include <QTimer>

#include "platform/platform.h"
#include "term/font_catalog.h"
#include "model/app_settings.h"
#include "ui/main_window.h"
#include "ui/theme.h"

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
	/* Set by the build (see the Makefile's VERSION), so the app, the APK
	 * and the Debian package cannot disagree about what this is. */
	QCoreApplication::setApplicationVersion(QStringLiteral(BSSH_VERSION_STRING));
	/* Freedesktop app ID, so the window matches its .desktop entry (sec 0.1). */
	QGuiApplication::setDesktopFileName(QStringLiteral("se.vibes.beerssh"));

	/*
	 * And the icon explicitly. setDesktopFileName is enough on Wayland,
	 * where the compositor looks the entry up, and is NOT enough on X11:
	 * the window manager wants _NET_WM_ICON on the window itself, so
	 * without this the running window shows a generic placeholder while
	 * the launcher that started it has a perfectly good icon. Seen on a
	 * real X11 desktop, running the packaged build.
	 *
	 * From the icon theme rather than a compiled-in resource, because the
	 * packaging installs it into hicolor (sec 12.2) and a second copy
	 * inside the binary is a second thing to keep in step.
	 */
	QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("se.vibes.beerssh")));

	/*
	 * The curated font set (sec 9), before any widget is built: a view
	 * measures its cell the moment it is constructed, and a font that
	 * registers after that measurement is a font the grid was not sized
	 * for. Loading it here is also why nothing further down has to know
	 * where fonts come from.
	 *
	 * Silent when the directory is missing. On a developer's checkout
	 * before tools/fetch-fonts.sh has run, the catalog falls back to
	 * whatever monospace families the system already has (sec 9.4), and a
	 * warning on every start would be noise about a state that renders
	 * perfectly well.
	 */
	bssh_platform *platform = bssh_platform::instance();
	if (platform) {
		const QString font_dir = platform->font_directory();
		const int faces = bssh_font_catalog::instance().load_bundled(font_dir);

		/*
		 * One line, because "the terminal font looks wrong" and "no fonts
		 * were found" are the same bug report, and only this can tell them
		 * apart. It is worth it specifically on Android, where the fonts
		 * live behind the assets: scheme and a packaging mistake produces
		 * an app that renders in the system font and never says why.
		 */
		if (faces > 0) {
			qInfo("beerssh: %d font faces from %s", faces, qPrintable(font_dir));
		} else {
			qInfo("beerssh: no bundled fonts (%s) -- using system families",
			      font_dir.isEmpty() ? "no font directory" : qPrintable(font_dir));
		}
	}

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
	/*
	 * Before the first window, so nothing is painted in one theme and
	 * repainted in the other (sec 6.2). The default is to follow the
	 * desktop, and the detection behind that is bssh_platform's -- Qt's
	 * own hint is not enough on its own.
	 */
	bssh_theme::apply(bssh_theme::from_text(bssh_app_settings().theme()));

	bssh_main_window window(self_test ? QString() : target);
	window.resize(900, 600);
	window.show();

	if (self_test) {
		QTimer::singleShot(0, &app, &QCoreApplication::quit);
	} else {
		/*
		 * After show(), so a confirmation has a window to sit on, and
		 * never under --self-test, which has nobody to dismiss it.
		 */
		window.import_launch_document();

		/*
		 * And what this device opens with: a local shell on desktop
		 * (sec 2). Here rather than in the constructor because it
		 * starts a real process -- under --self-test that forked a
		 * shell and, since a live session asks the platform to stay
		 * resident, the smoke test then never exited.
		 */
		if (target.isEmpty()) window.open_default_session();
	}

	return app.exec();
}
