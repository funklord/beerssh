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
struct bssh_command_line_t {
	/* [user@]host[:port]. Empty means no destination was given. */
	QString target;

	/* A preset file to import (sec 10.5), or empty. */
	QString document;

	/* A saved session to open by name (--load), or empty. */
	QString saved_session;

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

	/* CI's headless smoke test (sec 12.4). Deliberately undocumented. */
	bool self_test;

	/* Printing one of these is all the program does. */
	bool help;
	bool version;

	/*
	 * Why the command line was refused, or empty. Non-empty means print
	 * it and exit non-zero: an argument that cannot be honoured must not
	 * be silently dropped, which is the failure mode that makes people
	 * distrust a program's options.
	 */
	QString error;

	bssh_command_line_t()
	    : cols(0), rows(0), x(0), y(0), has_position(false), hold(false),
	      self_test(false), help(false), version(false) {}

	/* Nothing to open: the program prints something and stops. */
	bool exits_immediately() const { return help || version || !error.isEmpty(); }

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
