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

void usage(QTextStream &out) {
	out << "usage: beersshd --root DIR [--port N] [--bind ADDRESS]\n"
	       "                [--host-key FILE] [--authorized-keys FILE]\n"
	       "                [--password-file FILE]\n"
	       "       beersshd --set-password [--password-file FILE]\n"
	       "\n"
	       "Serves DIR over SFTP, and nothing else, to the keys in the\n"
	       "authorized keys file and to the password, if one is set.\n"
	       "Modern scp works; scp -O does not.\n"
	       "\n"
	       "  --root DIR             the folder served as /; required\n"
	       "  --port N               default 8022; 0 picks a free one\n"
	       "  --bind ADDRESS         default 0.0.0.0\n"
	       "  --host-key FILE        created if missing\n"
	       "  --authorized-keys FILE OpenSSH format, re-read at each login\n"
	       "  --password-file FILE   the password's hash, re-read at each login\n"
	       "  --set-password         read a password from standard input and\n"
	       "                         store its hash; an empty line removes it\n"
	       "  --version\n";
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
