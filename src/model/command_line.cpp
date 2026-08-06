#include "model/command_line.h"

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
			"name is the one the host list shows." },
		{ "self-test", "", "",
			"Build the window, run one turn of the event loop and exit. "
			"What CI runs to check that a headless build comes up at "
			"all." },
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

	line.help = parser.isSet(QStringLiteral("help"));
	line.version = parser.isSet(QStringLiteral("version"));
	line.self_test = parser.isSet(QStringLiteral("self-test"));
	line.hold = parser.isSet(QStringLiteral("hold"));
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
