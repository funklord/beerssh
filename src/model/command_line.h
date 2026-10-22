#ifndef BSSH_COMMAND_LINE_H
#define BSSH_COMMAND_LINE_H

#include <QList>
#include <QString>
#include <QStringList>

/*
 * bssh_command_line_t -- what the command line asked for (project.md
 * sec 8.6).
 *
 * BeerSSH opens a local shell on desktop (sec 2), which makes it a
 * terminal emulator as well as an SSH client, and a terminal emulator is
 * expected to take xterm's and konsole's arguments. Somebody typing
 * `beerssh -e htop` is not experimenting: they are using the vocabulary
 * every terminal since 1984 has answered to.
 *
 * A struct and a free function rather than a class, for the reason
 * bssh_tab_status is a namespace: parsing a command line is a pure
 * mapping from strings to a decision, and it is testable exactly as far
 * as it stays one. Nothing in here builds a window, reads a file or
 * connects to anything.
 *
 * It replaces TWO parsers. main() had a hand-rolled loop that took
 * --self-test and the first bare argument as a target, and
 * platform_desktop's launch_document() separately re-scanned argv for a
 * *.ini to import -- so the command line was read in two places that
 * agreed only by coincidence, and neither could report an error about
 * the other's arguments.
 */
/*
 * Who wrote this, in one place because it appears in three (harmonization
 * .md, 2026-08-29): --version, the About box, and the README.
 *
 * ATTRIBUTION, NOT LICENSING. Naming the holder is a statement of fact --
 * authorship vests automatically and saying who wrote something grants
 * nothing. It creates no obligation to add a LICENSE and does not touch
 * this project's licensing position, which CLAUDE.md reserves to the
 * copyright holder alone.
 *
 * Here rather than in main() so the About box and the command line cannot
 * drift: three surfaces reading one constant is the whole reason it is a
 * constant. The year is the one the work began, 2026, per the rule --
 * not a year chosen to fill the field.
 */
#define BSSH_COPYRIGHT_LINE "Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>"

struct bssh_command_line_t {
	/* [user@]host[:port]. Empty means no destination was given. */
	QString target;

	/* A preset file to import (sec 10.5), or empty. */
	QString document;

	/* A saved session to open by name (--load), or empty. */
	QString saved_session;

	/*
	 * `--load=?` asks what the option accepts rather than naming one.
	 * Answered in main() like the other listings -- it needs the host
	 * store, which the parser has no business opening.
	 */

	/*
	 * What to run instead of a shell (-e), argv-style and already split.
	 *
	 * Empty means the login shell locally, and the remote's own shell
	 * over SSH -- which is what every one of these programs does without
	 * -e, and is the only sane default for a terminal.
	 */
	QStringList command;

	/* Where to start the local shell (--workdir), or empty for inherited. */
	QString working_directory;

	/*
	 * The name this session carries until the remote sets its own with
	 * OSC (-T). It becomes the profile name, which is the tab label and
	 * the window title (sec 8.3) -- and OSC still wins, exactly as
	 * xterm's -T loses to a shell that sets a title.
	 */
	QString title;

	/*
	 * The terminal grid in CELLS, xterm's --geometry semantics rather
	 * than pixels: a terminal is measured in columns and rows, and a
	 * pixel size means a different grid on every font and every display.
	 * Zero means unset.
	 */
	int cols;
	int rows;

	/* The +X+Y half of --geometry, in pixels. Unset unless has_position. */
	int x;
	int y;
	bool has_position;

	/*
	 * Keep the pane when what it was running ends (--hold), rather than
	 * closing it. xterm's -hold and konsole's --hold, and the reason both
	 * have one: a command that fails prints why and exits, and a terminal
	 * that closes on exit takes the message with it.
	 */
	bool hold;

	/*
	 * Which of sec 7.1's terminal extensions to speak, as a spec string
	 * ("-sixel,-kitty-graphics"). Empty means all of them.
	 *
	 * On the command line because the thing it is for is a test run: a
	 * TUI library has to work against terminals that lack each of these,
	 * and a harness needs to say so per invocation rather than by editing
	 * a stored preset first. The same string is a preset field, so a host
	 * that always wants a reduced terminal can say it once.
	 */
	QString term_features;

	/* CI's headless smoke test (sec 12.4). */
	bool self_test;

	/* A store screenshot's view (ui/store_view.h); validated by main. */
	QString store_view;

	/* Printing one of these is all the program does. */
	bool help;
	bool version;
	bool man;
	bool completion;

	/*
	 * Something to print on stdout and stop, or empty --
	 * `--term-features=?` is the only one.
	 *
	 * Separate from `error` because it is not one: a listing is a
	 * question answered, so it belongs on stdout with a zero status,
	 * exactly like --help. Carried through `error` first, which sent a
	 * successful answer to stderr and exited 2 -- and a script asking
	 * what a program supports would have read that as the program
	 * refusing to run.
	 */
	QString listing;

	/*
	 * Why the command line was refused, or empty. Non-empty means print
	 * it and exit non-zero: an argument that cannot be honoured must not
	 * be silently dropped, which is the failure mode that makes people
	 * distrust a program's options.
	 */
	QString error;

	bssh_command_line_t()
	    : cols(0), rows(0), x(0), y(0), has_position(false), hold(false),
	      self_test(false), help(false), version(false), man(false),
	      completion(false) {}

	/* Nothing to open: the program prints something and stops. */
	bool exits_immediately() const {
		return help || version || man || completion || !listing.isEmpty()
		        || !error.isEmpty();
	}

	/*
	 * Whether the command line names a session to open at startup.
	 *
	 * A destination or a command obviously does. So does --workdir, -T or
	 * --hold ALONE, and that is the answer rather than an oversight: each
	 * of them describes a session, so `beerssh -T Logs` has asked for one
	 * and named it. Reading them as decoration on whatever the app would
	 * have opened anyway would mean a title that applied to a restored
	 * tab from last time.
	 */
	bool opens_a_session() const {
		return !target.isEmpty() || !saved_session.isEmpty() || !command.isEmpty()
		       || !working_directory.isEmpty() || !title.isEmpty() || hold;
	}
};

/*
 * One option, as both the parser and the help text see it.
 *
 * The table below is the single source for both. An option list written
 * out a second time to make a help text is a copy free to drift from the
 * behaviour -- the failure sec 6.1 describes for the stolen chords, where
 * the only way to learn which chords never reached your shell was to
 * press one. A test walks this table in both directions.
 */
struct bssh_option_t {
	/* Without the dashes. "execute", not "--execute". */
	const char *name;

	/* One letter, or empty where the option has no short form. */
	const char *short_name;

	/* What the value is called in the help text; empty for a flag. */
	const char *value_name;

	const char *description;
};

/* Every option this program accepts, in the order --help lists them. */
const QList<bssh_option_t> &bssh_command_line_options();

/*
 * Parses QCoreApplication::arguments() -- argv[0] included, and skipped.
 *
 * Never exits, never prints: --help and --version come back as fields so
 * that a test can ask what a command line means without the process it
 * runs in disappearing. QCommandLineParser::process() would do both.
 */
bssh_command_line_t bssh_parse_command_line(const QStringList &arguments);

/*
 * The --help text. Ours rather than QCommandLineParser's default so that
 * the examples can be in it: an option list tells you that -e exists and
 * not that everything after it belongs to the command.
 */
QString bssh_command_line_usage();

/*
 * The manual page, in roff, built from the same table.
 *
 * Generated rather than kept as a file for the reason the help text is
 * generated: a second copy of the option list is a copy that goes stale,
 * and a manual page is exactly where nobody would notice -- it documents
 * a flag that no longer exists and omits the one you wanted, and neither
 * is an error. fmake does the same, from the same argument, and
 * `make man` writes what this returns into beerssh.1.
 *
 * The version is passed in rather than read from QCoreApplication,
 * because --man runs before there is one (see bssh_command_line()).
 */
QString bssh_command_line_man_page(const QString &version);

/*
 * The bash completion script, built from the same table, for the reason
 * the manual page is: a completion kept as a file is a third copy of the
 * option list, and it is worse than the page at showing its age -- a
 * page that omits a flag is merely unhelpful, while a completion that
 * omits one actively refuses to finish what somebody is typing, and they
 * conclude the flag does not exist.
 *
 * Bash only, and deliberately: zsh and fish want different shapes rather
 * than the same script under another name, and a generator that emits
 * three dialects nobody here runs is three things to be wrong. The table
 * is what matters; another shell is another function over it when
 * somebody wants one.
 *
 * Needs no display, like --man and for the same reason -- it is answered
 * before QApplication exists, so a package build can generate it.
 */
QString bssh_command_line_completion();

/*
 * The command line THIS PROCESS was started with, parsed once.
 *
 * It exists because main() is no longer the only reader: the desktop
 * platform answers launch_document() from it (sec 10.5), and it cannot
 * ask QCoreApplication -- main() deliberately hands Qt nothing but the
 * program's own name, so that Qt's toolkit arguments cannot eat ours.
 *
 * That is not a precaution. Qt's XCB plugin consumes `--geometry`,
 * `--title` and `--name` before the program sees them, and the Wayland
 * and offscreen plugins do not -- so `beerssh --title Logs` set a title
 * on one desktop and was silently swallowed on another, and
 * `--geometry 80x24` meant cells in one place and pixels in the other.
 * A flag whose meaning depends on the display server is worse than one
 * that does not exist. Qt's own options are reachable through its
 * environment variables (QT_QPA_PLATFORM and the rest), which is how CI
 * and the packaging check already ask for them.
 *
 * Falls back to parsing QCoreApplication::arguments() when nothing has
 * been set, which is what tests and any other entry point get.
 */
const bssh_command_line_t &bssh_command_line();

/* Installs what main() parsed, before anything else asks. */
void bssh_set_command_line(const bssh_command_line_t &line);

#endif /* BSSH_COMMAND_LINE_H */
