#include "model/command_line.h"

#include "term/term_features.h"

#include "model/document_path.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QRegularExpression>

namespace {

/*
 * The largest grid --geometry will accept.
 *
 * Not a limit of anything: it is what tells a typo from an intention. A
 * window asked for 100000 columns is sized past every screen it could be
 * put on and cannot be dragged back, so it is refused with a message
 * rather than obeyed into a state the user has to kill the process to
 * leave.
 */
const int BSSH_MAX_CELLS = 1000;

/* Where the description column starts in the help text. */
const int BSSH_HELP_COLUMN = 32;
const int BSSH_HELP_WIDTH = 79;

QString option_syntax(const bssh_option_t &option) {
	QString text;

	if (*option.short_name) {
		text += QLatin1Char('-');
		text += QString::fromLatin1(option.short_name);
		text += QStringLiteral(", ");
	} else {
		text += QStringLiteral("    ");
	}

	text += QStringLiteral("--");
	text += QString::fromLatin1(option.name);

	if (*option.value_name) {
		text += QStringLiteral(" <");
		text += QString::fromLatin1(option.value_name);
		text += QLatin1Char('>');
	}

	return text;
}

/* Wraps a description under BSSH_HELP_COLUMN, which is where it starts. */
QString wrapped_description(const QString &text) {
	const QString indent(BSSH_HELP_COLUMN, QLatin1Char(' '));
	QString out;
	QString line;

	const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (const QString &word : words) {
		const int would_reach = BSSH_HELP_COLUMN + line.size() + 1 + word.size();

		if (!line.isEmpty() && would_reach > BSSH_HELP_WIDTH) {
			out += line + QLatin1Char('\n') + indent;
			line.clear();
		}
		if (!line.isEmpty()) line += QLatin1Char(' ');
		line += word;
	}

	return out + line;
}

} /* namespace */

const QList<bssh_option_t> &bssh_command_line_options() {
	/*
	 * xterm's and konsole's spellings wherever they have one, because the
	 * whole value of these arguments is that they are already in
	 * somebody's fingers -- a terminal with its own vocabulary for
	 * "run this command" is one whose options have to be looked up every
	 * time.
	 *
	 * Long forms are double-dashed, which is konsole's convention rather
	 * than xterm's single dash: this is a Qt program, QCommandLineParser
	 * reads them, and -hold would otherwise have to be told apart from
	 * -h -o -l -d by hand.
	 */
	static const QList<bssh_option_t> options = {
		{ "execute", "e", "command",
			"Run a command instead of a shell. Everything after -e is "
			"the command and its arguments, so it comes last. Locally "
			"the command is run directly, with no shell to quote for; "
			"over SSH it is handed to the remote's shell, which is what "
			"ssh does." },
		{ "hold", "", "",
			"Keep the pane when what it was running ends, rather than "
			"closing it. A command that fails prints why and exits, and "
			"the closing takes the message with it." },
		{ "workdir", "", "directory",
			"Start the local shell here rather than in the directory "
			"beerssh was started from. Local sessions only -- SSH has no "
			"way to ask." },
		{ "title", "T", "title",
			"What to call this session until the program running in it "
			"says otherwise. It is the tab label and the window title." },
		{ "geometry", "", "COLSxROWS[+X+Y]",
			"The terminal size in CELLS, as xterm means it -- 80x24 is "
			"eighty columns and twenty-four rows, whatever the font. An "
			"optional +X+Y places the window, in pixels." },
		{ "load", "", "session",
			"Open a saved session by name, as PuTTY's -load does. The "
			"name is the one the host list shows. Run --load=? to list "
			"them." },
		{ "term-features", "", "spec",
			"Which terminal extensions to speak, for testing a TUI "
			"against a terminal that lacks one. A comma-separated list "
			"read left to right, starting from everything on: `none` "
			"or `all` set every one at once, `-name` turns one off and "
			"`+name` turns it back on -- so `none,+bracketed-paste` is "
			"a terminal with nothing but that. Turning one off also "
			"stops this terminal answering that it has it. Run "
			"--term-features=? to list the names." },
		{ "self-test", "", "",
			"Build the window, run one turn of the event loop and exit. "
			"What CI runs to check that a headless build comes up at "
			"all." },
		{ "man", "", "",
			"Write this program's manual page to stdout and exit. It is "
			"generated from the same table these options are read from, so "
			"beerssh.1 cannot describe a flag the program does not take." },
		{ "completion", "", "",
			"Write a bash completion script to stdout and exit. Generated "
			"from the same table as the manual page, so it cannot offer a "
			"flag the program does not take or miss one it does." },
		{ "help", "h", "", "Show this help and exit." },
		{ "version", "v", "", "Show the version and exit." },
	};

	return options;
}

QString bssh_command_line_usage() {
	QString text = QStringLiteral(
	        "Usage: beerssh [options] [[user@]host[:port] | preset.ini]\n"
	        "\n"
	        "An SSH terminal, and a terminal. With no arguments it opens a local\n"
	        "shell on a desktop and its host list on a phone.\n"
	        "\n"
	        "Options:\n");

	for (const bssh_option_t &option : bssh_command_line_options()) {
		QString line = QStringLiteral("  ") + option_syntax(option);

		/*
		 * A syntax that has run past the description column gets its
		 * own line rather than pushing the column out for everything
		 * else -- one long option name should not re-indent the page.
		 */
		if (line.size() >= BSSH_HELP_COLUMN) {
			line += QLatin1Char('\n') + QString(BSSH_HELP_COLUMN, QLatin1Char(' '));
		} else {
			line += QString(BSSH_HELP_COLUMN - line.size(), QLatin1Char(' '));
		}

		text += line + wrapped_description(QString::fromLatin1(option.description))
		        + QLatin1Char('\n');
	}

	text += QStringLiteral(
	        "\n"
	        "Examples:\n"
	        "  beerssh                       a local shell\n"
	        "  beerssh user@host:2222        a shell on that host\n"
	        "  beerssh -e htop               a local shell running htop\n"
	        "  beerssh user@host -e htop     htop over there, as `ssh -t` runs it\n"
	        "  beerssh --workdir /srv -T Logs -e tail -f app.log\n"
	        "  beerssh --load \"prod bastion\" a saved session, by name\n"
	        "  beerssh preset.ini            import a preset file\n");

	return text;
}

namespace {

/*
 * Escapes text for roff.
 *
 * Three things bite. A backslash starts an escape; a line beginning with a
 * dot or a quote starts a MACRO, so a description opening with one would
 * be swallowed rather than printed; and a hyphen is typeset as a
 * hyphenation point rather than as the minus a reader is meant to type,
 * which is what turns \-\-hold in a manual page into something that
 * cannot be copied out of it.
 */
QString roff(const QString &text) {
	QString out;
	out.reserve(text.size() + 8);

	for (int i = 0; i < text.size(); ++i) {
		const QChar c = text.at(i);
		if (c == QLatin1Char('\\')) {
			out += QStringLiteral("\\e");
		} else if (c == QLatin1Char('-')) {
			out += QStringLiteral("\\-");
		} else {
			out += c;
		}
	}

	if (out.startsWith(QLatin1Char('.')) || out.startsWith(QLatin1Char('\''))) {
		out.prepend(QStringLiteral("\\&"));
	}

	return out;
}

/* One option as a .TP entry: the flags in bold, the value in italic. */
QString man_option(const bssh_option_t &option) {
	QString names;

	if (*option.short_name) {
		const QString letter = QString::fromLatin1(option.short_name);
		names += QStringLiteral("\\fB\\-%1\\fR, ").arg(letter);
	}

	names += QStringLiteral("\\fB\\-\\-%1\\fR").arg(QString::fromLatin1(option.name));

	if (*option.value_name) {
		const QString value = roff(QString::fromLatin1(option.value_name));
		names += QStringLiteral(" \\fI%1\\fR").arg(value);
	}

	const QString what = roff(QString::fromLatin1(option.description));

	return QStringLiteral(".TP\n%1\n%2\n").arg(names, what);
}

} /* namespace */

QString bssh_command_line_man_page(const QString &version) {
	QString out = QStringLiteral(
	        ".TH BEERSSH 1 \"\" \"beerssh %1\" \"User Commands\"\n"
	        ".SH NAME\n"
	        "beerssh \\- SSH terminal, and terminal, for desktop and Android\n"
	        ".SH SYNOPSIS\n"
	        ".B beerssh\n"
	        ".RI [ options ]\n"
	        ".RI [ user @] host [: port ]\n"
	        ".br\n"
	        ".B beerssh\n"
	        ".RI [ options ]\n"
	        ".I preset.ini\n"
	        ".SH DESCRIPTION\n").arg(version);

	out += roff("beerssh connects to SSH servers and renders the result "
	             "through its own VT emulator -- colours, scrollback, the "
	             "alternate screen, mouse reporting and inline images. On a "
	             "desktop it also opens a local shell, which is why it takes "
	             "the arguments a terminal emulator is expected to take.")
	       + QStringLiteral("\n.PP\n");

	out += roff("With no arguments it opens a local shell on a desktop and "
	             "its host list on a phone. A destination connects to that "
	             "host; a path ending in .ini is imported as a preset.")
	       + QStringLiteral("\n.PP\n");

	out += roff("Everything after -e belongs to the command, so -e comes "
	             "last. Locally the command is run directly, with no shell "
	             "between it and the kernel; over SSH the words are joined "
	             "and handed to the remote's own shell, which is what ssh "
	             "does and why that one needs quoting.")
	       + QStringLiteral("\n.PP\n");

	out += roff("A shell that ends closes its pane, and the window with it "
	             "if that was the last one. --hold keeps it, which is what "
	             "you want when a command has printed something worth "
	             "reading on its way out.")
	       + QStringLiteral("\n");

	out += QStringLiteral(".SH OPTIONS\n");
	for (const bssh_option_t &option : bssh_command_line_options()) {
		out += man_option(option);
	}

	out += QStringLiteral(
	        ".SH EXAMPLES\n"
	        ".TP\n"
	        "\\fBbeerssh\\fR\n"
	        "A local shell.\n"
	        ".TP\n"
	        "\\fBbeerssh user@host:2222\\fR\n"
	        "A shell on that host, over SSH.\n"
	        ".TP\n"
	        "\\fBbeerssh \\-e htop\\fR\n"
	        "A local shell running htop, which closes when htop does.\n"
	        ".TP\n"
	        "\\fBbeerssh user@host \\-e htop\\fR\n"
	        "htop over there, the way \\fBssh \\-t\\fR runs it.\n"
	        ".TP\n"
	        "\\fBbeerssh \\-\\-workdir /srv \\-T Logs \\-e tail \\-f app.log\\fR\n"
	        "A named window, watching a file, started in /srv.\n"
	        ".TP\n"
	        "\\fBbeerssh \\-\\-geometry 120x40\\fR\n"
	        "A hundred and twenty columns by forty rows, whatever the font.\n"
	        ".SH FILES\n"
	        ".TP\n"
	        "\\fB~/.ssh/beerssh_hosts\\fR\n"
	        "Saved sessions, in INI, one section per host. Meant to be "
	        "readable and hand-editable.\n"
	        ".TP\n"
	        "\\fB~/.config/beerssh/presets.ini\\fR\n"
	        "Host presets: the settings a saved session inherits.\n"
	        ".TP\n"
	        "\\fB~/.config/beerssh/layout.ini\\fR\n"
	        "Which hosts were open last time, and how they were arranged.\n"
	        ".SH ENVIRONMENT\n");

	out += roff("Qt's own options are not accepted on the command line, "
	             "because its X11 plugin consumes --geometry, --title and "
	             "--name before this program sees them while its other "
	             "plugins do not. Ask for them through Qt's environment "
	             "variables instead -- QT_QPA_PLATFORM and the rest.")
	       + QStringLiteral("\n");

	out += QStringLiteral(
	        ".SH EXIT STATUS\n"
	        ".TP\n"
	        "\\fB0\\fR\n"
	        "The window was closed, or a printing option finished.\n"
	        ".TP\n"
	        "\\fB2\\fR\n"
	        "The command line was refused; the reason is on stderr.\n"
	        ".SH SEE ALSO\n"
	        ".BR ssh (1),\n"
	        ".BR ssh_config (5),\n"
	        ".BR xterm (1)\n");

	return out;
}

QString bssh_command_line_completion() {
	/*
	 * One function, no _init dance: bash-completion sources this file
	 * when the word `beerssh` is first completed, so the body has to be
	 * the whole of it.
	 *
	 * `-o default` is what makes the positional argument work. The thing
	 * after the options is `[user@]host[:port]` or a preset file, and
	 * only one of those can be completed from the filesystem -- so when
	 * this offers nothing, bash falls back to its own filename
	 * completion rather than to silence, and somebody typing a path to a
	 * preset is not left wondering whether the program is broken.
	 */
	QStringList flags;
	QStringList takes_a_command;
	QStringList takes_a_directory;
	QStringList takes_a_session;
	QStringList takes_something_else;

	for (const bssh_option_t &option : bssh_command_line_options()) {
		const QString name = QStringLiteral("--")
		        + QString::fromLatin1(option.name);
		const QString shrt = QString::fromLatin1(option.short_name);

		flags.append(name);
		if (!shrt.isEmpty()) flags.append(QStringLiteral("-") + shrt);

		if (!*option.value_name) continue;

		/*
		 * Both spellings, because `beerssh -e <TAB>` is the one people
		 * actually type and a case arm listing only the long form
		 * would fall through to the option list there -- offering
		 * `--help` as the command to run.
		 */
		QStringList spellings;
		spellings.append(name);
		if (!shrt.isEmpty()) spellings.append(QStringLiteral("-") + shrt);

		const QString value = QString::fromLatin1(option.value_name);
		if (value == QLatin1String("command")) takes_a_command += spellings;
		else if (value == QLatin1String("directory")) takes_a_directory += spellings;
		else if (value == QLatin1String("session")) takes_a_session += spellings;
		else takes_something_else += spellings;
	}

	QString out = QStringLiteral(
	        "# bash completion for beerssh -- GENERATED by `beerssh "
	        "--completion`.\n"
	        "# Built from the option table the parser reads, so it cannot "
	        "offer a flag\n"
	        "# the program does not take. Do not edit; regenerate.\n"
	        "\n"
	        "_beerssh()\n"
	        "{\n"
	        "\tlocal cur prev\n"
	        "\tcur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
	        "\tprev=\"${COMP_WORDS[COMP_CWORD-1]}\"\n"
	        "\n");

	/*
	 * The two value shapes worth completing, keyed on the table's own
	 * value_name rather than on the option's name: a directory is a
	 * directory on anybody's machine and a command is on their PATH, so
	 * an option added later that takes one of those is completed without
	 * anybody editing this.
	 *
	 * Everything else takes a value this script has nothing true to say
	 * about -- a session name lives in the user's host store, a
	 * term-features spec is a grammar, a title is free text -- so it
	 * says nothing, deliberately, rather than falling through to the
	 * option list and offering `--title --version` as though that were
	 * a sensible thing to type.
	 */
	out += QStringLiteral("\tcase \"$prev\" in\n");

	if (!takes_a_directory.isEmpty()) {
		out += QStringLiteral("\t\t%1)\n")
		        .arg(takes_a_directory.join(QLatin1Char('|')));
		out += QStringLiteral(
		        "\t\t\tCOMPREPLY=( $(compgen -d -- \"$cur\") )\n"
		        "\t\t\treturn 0\n"
		        "\t\t\t;;\n");
	}

	if (!takes_a_command.isEmpty()) {
		out += QStringLiteral("\t\t%1)\n")
		        .arg(takes_a_command.join(QLatin1Char('|')));
		out += QStringLiteral(
		        "\t\t\tCOMPREPLY=( $(compgen -c -- \"$cur\") )\n"
		        "\t\t\treturn 0\n"
		        "\t\t\t;;\n");
	}

	/*
	 * A session name, which only this installation knows -- so the
	 * script asks the program rather than guessing. `--load=?` prints
	 * them one per line with a zero status, which is the shape that
	 * makes it readable here as well as by a person.
	 *
	 * `2>/dev/null` because a completion must never print: a program
	 * that cannot read its host list would otherwise spray an error
	 * across the command line somebody is still typing.
	 */
	if (!takes_a_session.isEmpty()) {
		out += QStringLiteral("\t\t%1)\n")
		        .arg(takes_a_session.join(QLatin1Char('|')));
		out += QStringLiteral(
		        "\t\t\tlocal IFS=$'\\n'\n"
		        "\t\t\tCOMPREPLY=( $(compgen -W \"$(beerssh --load=? 2>/dev/null)\" "
		        "-- \"$cur\") )\n"
		        "\t\t\treturn 0\n"
		        "\t\t\t;;\n");
	}

	if (!takes_something_else.isEmpty()) {
		out += QStringLiteral("\t\t%1)\n")
		        .arg(takes_something_else.join(QLatin1Char('|')));
		out += QStringLiteral(
		        "\t\t\treturn 0\n"
		        "\t\t\t;;\n");
	}

	out += QStringLiteral("\tesac\n\n");

	out += QStringLiteral(
	        "\tif [[ \"$cur\" == -* ]]; then\n"
	        "\t\tCOMPREPLY=( $(compgen -W \"%1\" -- \"$cur\") )\n"
	        "\t\treturn 0\n"
	        "\tfi\n"
	        "\n"
	        "\treturn 0\n"
	        "}\n"
	        "\n"
	        "complete -F _beerssh -o default beerssh\n")
	        .arg(flags.join(QLatin1Char(' ')));

	return out;
}

bssh_command_line_t bssh_parse_command_line(const QStringList &arguments) {
	bssh_command_line_t line;

	/*
	 * -e takes the REST of the command line, which no option parser
	 * models: `-e tail -f app.log` must not have -f read as beerssh's.
	 * xterm and konsole both say so in their manuals and both do it by
	 * splitting argv first, which is what happens here -- everything from
	 * -e onwards is set aside before the parser sees anything.
	 *
	 * --execute=htop still works and means a one-word command, because
	 * that spelling carries its own value and needs no rest-of-line rule.
	 */
	QStringList head;
	int split = -1;

	for (int i = 1; i < arguments.size(); ++i) {
		const QString &argument = arguments.at(i);
		if (argument == QLatin1String("-e") || argument == QLatin1String("--execute")) {
			split = i;
			break;
		}
		head.append(argument);
	}

	if (split >= 0) {
		line.command = arguments.mid(split + 1);
		if (line.command.isEmpty()) {
			line.error = QStringLiteral("-e needs a command to run");
			return line;
		}
	}

	QCommandLineParser parser;
	QList<QCommandLineOption> declared;

	for (const bssh_option_t &option : bssh_command_line_options()) {
		QStringList names(QString::fromLatin1(option.name));
		if (*option.short_name) names.prepend(QString::fromLatin1(option.short_name));

		QCommandLineOption declaration(names, QString::fromLatin1(option.description),
		                                QString::fromLatin1(option.value_name));
		if (!*option.value_name) declaration.setValueName(QString());

		parser.addOption(declaration);
		declared.append(declaration);
	}

	/*
	 * parse(), never process(): process() prints and calls exit(), which
	 * would make every one of these decisions untestable and would take
	 * the process down from inside a library call.
	 */
	if (!parser.parse(QStringList(arguments.value(0)) + head)) {
		line.error = parser.errorText();
		return line;
	}

	line.man = parser.isSet(QStringLiteral("man"));
	line.completion = parser.isSet(QStringLiteral("completion"));
	line.help = parser.isSet(QStringLiteral("help"));
	line.version = parser.isSet(QStringLiteral("version"));
	line.self_test = parser.isSet(QStringLiteral("self-test"));
	line.hold = parser.isSet(QStringLiteral("hold"));

	if (parser.isSet(QStringLiteral("term-features"))) {
		const QString spec = parser.value(QStringLiteral("term-features"));

		/*
		 * `?` lists them, which is the only discoverable spelling for a
		 * value nobody can guess. The help text cannot carry twelve
		 * names and their descriptions without becoming the whole help
		 * text.
		 */
		if (spec == QStringLiteral("?") || spec == QStringLiteral("list")) {
			QStringList lines;
			for (int i = 0; i < bssh_term_features_t::FEATURE_COUNT; ++i) {
				const bssh_term_features_t::feature_t feature =
				        bssh_term_features_t::feature_t(i);

				const QString name =
				        QString::fromLatin1(bssh_term_features_t::name(feature));
				const QString what = QString::fromLatin1(
				        bssh_term_features_t::description(feature));

				lines.append(QStringLiteral("  %1\n      %2").arg(name, what));
			}
			line.listing = QStringLiteral("terminal features:\n")
			        + lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
			return line;
		}

		/*
		 * Validated HERE rather than where it is applied, so a
		 * misspelling is a refused command line -- exit 2 with the
		 * reason -- instead of a session quietly running with the
		 * feature still on. A harness that got the name wrong would
		 * otherwise report a pass for a configuration nobody tested.
		 */
		QString error;
		bssh_term_features_t::from_spec(spec, error);
		if (!error.isEmpty()) {
			line.error = error;
			return line;
		}

		line.term_features = spec;
	}
	line.title = parser.value(QStringLiteral("title"));
	line.working_directory = parser.value(QStringLiteral("workdir"));
	line.saved_session = parser.value(QStringLiteral("load"));

	if (line.command.isEmpty() && parser.isSet(QStringLiteral("execute"))) {
		line.command = QStringList(parser.value(QStringLiteral("execute")));
	}

	if (parser.isSet(QStringLiteral("geometry"))) {
		const QString spec = parser.value(QStringLiteral("geometry"));
		/*
		 * COLSxROWS, optionally followed by a position. The position's
		 * signs are part of the numbers -- xterm's -0-0 is the bottom
		 * right corner, and dropping the sign would put the window at
		 * the top left instead of refusing.
		 */
		static const QRegularExpression pattern(
		        QStringLiteral("^(\\d+)x(\\d+)(?:([+-]\\d+)([+-]\\d+))?$"));
		const QRegularExpressionMatch match = pattern.match(spec);

		if (!match.hasMatch()) {
			line.error = QStringLiteral("--geometry wants COLSxROWS[+X+Y], not \"%1\"")
			                     .arg(spec);
			return line;
		}

		line.cols = match.captured(1).toInt();
		line.rows = match.captured(2).toInt();

		if (line.cols < 1 || line.rows < 1
		    || line.cols > BSSH_MAX_CELLS || line.rows > BSSH_MAX_CELLS) {
			line.error = QStringLiteral("--geometry: %1x%2 is not a terminal "
			                             "anyone could use (1 to %3 cells each way)")
			                     .arg(line.cols)
			                     .arg(line.rows)
			                     .arg(BSSH_MAX_CELLS);
			return line;
		}

		if (!match.captured(3).isEmpty()) {
			line.has_position = true;
			line.x = match.captured(3).toInt();
			line.y = match.captured(4).toInt();
		}
	}

	/*
	 * The positionals: a destination, a preset file, or one of each.
	 *
	 * A path is told from a hostname by its .ini suffix, which is what
	 * platform_desktop's own scan did -- but that one also required the
	 * file to EXIST, so a mistyped path fell through to being treated as
	 * a hostname and the app tried to resolve "preset.ini". Deciding on
	 * the suffix alone keeps this function free of the filesystem and
	 * gets the honest error from whoever goes to read the file.
	 */
	for (const QString &argument : parser.positionalArguments()) {
		const bool is_document =
		        argument.endsWith(QStringLiteral(".ini"), Qt::CaseInsensitive)
		        || argument.startsWith(QStringLiteral("file://"));

		if (is_document) {
			if (!line.document.isEmpty()) {
				line.error = QStringLiteral("only one preset file can be imported "
				                             "at a time");
				return line;
			}
			line.document = bssh_normalise_document_path(argument);
			continue;
		}

		if (!line.target.isEmpty()) {
			line.error = QStringLiteral("only one destination at a time (\"%1\" "
			                             "and \"%2\")")
			                     .arg(line.target, argument);
			return line;
		}
		line.target = argument;
	}

	if (!line.target.isEmpty() && !line.saved_session.isEmpty()) {
		line.error = QStringLiteral("--load names a saved session and \"%1\" is a "
		                             "destination; give one or the other")
		                     .arg(line.target);
		return line;
	}

	/*
	 * A working directory is the local shell's, and SSH has no way to
	 * ask for one -- the remote's shell starts wherever the account
	 * starts. Refused rather than ignored: a --workdir that silently did
	 * nothing over SSH is worse than one that says it cannot.
	 */
	if (!line.working_directory.isEmpty()
	    && (!line.target.isEmpty() || !line.saved_session.isEmpty())) {
		line.error = QStringLiteral("--workdir applies to a local shell; SSH has no "
		                             "way to ask a server to start somewhere");
		return line;
	}

	return line;
}

namespace {

/* Set once by main(); see bssh_command_line(). */
bssh_command_line_t process_line;
bool process_line_set = false;

} /* namespace */

const bssh_command_line_t &bssh_command_line() {
	if (!process_line_set) {
		process_line = bssh_parse_command_line(QCoreApplication::arguments());
		process_line_set = true;
	}
	return process_line;
}

void bssh_set_command_line(const bssh_command_line_t &line) {
	process_line = line;
	process_line_set = true;
}
