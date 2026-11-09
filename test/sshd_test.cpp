#include "sshd_test.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sshd/root_path.h"
#include "sshd/sshd.h"

namespace {

bool write_file(const QString &path, const QByteArray &bytes) {
	QFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray read_file(const QString &path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool have_openssh() {
	return !QStandardPaths::findExecutable(QStringLiteral("sftp")).isEmpty()
	       && !QStandardPaths::findExecutable(QStringLiteral("ssh-keygen")).isEmpty();
}

/*
 * The descriptor in this process listening on `port`, or -1. Found by
 * looking rather than by asking the server, which has no reason to hand
 * its socket out.
 */
int listener_on(int port) {
	/*
	 * Every descriptor number rather than a directory listing: /proc is
	 * Linux's, and macOS's /dev/fd lists nothing QDir will return.
	 */
	const int limit = int(::sysconf(_SC_OPEN_MAX));
	for (int fd = 0; fd < limit && fd < 65536; ++fd) {
		int listening = 0;
		socklen_t size = sizeof listening;
		if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) != 0 || !listening) continue;
		struct sockaddr_in address;
		socklen_t length = sizeof address;
		if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) continue;
		if (address.sin_family == AF_INET && ntohs(address.sin_port) == port) return fd;
	}
	return -1;
}

/* What the sockets of this process are, for a failure to show. */
QString describe_sockets() {
	QString text;
	const int limit = int(::sysconf(_SC_OPEN_MAX));
	for (int fd = 0; fd < limit && fd < 65536; ++fd) {
		struct sockaddr_storage address;
		socklen_t length = sizeof address;
		if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) continue;
		int listening = -1;
		socklen_t size = sizeof listening;
		const int asked = ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size);
		int port = -1;
		if (address.ss_family == AF_INET) port = ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
		if (address.ss_family == AF_INET6) port = ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
		text += QStringLiteral(" fd %1 family %2 port %3 acceptconn %4/%5;")
		                .arg(fd).arg(address.ss_family).arg(port).arg(asked).arg(listening);
	}
	return text;
}

/* Process CPU time, user and system, in seconds. */
double cpu_seconds() {
	return double(std::clock()) / CLOCKS_PER_SEC;
}

} /* namespace */

void sshd_test::init() {
	m_dir = std::make_unique<QTemporaryDir>();
	QVERIFY(m_dir->isValid());

	m_root = m_dir->filePath(QStringLiteral("root"));
	m_outside = m_dir->filePath(QStringLiteral("outside"));
	m_config = m_dir->filePath(QStringLiteral("config"));
	QVERIFY(QDir().mkpath(m_root + QStringLiteral("/sub")));
	QVERIFY(QDir().mkpath(m_outside));
	QVERIFY(QDir().mkpath(m_config));

	QVERIFY(write_file(m_root + QStringLiteral("/hello.txt"), "hello\n"));
	QVERIFY(write_file(m_outside + QStringLiteral("/secret.txt"), "secret\n"));

	/*
	 * The case that matters more than "..": a link inside the root to a
	 * directory outside it. On a phone, outside is the app's own private
	 * storage, where the host key is.
	 */
	QVERIFY(QFile::link(m_outside, m_root + QStringLiteral("/escape")));
}

void sshd_test::cleanup() {
	if (m_server) m_server->stop();
	m_server.reset();
	m_dir.reset();
}

void sshd_test::a_client_path_never_climbs_above_the_root() {
	QCOMPARE(bssh_root_path::normalise(""), QByteArray("/"));
	QCOMPARE(bssh_root_path::normalise("."), QByteArray("/"));
	QCOMPARE(bssh_root_path::normalise("a/b"), QByteArray("/a/b"));
	QCOMPARE(bssh_root_path::normalise("/a//b/./c/"), QByteArray("/a/b/c"));
	QCOMPARE(bssh_root_path::normalise("/a/../b"), QByteArray("/b"));
	QCOMPARE(bssh_root_path::normalise("../../etc/passwd"), QByteArray("/etc/passwd"));
	QCOMPARE(bssh_root_path::normalise("/.."), QByteArray("/"));

	/* And the clamped path lands inside the root, not at the system's /etc. */
	const bssh_root_path root(QFile::encodeName(m_root));
	QVERIFY(root.valid());
	QCOMPARE(root.existing("../../hello.txt").real, QFile::encodeName(QDir(m_root).canonicalPath())
	                                                  + "/hello.txt");
	QCOMPARE(root.existing("../../etc/passwd").error, ENOENT);
}

void sshd_test::a_symlink_out_of_the_root_is_not_followed() {
	const bssh_root_path root(QFile::encodeName(m_root));

	QVERIFY(root.existing("/escape/secret.txt").real.isEmpty());
	QCOMPARE(root.existing("/escape/secret.txt").error, EACCES);
	QCOMPARE(root.existing("/escape").error, EACCES);

	/* Creating THROUGH it is refused, and so is creating at a dangling link. */
	QCOMPARE(root.leaf("/escape/new.txt").error, EACCES);
	QVERIFY(QFile::link(m_outside + QStringLiteral("/not-yet"), m_root + QStringLiteral("/dangling")));
	QCOMPARE(root.leaf("/dangling").error, EACCES);

	/* A link that stays inside is fine; the root is not against links, only escapes. */
	QVERIFY(QFile::link(m_root + QStringLiteral("/sub"), m_root + QStringLiteral("/inside")));
	QVERIFY(!root.existing("/inside").real.isEmpty());
	QCOMPARE(root.client_view(root.existing("/inside").real), QByteArray("/sub"));
}

void sshd_test::the_root_itself_is_never_removed_or_replaced() {
	const bssh_root_path root(QFile::encodeName(m_root));
	QCOMPARE(root.leaf("/").error, EACCES);
	QCOMPARE(root.leaf("/..").error, EACCES);
	QCOMPARE(root.client_view(root.existing("/").real), QByteArray("/"));
}

bool sshd_test::start_server(long exchange_seconds) {
	if (!have_openssh()) return false;

	const QString key = m_config + QStringLiteral("/client");
	if (client(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"),
	           QStringLiteral("ed25519"), QStringLiteral("-N"), QString(), QStringLiteral("-f"), key}) != 0)
		return false;
	if (!QFile::copy(key + QStringLiteral(".pub"), m_config + QStringLiteral("/authorized_keys")))
		return false;

	bssh_sshd_config_t config;
	config.root = QFile::encodeName(m_root);
	config.host_key_path = m_config + QStringLiteral("/host_key");
	config.authorized_keys_path = m_config + QStringLiteral("/authorized_keys");
	config.password_path = m_config + QStringLiteral("/password");
	config.bind_address = "127.0.0.1";
	config.port = 0;
	config.exchange_seconds = exchange_seconds;

	m_server = std::make_unique<bssh_sshd>();
	QString error;
	if (!m_server->start(config, &error)) {
		qWarning("%s", qPrintable(error));
		return false;
	}
	return m_server->port() > 0;
}

QStringList sshd_test::client_options(const QString &key) const {
	return {QStringLiteral("-i"), key,
	        QStringLiteral("-o"), QStringLiteral("IdentitiesOnly=yes"),
	        QStringLiteral("-o"), QStringLiteral("IdentityAgent=none"),
	        QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=no"),
	        QStringLiteral("-o"), QStringLiteral("UserKnownHostsFile=/dev/null"),
	        QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
	        QStringLiteral("-o"), QStringLiteral("LogLevel=ERROR"),
	        QStringLiteral("-F"), QStringLiteral("/dev/null")};
}

/*
 * One client run, bounded: thirty seconds and then killed, so a server
 * that stops answering fails a test instead of holding the suite.
 */
int sshd_test::client(const QString &program, const QStringList &args, const QByteArray &input,
                      QByteArray *output, const QStringList &environment) {
	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
	if (!environment.isEmpty()) {
		QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
		for (const QString &pair : environment) {
			const int eq = pair.indexOf(QLatin1Char('='));
			env.insert(pair.left(eq), pair.mid(eq + 1));
		}
		process.setProcessEnvironment(env);
	}
	process.start(program, args);
	if (!process.waitForStarted(5000)) return -1;
	if (!input.isEmpty()) process.write(input);
	process.closeWriteChannel();

	if (!process.waitForFinished(30000)) {
		process.kill();
		process.waitForFinished(5000);
		return -2;
	}
	if (output) *output = process.readAll();
	return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -3;
}

void sshd_test::files_go_up_and_down_inside_the_root() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	const QString local = m_dir->filePath(QStringLiteral("local.bin"));
	QByteArray payload;
	for (int i = 0; i < 300000; ++i) payload.append(char(i * 7 + i / 255));
	QVERIFY(write_file(local, payload));
	const QString back = m_dir->filePath(QStringLiteral("back.bin"));

	/* Big enough to take many READ and WRITE requests, and the READ limit's short replies. */
	const QByteArray batch = "put " + QFile::encodeName(local) + " up.bin\n"
	                         "rename up.bin moved.bin\n"
	                         "mkdir made\n"
	                         "get moved.bin " + QFile::encodeName(back) + "\n"
	                         "rm hello.txt\n";

	QStringList args = client_options(m_config + QStringLiteral("/client"));
	args << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
	     << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");
	QByteArray output;
	QCOMPARE(client(QStringLiteral("sftp"), args, batch, &output), 0);

	QCOMPARE(read_file(m_root + QStringLiteral("/moved.bin")), payload);
	QCOMPARE(read_file(back), payload);
	QVERIFY(!QFile::exists(m_root + QStringLiteral("/up.bin")));
	QVERIFY(QFileInfo(m_root + QStringLiteral("/made")).isDir());
	QVERIFY(!QFile::exists(m_root + QStringLiteral("/hello.txt")));
}

void sshd_test::nothing_outside_the_root_is_reachable_over_the_wire() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	const QString got = m_dir->filePath(QStringLiteral("got.txt"));

	/*
	 * Each on its own run, because sftp -b stops at the first failure and
	 * every one of these is meant to fail. Reads, writes and removes, by
	 * climbing and by the link.
	 */
	const QList<QByteArray> attempts = {
		"get ../outside/secret.txt " + QFile::encodeName(got),
		"get escape/secret.txt " + QFile::encodeName(got),
		"put " + QFile::encodeName(m_root + QStringLiteral("/hello.txt")) + " escape/planted.txt",
		"rm escape/secret.txt",
		"rename hello.txt escape/moved.txt",
		"symlink /escape/secret.txt link",
	};

	for (const QByteArray &attempt : attempts) {
		QStringList args = client_options(m_config + QStringLiteral("/client"));
		args << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
		     << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");
		QByteArray output;
		QVERIFY2(client(QStringLiteral("sftp"), args, attempt + '\n', &output) != 0, attempt.constData());
	}

	QVERIFY(!QFile::exists(got));
	QCOMPARE(read_file(m_outside + QStringLiteral("/secret.txt")), QByteArray("secret\n"));
	QVERIFY(!QFile::exists(m_outside + QStringLiteral("/planted.txt")));
	QVERIFY(!QFile::exists(m_outside + QStringLiteral("/moved.txt")));
	QVERIFY(QFile::exists(m_root + QStringLiteral("/hello.txt")));
	QVERIFY(!QFileInfo::exists(m_root + QStringLiteral("/link")));
}

void sshd_test::only_a_listed_key_logs_in() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	const QString other = m_config + QStringLiteral("/other");
	QCOMPARE(client(QStringLiteral("ssh-keygen"), {QStringLiteral("-q"), QStringLiteral("-t"),
	         QStringLiteral("ed25519"), QStringLiteral("-N"), QString(), QStringLiteral("-f"), other}), 0);

	QStringList args = client_options(other);
	args << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
	     << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");
	QByteArray output;
	QVERIFY(client(QStringLiteral("sftp"), args, "ls\n", &output) != 0);
	QVERIFY2(output.contains("Permission denied"), output.constData());

	/* Re-read at the next login: listing it lets it in without a restart. */
	QFile keys(m_config + QStringLiteral("/authorized_keys"));
	QVERIFY(keys.open(QIODevice::Append));
	keys.write(read_file(other + QStringLiteral(".pub")));
	keys.close();
	QCOMPARE(client(QStringLiteral("sftp"), args, "ls\n", &output), 0);
}

void sshd_test::a_password_is_stored_as_a_salted_hash() {
	const QString path = m_config + QStringLiteral("/password");
	QVERIFY(!bssh_sshd::has_password(path));

	QString error;
	QVERIFY2(bssh_sshd::set_password(path, QStringLiteral("correct horse"), &error), qPrintable(error));
	QVERIFY(bssh_sshd::has_password(path));

	/* The hash and never the password, readable by the owner alone. */
	const QByteArray stored = read_file(path);
	QVERIFY(!stored.contains("correct horse"));
	QVERIFY2(stored.startsWith("pbkdf2-sha256:"), stored.constData());
	QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther
	                                      | QFileDevice::WriteGroup | QFileDevice::WriteOther),
	         QFileDevice::Permissions());

	QVERIFY(bssh_sshd::password_matches(path, "correct horse"));
	QVERIFY(!bssh_sshd::password_matches(path, "correct hors"));
	QVERIFY(!bssh_sshd::password_matches(path, ""));

	/* Salted: the same password twice is two different lines. */
	QVERIFY(bssh_sshd::set_password(path, QStringLiteral("correct horse"), &error));
	QVERIFY(read_file(path) != stored);
	QVERIFY(bssh_sshd::password_matches(path, "correct horse"));

	/* And an empty one takes it away: keys only again. */
	QVERIFY(bssh_sshd::set_password(path, QString(), &error));
	QVERIFY(!bssh_sshd::has_password(path));
	QVERIFY(!QFile::exists(path));
}

void sshd_test::a_password_logs_in_once_it_is_set() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	/*
	 * OpenSSH asks for a password through SSH_ASKPASS when told to, which is
	 * how a test can type one. No key is offered at all.
	 */
	const QString askpass = m_config + QStringLiteral("/askpass");
	const QString typed = m_config + QStringLiteral("/typed");
	QVERIFY(write_file(askpass, "#!/bin/sh\ncat \"$(dirname \"$0\")/typed\"\n"));
	QVERIFY(QFile::setPermissions(askpass, QFileDevice::ReadOwner | QFileDevice::WriteOwner
	                                       | QFileDevice::ExeOwner));
	const QStringList environment = {QStringLiteral("SSH_ASKPASS=") + askpass,
	                                  QStringLiteral("SSH_ASKPASS_REQUIRE=force"),
	                                  QStringLiteral("DISPLAY=:0")};
	const QStringList args = {
		QStringLiteral("-o"), QStringLiteral("PubkeyAuthentication=no"),
		QStringLiteral("-o"), QStringLiteral("PreferredAuthentications=password"),
		QStringLiteral("-o"), QStringLiteral("NumberOfPasswordPrompts=1"),
		QStringLiteral("-o"), QStringLiteral("IdentityAgent=none"),
		QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=no"),
		QStringLiteral("-o"), QStringLiteral("UserKnownHostsFile=/dev/null"),
		QStringLiteral("-o"), QStringLiteral("LogLevel=ERROR"),
		QStringLiteral("-F"), QStringLiteral("/dev/null"),
		QStringLiteral("-P"), QString::number(m_server->port()), QStringLiteral("anyone@127.0.0.1")};
	/*
	 * Commands from standard input rather than `-b -`: sftp's batch mode
	 * turns on ssh's BatchMode, which refuses every password prompt --
	 * askpass included -- so with -b no password was ever sent, and the
	 * two refusals below passed without testing anything.
	 */

	/* None set: the server does not even offer passwords. */
	QVERIFY(write_file(typed, "correct horse\n"));
	QByteArray output;
	QVERIFY(client(QStringLiteral("sftp"), args, "ls\n", &output, environment) != 0);

	/* Set, and read at the next login without a restart. */
	QString error;
	QVERIFY(bssh_sshd::set_password(m_config + QStringLiteral("/password"),
	                                QStringLiteral("correct horse"), &error));

	QVERIFY(write_file(typed, "wrong horse\n"));
	QVERIFY(client(QStringLiteral("sftp"), args, "ls\n", &output, environment) != 0);
	QVERIFY2(output.contains("Permission denied"), output.constData());

	QVERIFY(write_file(typed, "correct horse\n"));
	const int rc = client(QStringLiteral("sftp"), args, "ls\n", &output, environment);
	QVERIFY2(rc == 0, output.constData());
	QVERIFY2(output.contains("hello.txt"), output.constData());
}

/*
 * Twelve guesses a second, for as long as the server is up, is what a
 * connection's own limits left (sec 19). Six are free, then each wrong one
 * doubles the wait to an hour; an address is its own, and a success or a
 * quiet day forgives.
 */
void sshd_test::wrong_passwords_wait_longer_and_longer() {
	using std::chrono::hours;
	using std::chrono::seconds;
	bssh_login_throttle throttle;
	const QString guesser = QStringLiteral("192.0.2.1");
	auto now = bssh_login_throttle::clock::now();

	for (int i = 0; i < bssh_login_throttle::free_failures; ++i) {
		QVERIFY(throttle.allowed(guesser, now));
		throttle.failed(guesser, now);
	}
	QVERIFY2(throttle.allowed(guesser, now), "a person mistyping was made to wait");

	throttle.failed(guesser, now);
	QVERIFY(!throttle.allowed(guesser, now));
	QVERIFY(!throttle.allowed(guesser, now + seconds(1)));
	QVERIFY(throttle.allowed(guesser, now + seconds(2)));
	QVERIFY2(throttle.allowed(QStringLiteral("192.0.2.2"), now), "one address made another wait");

	for (int i = 0; i < 40; ++i) throttle.failed(guesser, now);
	QVERIFY(!throttle.allowed(guesser, now + hours(1) - seconds(1)));
	QVERIFY2(throttle.allowed(guesser, now + hours(1)), "the wait went past its hour");

	throttle.succeeded(guesser);
	QVERIFY(throttle.allowed(guesser, now));

	/* The table holds a bounded number of addresses, however many knock. */
	for (int i = 0; i < bssh_login_throttle::max_addresses + 50; ++i) {
		throttle.failed(QString::number(i), now);
	}
	QVERIFY(throttle.addresses() <= bssh_login_throttle::max_addresses);
}

/* Over the wire: the wait refuses the right password as well, and a key never waits. */
void sshd_test::guessing_across_connections_stops_a_right_password_too() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	const QString askpass = m_config + QStringLiteral("/askpass");
	QVERIFY(write_file(askpass, "#!/bin/sh\necho 'correct horse'\n"));
	QVERIFY(QFile::setPermissions(askpass, QFileDevice::ReadOwner | QFileDevice::WriteOwner
	                                       | QFileDevice::ExeOwner));
	const QStringList environment = {QStringLiteral("SSH_ASKPASS=") + askpass,
	                                  QStringLiteral("SSH_ASKPASS_REQUIRE=force"),
	                                  QStringLiteral("DISPLAY=:0")};
	const QStringList args = {
		QStringLiteral("-o"), QStringLiteral("PubkeyAuthentication=no"),
		QStringLiteral("-o"), QStringLiteral("PreferredAuthentications=password"),
		QStringLiteral("-o"), QStringLiteral("NumberOfPasswordPrompts=1"),
		QStringLiteral("-o"), QStringLiteral("IdentityAgent=none"),
		QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=no"),
		QStringLiteral("-o"), QStringLiteral("UserKnownHostsFile=/dev/null"),
		QStringLiteral("-o"), QStringLiteral("LogLevel=ERROR"),
		QStringLiteral("-F"), QStringLiteral("/dev/null"),
		QStringLiteral("-P"), QString::number(m_server->port()), QStringLiteral("anyone@127.0.0.1")};

	QString error;
	QVERIFY(bssh_sshd::set_password(m_config + QStringLiteral("/password"),
	                                QStringLiteral("correct horse"), &error));

	/* As seven earlier connections would have left it, without their seven seconds. */
	const QString address = QStringLiteral("127.0.0.1");
	const auto now = bssh_login_throttle::clock::now() + std::chrono::minutes(1);
	for (int i = 0; i <= bssh_login_throttle::free_failures; ++i) m_server->throttle().failed(address, now);

	QByteArray output;
	QVERIFY2(client(QStringLiteral("sftp"), args, "ls\n", &output, environment) != 0,
	         "the right password got through a wait");
	QVERIFY2(output.contains("Permission denied"), output.constData());

	/* The key beside it logs in as if nothing had happened. */
	QStringList keyed = client_options(m_config + QStringLiteral("/client"));
	keyed << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
	      << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");
	const int rc = client(QStringLiteral("sftp"), keyed, "ls\n", &output);
	QVERIFY2(rc == 0, output.constData());

	m_server->throttle().succeeded(address);
	QVERIFY2(client(QStringLiteral("sftp"), args, "ls\n", &output, environment) == 0, output.constData());
}

/*
 * A pipe in the root waited in open() for a writer, on a thread that
 * neither the client hanging up nor Stop could reach, and Stop joins it.
 * So the proof that nothing is held is every connection ending.
 */
void sshd_test::a_named_pipe_is_refused_and_holds_nothing_up() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	QVERIFY(::mkfifo(QFile::encodeName(m_root + QStringLiteral("/pipe")).constData(), 0600) == 0);

	QStringList args = client_options(m_config + QStringLiteral("/client"));
	args << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
	     << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");

	/*
	 * put and not get: OpenSSH's client asks first and will not download
	 * what is not a file, so the open never came and this passed with the
	 * guard taken out. An upload opens without asking.
	 */
	const QByteArray upload = "put " + QFile::encodeName(m_root + QStringLiteral("/hello.txt")) + " pipe\n";
	QByteArray output;
	QVERIFY(client(QStringLiteral("sftp"), args, upload, &output) != 0);
	QVERIFY(client(QStringLiteral("sftp"), args, "ls\n", &output) == 0);

	/*
	 * Every connection over once its client has gone. A thread in open()
	 * shows as one that never ends; a reader lets it go, whichever way
	 * this came out, or cleanup() would hang in the same join Stop does
	 * instead of this failing.
	 */
	QElapsedTimer waited;
	waited.start();
	while (m_server->connections() != 0 && waited.elapsed() < 5000) QTest::qWait(50);
	const int connections = m_server->connections();

	const int reader = ::open(QFile::encodeName(m_root + QStringLiteral("/pipe")).constData(),
	                          O_RDONLY | O_NONBLOCK);
	QTest::qWait(200);
	if (reader >= 0) ::close(reader);

	QCOMPARE(connections, 0);
}

/*
 * A connection that says nothing waited in the key exchange for ever,
 * before any deadline applied, and four of them were every slot there is:
 * a server meant to be reachable all the time, shut out by four idle
 * sockets from anywhere on the network until somebody pressed Stop.
 */
void sshd_test::silent_connections_do_not_hold_the_server() {
	if (!start_server(1)) QSKIP("the OpenSSH client is not installed");

	QList<QTcpSocket *> silent;
	for (int i = 0; i < 4; ++i) {
		auto *socket = new QTcpSocket(this);
		socket->connectToHost(QHostAddress::LocalHost, quint16(m_server->port()));
		QVERIFY(socket->waitForConnected(2000));
		silent.append(socket);
	}
	QTest::qWait(300);

	/* One address cannot take the last slot. */
	QVERIFY2(m_server->connections() < 4, "one address took every slot");

	/* Past the exchange's limit, the slots are free again. */
	QElapsedTimer waited;
	waited.start();
	while (m_server->connections() != 0 && waited.elapsed() < 4000) QTest::qWait(50);
	const int held = m_server->connections();
	qDeleteAll(silent);
	QCOMPARE(held, 0);
}

void sshd_test::a_key_with_options_is_not_honoured_without_them() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");

	/* The same key, restricted. Nothing here enforces from=, so the line is not used at all. */
	const QByteArray key = read_file(m_config + QStringLiteral("/client.pub"));
	QVERIFY(write_file(m_config + QStringLiteral("/authorized_keys"), "from=\"10.9.9.9\" " + key));

	QStringList args = client_options(m_config + QStringLiteral("/client"));
	args << QStringLiteral("-b") << QStringLiteral("-") << QStringLiteral("-P")
	     << QString::number(m_server->port()) << QStringLiteral("127.0.0.1");
	QVERIFY(client(QStringLiteral("sftp"), args, "ls\n") != 0);
}

void sshd_test::sftp_is_all_it_serves() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");
	if (QStandardPaths::findExecutable(QStringLiteral("ssh")).isEmpty()) QSKIP("no ssh client");

	QStringList args = client_options(m_config + QStringLiteral("/client"));
	args << QStringLiteral("-p") << QString::number(m_server->port()) << QStringLiteral("127.0.0.1")
	     << QStringLiteral("touch ran-a-command");
	QByteArray output;
	QVERIFY(client(QStringLiteral("ssh"), args, QByteArray(), &output) != 0);
	QVERIFY2(output.contains("request failed"), output.constData());
	QVERIFY(!QFile::exists(m_root + QStringLiteral("/ran-a-command")));
}

void sshd_test::scp_finishes_with_a_clean_exit() {
	if (!start_server()) QSKIP("the OpenSSH client is not installed");
	if (QStandardPaths::findExecutable(QStringLiteral("scp")).isEmpty()) QSKIP("no scp");

	/*
	 * The exit status is the assertion. A server that disconnected as the
	 * channel ended, before the client could, delivered the file and still
	 * made scp exit 1 -- a failure for a transfer that had succeeded.
	 */
	QStringList args = client_options(m_config + QStringLiteral("/client"));
	args << QStringLiteral("-P") << QString::number(m_server->port())
	     << m_root + QStringLiteral("/hello.txt") << QStringLiteral("127.0.0.1:copied.txt");
	QByteArray output;
	QCOMPARE(client(QStringLiteral("scp"), args, QByteArray(), &output), 0);
	QCOMPARE(read_file(m_root + QStringLiteral("/copied.txt")), QByteArray("hello\n"));

	/*
	 * And the client hung up, not the server: dropping the connection the
	 * moment the channel closed made every client report "closed by
	 * remote host" for a session that had ended normally. INFO, because
	 * that is the level ssh reports it at and the options above silence it.
	 */
	args.replace(args.indexOf(QStringLiteral("LogLevel=ERROR")), QStringLiteral("LogLevel=INFO"));
	QCOMPARE(client(QStringLiteral("scp"), args, QByteArray(), &output), 0);
	QVERIFY2(!output.contains("closed by remote host"), output.constData());
}

/*
 * Android destroys a socket bound to an address when the network that
 * owned the address goes (project.md sec 19.6). Seen on an API 36
 * emulator before this was handled: the accepting thread spun at ~90% of
 * a core on the dead socket while Wi-Fi was off, and nothing listened
 * once it was back. Here the socket is shut down from outside, which
 * Linux reports to poll() the same way, and the port is then held so
 * that listening again fails for a while -- the window in which a loop
 * that kept the dead socket would spin.
 */
void sshd_test::a_listener_the_system_takes_away_comes_back_without_spinning() {
	bssh_sshd_config_t config;
	config.root = QFile::encodeName(m_root);
	config.host_key_path = m_config + QStringLiteral("/host_key");
	config.authorized_keys_path = m_config + QStringLiteral("/authorized_keys");
	config.bind_address = "127.0.0.1";
	config.port = 0;

	m_server = std::make_unique<bssh_sshd>();
	QString error;
	QVERIFY2(m_server->start(config, &error), qPrintable(error));
	const int port = m_server->port();
	QVERIFY(m_server->listening());

	const int fd = listener_on(port);
	QVERIFY2(fd >= 0, qPrintable(QStringLiteral("nothing listening on %1 among:%2").arg(port).arg(describe_sockets())));
	QCOMPARE(::shutdown(fd, SHUT_RDWR), 0);
	QTRY_VERIFY(!m_server->listening());
	QVERIFY(m_server->running());

	/* The port taken while it is free, so listening again fails. */
	QTcpServer blocker;
	QVERIFY(blocker.listen(QHostAddress(QStringLiteral("127.0.0.1")), quint16(port)));

	const double before = cpu_seconds();
	QTest::qWait(3000);
	const double spent = cpu_seconds() - before;
	QVERIFY2(spent < 0.3, qPrintable(QStringLiteral("%1 s of CPU in 3 s without a listener").arg(spent)));
	QVERIFY(!m_server->listening());

	blocker.close();
	QTRY_VERIFY_WITH_TIMEOUT(m_server->listening(), 5000);

	QTcpSocket probe;
	probe.connectToHost(QHostAddress(QStringLiteral("127.0.0.1")), quint16(port));
	QVERIFY(probe.waitForConnected(3000));
	QTRY_VERIFY(probe.bytesAvailable() >= 8 || probe.waitForReadyRead(100));
	QVERIFY(probe.readAll().startsWith("SSH-2.0-"));
}
