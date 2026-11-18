#include "fuzznet_session_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#ifdef BSSH_FUZZNET
#include "ssh/fuzznet_session.h"
#endif

namespace {

#ifdef BSSH_FUZZNET
const QStringList PAIR = { QStringLiteral("--fuzznet-service=1"), QStringLiteral("--fuzznet-product=1") };

/* One of beersshd's pairing steps: its last line of output, or empty. */
QString step(const QString &program, const QStringList &arguments) {
	QProcess process;
	process.start(program, QStringList{ QStringLiteral("--fuzznet") } + arguments + PAIR);
	if (!process.waitForFinished(20000) || process.exitCode() != 0) return QString();
	const QStringList lines =
	        QString::fromLatin1(process.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	return lines.isEmpty() ? QString() : lines.last().trimmed();
}

/*
 * A beersshd serving over fuzznet with --sessions, paired to one device
 * whose image is `image`; the helper beside it, as --sessions finds it.
 * Everything in a scratch directory of its own, the socket in a short one.
 */
struct server_t {
	QTemporaryDir dir;
	QTemporaryDir socket_dir{ QDir::tempPath() + QStringLiteral("/fzs") };
	QProcess process;
	QString image;
	quint16 port = 0;

	bool start(const QString &sshd, const QString &helper) {
		if (!dir.isValid() || !socket_dir.isValid()) return false;
		const QString bin = dir.filePath(QStringLiteral("beersshd"));
		if (!QFile::copy(sshd, bin) || !QFile::copy(helper, dir.filePath(QStringLiteral("beerssh-session"))))
			return false;
		QFile::setPermissions(dir.filePath(QStringLiteral("beerssh-session")),
		                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		const QString server = dir.filePath(QStringLiteral("server"));
		const QString client = dir.filePath(QStringLiteral("client"));
		QDir().mkpath(server);
		QDir().mkpath(client);
		if (step(bin, { QStringLiteral("--fuzznet-dir=") + server, QStringLiteral("--new-root") }).isEmpty())
			return false;
		const QString prekey = step(bin, { QStringLiteral("--fuzznet-dir=") + client, QStringLiteral("--prekey") });
		const QString card = step(bin, { QStringLiteral("--fuzznet-dir=") + server, QStringLiteral("--pair=") + prekey });
		const QString gateway = step(bin, { QStringLiteral("--fuzznet-dir=") + client, QStringLiteral("--accept=") + card });
		image = step(bin, { QStringLiteral("--fuzznet-dir=") + client, QStringLiteral("--device-image=") + gateway });
		if (!image.startsWith(QLatin1String("FZD1:"))) return false;

		port = quint16(29000 + QCoreApplication::applicationPid() % 900);
		const QString socket = socket_dir.filePath(QStringLiteral("s"));
		process.start(bin, QStringList{ QStringLiteral("--fuzznet"), QStringLiteral("--fuzznet-dir=") + server,
		                                QStringLiteral("--socket=") + socket,
		                                QStringLiteral("--udp-port=%1").arg(port), QStringLiteral("--no-log-file"),
		                                QStringLiteral("--sessions") } + PAIR);
		for (int i = 0; i < 50 && !QFile::exists(socket); ++i) QTest::qWait(200);
		return QFile::exists(socket);
	}

	~server_t() {
		process.terminate();
		if (!process.waitForFinished(5000)) process.kill();
		process.waitForFinished(2000);
	}
};

/* The two binaries the Makefile hands over, or why this cannot run. */
QString binaries(QString *sshd, QString *helper) {
	*sshd = qEnvironmentVariable("BSSH_TEST_SSHD_BIN");
	*helper = qEnvironmentVariable("BSSH_TEST_SESSION_BIN");
	if (!QFile::exists(*sshd) || !QFile::exists(*helper))
		return QStringLiteral("needs BSSH_TEST_SSHD_BIN and BSSH_TEST_SESSION_BIN, as make test gives");
	return QString();
}
#endif

} /* namespace */

void fuzznet_session_test::the_helper_answers_through_the_transport() {
#ifndef BSSH_FUZZNET
	QSKIP("fuzznet is not built here");
#else
	QString sshd, helper;
	const QString missing = binaries(&sshd, &helper);
	if (!missing.isEmpty()) QSKIP(qPrintable(missing));
	server_t server;
	QVERIFY2(server.start(sshd, helper), "beersshd --fuzznet did not pair and serve");

	bssh_fuzznet_session session(server.image);
	QByteArray heard;
	connect(&session, &bssh_ssh_session::bytes_received, &session,
	        [&heard](const QByteArray &bytes) { heard += bytes; });
	QSignalSpy failures(&session, &bssh_ssh_session::failed);
	session.connect_to_host(QStringLiteral("127.0.0.1"), server.port, QString());
	QTRY_COMPARE_WITH_TIMEOUT(session.state(), bssh_ssh_session::CONNECTED, 15000);

	/* As a kept session's attach is opened: the raw channel, the helper named. */
	session.set_raw_channel(true);
	session.set_command({ QStringLiteral("beerssh-session"), QStringLiteral("version") });
	session.open_shell(80, 24);
	QTRY_COMPARE_WITH_TIMEOUT(heard, QByteArray("beerssh-session 1\n"), 15000);
	QCOMPARE(failures.size(), 0);

	session.disconnect_from_host();
	QCOMPARE(session.state(), bssh_ssh_session::DISCONNECTED);
#endif
}

void fuzznet_session_test::a_shell_not_kept_is_refused() {
#ifndef BSSH_FUZZNET
	QSKIP("fuzznet is not built here");
#else
	/*
	 * Nothing to serve one: beersshd over fuzznet answers the helper's two
	 * commands alone. Refused at once, saying so, rather than sent and
	 * left unanswered.
	 */
	bssh_fuzznet_session session(QStringLiteral("not an image"));
	QSignalSpy failures(&session, &bssh_ssh_session::failed);
	session.set_command({ QStringLiteral("bash") });
	session.open_shell(80, 24);
	QCOMPARE(failures.size(), 1);
	QVERIFY2(failures.at(0).at(0).toString().contains(QLatin1String("keeps BeerSSH's sessions only")),
	         qPrintable(failures.at(0).at(0).toString()));
	QCOMPARE(session.state(), bssh_ssh_session::FAILED);
#endif
}
