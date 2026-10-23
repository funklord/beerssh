#include <QApplication>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include "platform/headless.h"
#include "model/app_settings.h"
#include "ui/interface_scale.h"
#include "model/command_line.h"
#include "term/term_features.h"
#include "model/profile.h"
#include "model/profile_store.h"
#include "model/session_registry.h"
#include "model/handoff_point.h"
#include "model/session.h"
#include "model/font_download.h"
#include "platform/platform.h"
#include "term/font_catalog.h"
#include "model/custom_font.h"
#include "term/picture_spill.h"
#include "hang_watchdog.h"
#include "ui/main_window.h"
#include "ui/store_view.h"
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
	if (!line.listing.isEmpty()) {
		out << line.listing;
		return 0;
	}
	if (line.help) {
		out << bssh_command_line_usage();
		return 0;
	}
	if (line.version) {
		out << "beerssh " << BSSH_VERSION_STRING << Qt::endl
		     << BSSH_COPYRIGHT_LINE << Qt::endl;
		return 0;
	}
	if (line.man) {
		out << bssh_command_line_man_page(QStringLiteral(BSSH_VERSION_STRING));
		return 0;
	}
	if (line.completion) {
		out << bssh_command_line_completion();
		return 0;
	}

	/*
	 * `--load=?`, answered here with the others and for the same reason:
	 * a script asking what this program can open has no business needing
	 * a display, and the shell completion that calls it runs in every
	 * terminal somebody opens.
	 *
	 * The identity is set BEFORE the store is built, because the store's
	 * default directory is QStandardPaths::AppConfigLocation and that
	 * reads QCoreApplication's application name -- which is a static and
	 * answers with no instance, but answers "beerssh" only once it has
	 * been told. Without this the listing would read a different
	 * directory from the one the app uses, and would confidently print
	 * nothing.
	 *
	 * Setting them twice is harmless: the calls below are left where they
	 * are so the ordinary path still reads in one place.
	 */
	if (line.saved_session == QStringLiteral("?")) {
		QCoreApplication::setOrganizationDomain(QStringLiteral("vibes.se"));
		QCoreApplication::setApplicationName(QStringLiteral("beerssh"));

		out << bssh_saved_session_listing();
		return 0;
	}

	/*
	 * A store screenshot (ui/store_view.h): named on the command line, or
	 * on Android by the launching intent once there is an application to
	 * ask. Checked against the names here, so a misspelt view is a refused
	 * start rather than a real window photographed in its place.
	 */
	if (!line.store_view.isEmpty() && !bssh_store_view_names().contains(line.store_view)) {
		err << "beerssh: no store view named \"" << line.store_view << "\"; there are "
		     << bssh_store_view_names().join(QStringLiteral(", ")) << Qt::endl;
		return 2;
	}

	int qt_argc = 1;
	char *qt_argv[] = { argv[0], nullptr };
	bssh_drop_theme_when_offscreen();
	QApplication app(qt_argc, qt_argv);

	QCoreApplication::setOrganizationDomain(QStringLiteral("vibes.se"));
	QCoreApplication::setApplicationName(QStringLiteral("beerssh"));
	/* Set by the build (see the Makefile's VERSION), so the app, the APK
	 * and the Debian package cannot disagree about what this is. */
	QCoreApplication::setApplicationVersion(QStringLiteral(BSSH_VERSION_STRING));
	/* Freedesktop app ID, so the window matches its .desktop entry (sec 0.1). */
	QGuiApplication::setDesktopFileName(QStringLiteral("se.vibes.beerssh"));

	if (line.store_view.isEmpty()) {
		if (bssh_platform *platform = bssh_platform::instance())
			line.store_view = platform->launch_extra(QStringLiteral("store_view"));
	}

	/*
	 * Isolated before ANY store is opened -- the settings read just below
	 * included -- and refused outright if it cannot be: a store screenshot
	 * of the real data is the one outcome this must never produce.
	 */
	if (!line.store_view.isEmpty()) {
		if (!bssh_store_view_names().contains(line.store_view)) {
			qWarning("store view: no view named %s", qPrintable(line.store_view));
			return 2;
		}
		if (!bssh_store_view_isolate()) return 1;
	}

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

	/*
	 * Scale the interface with the system font-size accessibility setting
	 * (sec 9.5), before any widget is built so chrome is measured at the
	 * right size from the first one. The terminal grid is untouched: it
	 * builds its own font with an explicit point size, so this moves the
	 * menus, forms and host list around it and never the column count.
	 *
	 * Only Android reports anything but 1.0. A font-size change does NOT
	 * recreate the activity -- the manifest handles fontScale itself -- so
	 * this is applied again each time the app comes back to the front,
	 * which is when a change made in Android's Settings can arrive. Off is
	 * honoured for the user who wants this app's chrome at its own size.
	 */
	bssh_follow_interface_scale();

	/*
	 * Pictures a crashed instance spilled (sec 7.1). Sealed under a key
	 * that died with it, so they are unreadable noise -- but they are
	 * still disk, and nothing else will ever remove them.
	 */
	bssh_picture_spill::sweep_abandoned(bssh_picture_spill::default_base());

	if (platform) {
		const QString font_dir = platform->font_directory();
		int faces = bssh_font_catalog::instance().load_bundled(font_dir);

		/*
		 * And whatever was downloaded on this device (sec 9.4), after the
		 * bundle so a bundled face is never shadowed by a fetched copy.
		 */
		faces += bssh_font_catalog::instance().load_bundled(bssh_downloaded_font_directory());

		/* And the user's own file, if one was imported (sec 9.4). */
		bssh_load_custom_font();

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

	/*
	 * Every start at full brightness with the contrast floor on: those two
	 * last only as long as a run, and the values older versions stored --
	 * a 10 that kept bringing the dim back -- are taken out here, once.
	 */
	bssh_app_settings().forget_session_appearance();

	/* --self-test never opens anything: CI has no server and no shell to run. */

	/*
	 * --term-features, before any session exists (sec 7.1). Process-wide
	 * and ahead of every preset: a reduced terminal is an instruction
	 * about this run, so a stored preset must not restore a feature the
	 * person running the test asked to be without.
	 *
	 * The spec was already validated by the parser, so a bad name has
	 * exited long before here -- this cannot fail.
	 */
	if (!line.term_features.isEmpty()) {
		QString ignored;
		bssh_term_features_t::set_process_override(
		        bssh_term_features_t::from_spec(line.term_features, ignored));
	}

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
	if (!line.self_test && !launch.is_valid() && line.store_view.isEmpty() &&
	    line.adopt_from.isEmpty()) {
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

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	/*
	 * Edge to edge, with the content kept inside the safe area -- which is
	 * what lets the hidden status bar's height go to the terminal. A
	 * normal window measured the bars without asking whether they were
	 * showing: hidden, the status bar left a blank band of its own height
	 * at the top, the window's frame covering the screen and Qt's content
	 * starting below the band anyway (seen on the Fold, 2026-10-09). The
	 * safe area does follow visibility. Before show(): a window flag set
	 * afterwards re-creates the window.
	 */
	window.setWindowFlag(Qt::ExpandedClientAreaHint);
#endif
	window.show();

	/* After show(): the bar belongs to the window the app now has. */
	if (bssh_platform *platform = bssh_platform::instance()) platform->hide_status_bar();

	if (!line.store_view.isEmpty()) bssh_store_view_show(&window, line.store_view);

	if (line.self_test) {
		QTimer::singleShot(0, &app, &QCoreApplication::quit);
		return app.exec();
	}

	/*
	 * The hang watchdog (sec 5.1), after show() so there is a foreground
	 * window for it to speak about, and not under --self-test above, which
	 * has no run to watch. It arms only on Android.
	 */
	bssh_start_hang_watchdog();

	/*
	 * After show(), so a confirmation has a window to sit on, and never
	 * under --self-test, which has nobody to dismiss it.
	 */
	window.import_launch_document();

	/*
	 * Kept scrollback is written when a session closes, and also at the
	 * two moments one can be lost without closing: the app quitting, and
	 * the app leaving the foreground -- on a phone the last moment it is
	 * certain to be running, since Android may end a backgrounded app
	 * without a word.
	 */
	bssh_session_registry *registry = bssh_session_registry::shared();

#ifndef Q_OS_ANDROID
	/*
	 * Where another BeerSSH asks this one for a live session (sec 8.4): a
	 * tab dropped on one of its windows, or a new version taking every
	 * session over. Desktop only -- on Android an app is one process, and
	 * an update or a process death ends all of it at once. Not for a
	 * store screenshot, whose sessions are made up and never connect.
	 */
	if (line.store_view.isEmpty()) {
		auto *handoff = new bssh_handoff_point([registry](const QString &id) -> bssh_session * {
			for (bssh_session *session : registry->sessions()) {
				if (session->handoff_id() == id &&
				    session->state() == bssh_ssh_session::CONNECTED)
					return session;
			}
			return nullptr;
		}, &app);
		handoff->set_lister([registry] {
			QStringList ids;
			for (bssh_session *session : registry->sessions()) {
				if (session->state() == bssh_ssh_session::CONNECTED)
					ids.append(session->handoff_id());
			}
			return ids;
		});
		QString why;
		if (!handoff->listen(&why)) qWarning("handoff: %s", qPrintable(why));
		QObject::connect(handoff, &bssh_handoff_point::session_given,
		                  registry, &bssh_session_registry::release);
		QObject::connect(handoff, &bssh_handoff_point::offered, &app,
		                  [](const QString &source, const QString &id, const QPoint &global) {
			bssh_main_window::take_from_another_process(source, id, global);
		});
		/* A new version has everything it could take. Anything it could
		 * not -- a session reconnecting, say, or one too busy to stop in
		 * time -- keeps this one running, rather than ending with the
		 * program. */
		QObject::connect(handoff, &bssh_handoff_point::quit_requested, &app, [registry] {
			if (registry->live_count() == 0) QCoreApplication::quit();
		});

		/*
		 * --adopt-from: this IS the new version. Every session the old one
		 * will give, taken and shown here, and then it is asked to go.
		 */
		if (!line.adopt_from.isEmpty()) {
			int taken = 0;
			QString reason;
			const QStringList ids = bssh_handoff_point::list(line.adopt_from, &reason);
			for (const QString &id : ids) {
				bssh_session *session =
				        bssh_handoff_point::take(line.adopt_from, id, registry, &reason);
				if (!session) {
					qWarning("handoff: could not take a session: %s", qPrintable(reason));
					continue;
				}
				registry->adopt(session);
				window.adopt_session(session);
				++taken;
			}
			if (taken == 0) window.open_default_session();
			bssh_handoff_point::request_quit(line.adopt_from);
		}
	}
#endif

	QObject::connect(&app, &QCoreApplication::aboutToQuit, registry,
	                  &bssh_session_registry::save_kept_scrollback);
	QObject::connect(&app, &QGuiApplication::applicationStateChanged, registry,
	                  [registry](Qt::ApplicationState state) {
		if (state != Qt::ApplicationActive) registry->save_kept_scrollback();
	});

	return app.exec();
}
