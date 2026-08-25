#include <QApplication>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include "model/app_settings.h"
#include "model/command_line.h"
#include "model/profile.h"
#include "model/profile_store.h"
#include "platform/platform.h"
#include "term/font_catalog.h"
#include "ui/main_window.h"
#include "ui/theme.h"

/*
 * BeerSSH -- SSH terminal for Android and desktop (project.md sec 0.1).
 *
 * The arguments are xterm's and konsole's, because opening a local shell
 * on desktop (sec 2) made this a terminal emulator as well as an SSH
 * client, and `beerssh -e htop` is a sentence people already know how to
 * say. bssh_parse_command_line is where they are read -- all of them, in
 * one place, which they were not: this function had a hand-rolled loop
 * and platform_desktop had a second scan of its own for a preset file.
 * See sec 8.6, and --help.
 *
 * --self-test constructs the window, pumps the event loop once and exits
 * 0. It is what CI runs under the offscreen QPA plugin (sec 12.4): a GUI
 * that cannot be launched headlessly is a GUI CI cannot check at all, and
 * this is the cheapest possible smoke test that the widget tree, the
 * platform implementation and the font metrics all come up.
 */
namespace {

/*
 * The saved session --load names (sec 8.6), or an invalid profile.
 *
 * Resolved here, before a window exists, so that a name that matches
 * nothing is a message on stderr and a non-zero exit rather than a dialog
 * over a window that should not have opened. A console program's errors
 * belong on the console.
 *
 * The store is read directly rather than through the window's, which is
 * built with the same (default) directory a moment later. Two readers of
 * one INI file is what it is designed for -- sec 10.6 expects the files
 * to be hand-edited while the app runs.
 */
bssh_profile_t saved_session_named(const QString &name, QString *error) {
	bssh_profile_store store;

	for (const bssh_profile_t &profile : store.profiles()) {
		if (profile.name.compare(name, Qt::CaseInsensitive) == 0) return profile;
	}

	*error = QStringLiteral("no saved session called \"%1\"").arg(name);
	return bssh_profile_t();
}

} /* namespace */

int main(int argc, char *argv[]) {
	/*
	 * Read BEFORE QApplication exists, and Qt is then handed nothing but
	 * the program's own name.
	 *
	 * Not a style choice -- measured. Qt's XCB plugin eats `--geometry`,
	 * `--title` and `--name` out of argv on the way past, and the Wayland
	 * and offscreen plugins leave them alone: `beerssh --title Logs` set
	 * a title on one desktop and vanished on another, and `--geometry
	 * 80x24` was twenty-four ROWS here and twenty-four PIXELS there. Qt
	 * has its own meanings for those spellings and is entitled to them;
	 * what it cannot have is this program's command line, whose whole
	 * value is that it is xterm's (sec 8.6).
	 *
	 * The cost is that Qt's toolkit options are no longer accepted on the
	 * command line. They all have environment variables -- QT_QPA_PLATFORM
	 * is what `make test` and tool/check-deb.sh already use -- and an
	 * option that works on one display server is worse than one that is
	 * not offered.
	 */
	QStringList raw;
	for (int i = 0; i < argc; ++i) raw.append(QString::fromLocal8Bit(argv[i]));

	bssh_command_line_t line = bssh_parse_command_line(raw);
	bssh_set_command_line(line);

	QTextStream out(stdout);
	QTextStream err(stderr);

	/*
	 * Everything that only prints is answered HERE, before QApplication
	 * exists -- because QApplication needs a display, and these are
	 * exactly the invocations that happen without one.
	 *
	 * Found by running it: with DISPLAY unset, `beerssh --help` died with
	 * "could not connect to display" and exit 141 rather than printing
	 * anything. That is `--help` over SSH, `--help` in a container, and
	 * `--man` in the package build that generates beerssh.1 -- three
	 * places a terminal emulator has no business needing a screen.
	 *
	 * The version comes from the build's own define rather than from
	 * QCoreApplication::applicationVersion(), which nothing has set yet;
	 * it is the same string, and setApplicationVersion below reads it too.
	 */
	if (!line.error.isEmpty()) {
		err << "beerssh: " << line.error << Qt::endl
		     << "Try 'beerssh --help'." << Qt::endl;
		return 2;
	}
	if (line.help) {
		out << bssh_command_line_usage();
		return 0;
	}
	if (line.version) {
		out << "beerssh " << BSSH_VERSION_STRING << Qt::endl;
		return 0;
	}
	if (line.man) {
		out << bssh_command_line_man_page(QStringLiteral(BSSH_VERSION_STRING));
		return 0;
	}

	int qt_argc = 1;
	char *qt_argv[] = { argv[0], nullptr };
	QApplication app(qt_argc, qt_argv);

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
	 * before tool/fetch-fonts.sh has run, the catalog falls back to
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

	/*
	 * What the command line asked to open, as one profile (sec 8.6).
	 *
	 * A destination, a saved session by name, or a local shell dressed
	 * with whatever -e, --workdir and -T said. Assembled here rather than
	 * inside the window because every way of getting it wrong is a
	 * console error -- an unknown session name, a directory that is not
	 * one -- and those belong on stderr before a window appears.
	 */
	bssh_profile_t launch;

	if (!line.saved_session.isEmpty()) {
		QString reason;
		launch = saved_session_named(line.saved_session, &reason);
		if (!launch.is_valid()) {
			err << "beerssh: " << reason << Qt::endl;
			return 2;
		}
	} else if (!line.target.isEmpty()) {
		launch = bssh_profile_t::from_target(line.target);
	} else if (line.opens_a_session()) {
		/*
		 * -e, --workdir, -T or --hold with no destination: a local
		 * shell, on either platform. The DEFAULT differs by device
		 * (sec 8.5) and an explicit request does not -- somebody who
		 * typed a command has said which they want.
		 */
		launch = bssh_profile_t::local_shell();
	}

	if (launch.is_valid()) {
		launch.command = line.command;
		launch.working_directory = line.working_directory;
		launch.hold = line.hold;
		if (!line.title.isEmpty()) launch.name = line.title;
	}

	/*
	 * Before the first window, so nothing is painted in one theme and
	 * repainted in the other (sec 6.2). The default is to follow the
	 * desktop, and the detection behind that is bssh_platform's -- Qt's
	 * own hint is not enough on its own.
	 */
	bssh_theme::apply(bssh_theme::from_text(bssh_app_settings().theme()));

	/* --self-test never opens anything: CI has no server and no shell to run. */
	bssh_main_window window(line.self_test ? bssh_profile_t() : launch);
	window.resize(900, 600);

	/*
	 * Last time's tabs, and then what this device opens with (sec 8.5) --
	 * both skipped when the command line already named a session, which
	 * would otherwise be buried under the restored ones.
	 *
	 * Here rather than in the window's constructor, for the reason
	 * open_default_session() gives: each creates sessions, and a window
	 * is a widget tree rather than a running program.
	 *
	 * Before show() and before the sizing below, which is what lets
	 * --geometry be measured against the terminal that will actually be
	 * on screen rather than against a guess.
	 */
	if (!line.self_test && !launch.is_valid()) {
		window.restore_layout();
		window.open_default_session();
	}

	/*
	 * --geometry is in CELLS (sec 8.6), so the window is sized from what
	 * a grid that big measures rather than from a pixel count: the same
	 * 80x24 has to mean the same terminal in a large font as in a small
	 * one, which is the whole reason xterm counts this way.
	 */
	if (line.cols > 0 && line.rows > 0) {
		window.resize(window.size_for_grid(line.cols, line.rows));
	}
	if (line.has_position) window.move(line.x, line.y);

	window.show();

	if (line.self_test) {
		QTimer::singleShot(0, &app, &QCoreApplication::quit);
		return app.exec();
	}

	/*
	 * After show(), so a confirmation has a window to sit on, and never
	 * under --self-test, which has nobody to dismiss it.
	 */
	window.import_launch_document();

	return app.exec();
}
