#include "sshd_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>

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

bool sshd_test::start_server() {
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
	config.bind_address = "127.0.0.1";
	config.port = 0;

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
                      QByteArray *output) {
	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
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
