/*
 * beersshd on the desktop: the same server the Android app runs, from a
 * command line (project.md sec 19). It exists so the server can be built,
 * tested and debugged in the loop every other feature is, before any of
 * it reaches a phone.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>

#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include "sshd/sshd.h"

namespace {

int g_signal_pipe[2] = {-1, -1};

void on_signal(int) {
	const char byte = 0;
	(void)!::write(g_signal_pipe[1], &byte, 1);
}

/*
 * The options, once: --help and the manual page are both written from this
 * table, the way beerssh's are from its own (command_line.cpp), so the page
 * cannot document a flag the program does not take. The parsing below
 * matches on the same names.
 */
struct sshd_option_t {
	const char *name;
	const char *value;
	const char *help;
};

const sshd_option_t SSHD_OPTIONS[] = {
	{ "root", "DIR", "The folder served as /. Required: there is no default, because "
	                 "serving the current directory when the flag is forgotten is how "
	                 "a home directory ends up on the network." },
	{ "port", "N", "The port to listen on; 8022 by default, and 0 picks a free one." },
	{ "bind", "ADDRESS", "The address to listen on; 0.0.0.0 by default." },
	{ "host-key", "FILE", "The server's ed25519 host key, created if missing; by default "
	                      "host_ed25519_key in ~/.config/beersshd." },
	{ "authorized-keys", "FILE", "The keys allowed in, in OpenSSH's format, re-read at "
	                             "every login; by default authorized_keys in "
	                             "~/.config/beersshd." },
	{ "password-file", "FILE", "Where the password's hash is kept, re-read at every "
	                           "login; by default password in ~/.config/beersshd." },
	{ "set-password", nullptr, "Read a password from standard input, without echoing "
	                           "it at a terminal, and store its salted hash; an empty "
	                           "line removes the password. Serves nothing." },
	{ "version", nullptr, "Print the version and exit." },
	{ "help", nullptr, "Print a summary of the options and exit." },
	{ "man", nullptr, "Write this manual page to standard output and exit." },
};

void usage(QTextStream &out) {
	out << "usage: beersshd --root DIR [--port N] [--bind ADDRESS]\n"
	       "                [--host-key FILE] [--authorized-keys FILE]\n"
	       "                [--password-file FILE]\n"
	       "       beersshd --set-password [--password-file FILE]\n"
	       "\n"
	       "Serves DIR over SFTP, and nothing else, to the keys in the\n"
	       "authorized keys file and to the password, if one is set.\n"
	       "Modern scp works; scp -O does not.\n"
	       "\n";
	for (const sshd_option_t &option : SSHD_OPTIONS) {
		QString head = QStringLiteral("--") + QLatin1String(option.name);
		if (option.value) head += QLatin1Char(' ') + QLatin1String(option.value);
		/* The first sentence of the help, which is the summary. */
		QString summary = QString::fromLatin1(option.help);
		const int stop = summary.indexOf(QStringLiteral(". "));
		if (stop > 0) summary.truncate(stop);
		else if (summary.endsWith(QLatin1Char('.'))) summary.chop(1);
		out << "  " << head.leftJustified(23) << ' ' << summary << "\n";
	}
}

/* Text for roff: a backslash escaped, and a leading dot or quote kept from
 * being read as a request. */
QString roff(const QString &text) {
	QString escaped = text;
	escaped.replace(QLatin1Char('\\'), QStringLiteral("\\e"));
	if (escaped.startsWith(QLatin1Char('.')) || escaped.startsWith(QLatin1Char('\'')))
		escaped.prepend(QStringLiteral("\\&"));
	return escaped;
}

void man_page(QTextStream &out) {
	out << ".TH BEERSSHD 1 \"\" \"beersshd " << BSSH_VERSION_STRING << "\" \"User Commands\"\n"
	       ".SH NAME\n"
	       "beersshd \\- serve one folder over SFTP, and nothing else\n"
	       ".SH SYNOPSIS\n"
	       ".B beersshd\n"
	       ".B \\-\\-root\n"
	       ".I DIR\n"
	       ".RI [ options ]\n"
	       ".br\n"
	       ".B beersshd \\-\\-set\\-password\n"
	       ".RB [ \\-\\-password\\-file\n"
	       ".IR FILE ]\n"
	       ".SH DESCRIPTION\n"
	    << roff(QStringLiteral(
	               "beersshd serves one folder over SFTP to the keys in its authorized "
	               "keys file and, once one is set, to a password. It runs no shell and "
	               "no commands, forwards nothing, and keeps every path inside the "
	               "folder. Modern scp works, being SFTP underneath; scp -O, the old "
	               "protocol, does not.")) << "\n.PP\n"
	    << roff(QStringLiteral(
	               "The same server runs on Android as an app of its own. It writes a "
	               "line to standard output for each login and each refusal, and "
	               "SIGINT and SIGTERM stop it cleanly.")) << "\n"
	       ".SH OPTIONS\n";
	for (const sshd_option_t &option : SSHD_OPTIONS) {
		const QString name = QString::fromLatin1(option.name).replace(
		        QLatin1Char('-'), QStringLiteral("\\-"));
		if (option.value)
			out << ".TP\n.BI \"\\-\\-" << name << " \" " << option.value << "\n";
		else
			out << ".TP\n.B \\-\\-" << name << "\n";
		out << roff(QString::fromLatin1(option.help)) << "\n";
	}
	out << ".SH FILES\n"
	       ".TP\n.I ~/.config/beersshd/\n"
	    << roff(QStringLiteral("The host key, the authorized keys and the password's hash, "
	                           "unless the options above name others.")) << "\n"
	       ".SH SEE ALSO\n"
	       ".BR beerssh (1),\n"
	       ".BR sftp (1)\n";
}

} /* namespace */

int main(int argc, char *argv[]) {
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("beersshd"));

	QTextStream out(stdout);
	QTextStream err(stderr);

	const QString config_dir =
	        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
	        + QStringLiteral("/beersshd");

	bssh_sshd_config_t config;
	config.host_key_path = config_dir + QStringLiteral("/host_ed25519_key");
	config.authorized_keys_path = config_dir + QStringLiteral("/authorized_keys");
	config.password_path = config_dir + QStringLiteral("/password");
	bool set_password = false;

	const QStringList args = app.arguments().mid(1);
	for (int i = 0; i < args.size(); ++i) {
		const QString &arg = args.at(i);
		const bool has_value = i + 1 < args.size();

		if (arg == QStringLiteral("--version")) {
			out << "beersshd " << BSSH_VERSION_STRING << "\n"
			    << "Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n";
			return 0;
		} else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
			usage(out);
			return 0;
		} else if (arg == QStringLiteral("--man")) {
			man_page(out);
			return 0;
		} else if (arg == QStringLiteral("--root") && has_value) {
			config.root = QFile::encodeName(args.at(++i));
		} else if (arg == QStringLiteral("--port") && has_value) {
			bool ok = false;
			config.port = args.at(++i).toInt(&ok);
			if (!ok || config.port < 0 || config.port > 65535) {
				err << "beersshd: not a port: " << args.at(i) << "\n";
				return 2;
			}
		} else if (arg == QStringLiteral("--bind") && has_value) {
			config.bind_address = args.at(++i).toLatin1();
		} else if (arg == QStringLiteral("--host-key") && has_value) {
			config.host_key_path = args.at(++i);
		} else if (arg == QStringLiteral("--authorized-keys") && has_value) {
			config.authorized_keys_path = args.at(++i);
		} else if (arg == QStringLiteral("--password-file") && has_value) {
			config.password_path = args.at(++i);
		} else if (arg == QStringLiteral("--set-password")) {
			set_password = true;
		} else {
			usage(err);
			return 2;
		}
	}

	/*
	 * One line from standard input, hashed and stored; nothing echoed, and
	 * nothing served. A terminal is the person typing, so they are asked.
	 */
	if (set_password) {
		termios was {};
		const bool typed = ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &was) == 0;
		if (typed) {
			err << "password (empty to remove): " << Qt::flush;
			termios quiet = was;
			quiet.c_lflag &= ~tcflag_t(ECHO);
			::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
		}
		QTextStream in(stdin);
		const QString password = in.readLine();
		if (typed) {
			::tcsetattr(STDIN_FILENO, TCSAFLUSH, &was);
			err << "\n" << Qt::flush;
		}
		QDir().mkpath(QFileInfo(config.password_path).absolutePath());
		QString error;
		if (!bssh_sshd::set_password(config.password_path, password, &error)) {
			err << "beersshd: " << error << "\n";
			return 1;
		}
		out << (password.isEmpty() ? "password removed\n" : "password set\n");
		return 0;
	}

	/*
	 * No default root. Serving the current directory when the flag is
	 * forgotten is how a home directory ends up on the network.
	 */
	if (config.root.isEmpty()) {
		usage(err);
		return 2;
	}

	if (!QFile::exists(config.authorized_keys_path)
	    && !bssh_sshd::has_password(config.password_path)) {
		err << "beersshd: " << config.authorized_keys_path
		    << " does not exist and no password is set, so nobody can log in.\n";
	}

	bssh_sshd server;
	QObject::connect(&server, &bssh_sshd::logged, &app, [&out](const QString &line) {
		out << line << "\n";
		out.flush();
	});

	QString error;
	if (!server.start(config, &error)) {
		err << "beersshd: " << error << "\n";
		return 1;
	}

	out << "serving " << QFile::decodeName(config.root) << " on port " << server.port() << "\n"
	    << "host key " << server.host_key_fingerprint() << "\n";
	out.flush();

	/* SIGINT and SIGTERM stop the server cleanly, through the event loop. */
	if (::pipe2(g_signal_pipe, O_CLOEXEC) == 0) {
		auto *notifier = new QSocketNotifier(g_signal_pipe[0], QSocketNotifier::Read, &app);
		QObject::connect(notifier, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
		std::signal(SIGINT, on_signal);
		std::signal(SIGTERM, on_signal);
	}

	const int rc = app.exec();
	server.stop();
	return rc;
}
