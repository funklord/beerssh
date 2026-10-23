#include "handoff_test.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "input/input_router.h"
#include "model/handoff_point.h"
#include "model/profile.h"
#include "model/session.h"
#include "term/emulator.h"

namespace {

bssh_profile_t local_shell_profile() {
	bssh_profile_t profile = bssh_profile_t::local_shell();
	profile.shell = QStringLiteral("/bin/sh");
	return profile;
}

bool screen_shows(bssh_session *session, const QString &wanted, int timeout_ms = 10000) {
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < timeout_ms) {
		if (session->emulator()->screen().to_text().contains(wanted)) return true;
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	}
	return session->emulator()->screen().to_text().contains(wanted);
}

/* The handoff directory, private to the test: XDG_RUNTIME_DIR, which
 * bssh_handoff_point::directory() is under. */
struct runtime_dir_t {
	QTemporaryDir dir;
	QByteArray before = qgetenv("XDG_RUNTIME_DIR");
	runtime_dir_t() {
		QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
	}
	~runtime_dir_t() {
		if (before.isEmpty()) qunsetenv("XDG_RUNTIME_DIR");
		else qputenv("XDG_RUNTIME_DIR", before);
	}
};

/*
 * Runs the taker -- this binary again -- while this process's event loop
 * serves it. Bounded: a taker that never finishes is killed and reported.
 */
struct taker_t {
	QProcess process;
	QByteArray output;
	bool finished = false;
	int exit_code = -1;

	void run(const QString &path, const QString &id, int timeout_ms = 30000) {
		process.setProcessChannelMode(QProcess::MergedChannels);
		process.start(QCoreApplication::applicationFilePath(),
		              { QStringLiteral("--handoff-take"), path, id });
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < timeout_ms && process.state() != QProcess::NotRunning) {
			QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
			process.waitForFinished(0);
		}
		if (process.state() != QProcess::NotRunning) {
			process.kill();
			process.waitForFinished(5000);
		}
		output = process.readAll();
		finished = process.exitStatus() == QProcess::NormalExit;
		exit_code = process.exitCode();
	}
};

}  // namespace

int bssh_handoff_take_mode(const QString &path, const QString &handoff_id) {
	QString why;
	bssh_session *session = bssh_handoff_point::take(path, handoff_id, nullptr, &why);
	if (!session) {
		printf("take refused: %s\n", qPrintable(why));
		return 3;
	}
	const bool came = session->emulator()->screen().to_text().contains(QStringLiteral("before-move"));
	session->input_router()->send_text(QStringLiteral("echo after-$((6*7))\n"));
	const bool answered = screen_shows(session, QStringLiteral("after-42"));
	printf("screen came along: %s\nanswered: %s\n%s\n", came ? "yes" : "no",
	       answered ? "yes" : "no", qPrintable(session->emulator()->screen().to_text()));
	fflush(stdout);
	session->disconnect_from_host();
	delete session;
	return came && answered ? 0 : 1;
}

/*
 * The real thing: a local shell in this process, given to another process
 * through its handoff point, which runs a command in it and gets the
 * answer. This process then holds nothing of it -- the session it gave is
 * reported given, and is inert.
 */
void handoff_test::a_live_session_moves_to_another_process() {
	runtime_dir_t runtime;
	bssh_session *session = new bssh_session(local_shell_profile());
	session->resize(60, 12);
	session->connect_to_host();
	QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);
	session->input_router()->send_text(QStringLiteral("echo before-move\n"));
	QVERIFY(screen_shows(session, QStringLiteral("before-move")));

	bssh_handoff_point point([session](const QString &id) {
		return id == QStringLiteral("the-one") ? session : nullptr;
	});
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QVERIFY(point.path().startsWith(runtime.dir.path()));
	QList<bssh_session *> given;
	connect(&point, &bssh_handoff_point::session_given, this,
	        [&given](bssh_session *s) { given.append(s); });

	taker_t taker;
	taker.run(point.path(), QStringLiteral("the-one"));
	QVERIFY2(taker.finished && taker.exit_code == 0, taker.output.constData());
	QCOMPARE(given.size(), 1);
	QCOMPARE(given.first(), session);
	QVERIFY(session->is_moving());
	delete session;
}

/* Asked for a session it does not have, the giver says so, and the taker
 * comes away with nothing. */
void handoff_test::a_session_nobody_has_is_refused_and_nothing_moves() {
	runtime_dir_t runtime;
	bssh_handoff_point point([](const QString &) { return nullptr; });
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));

	taker_t taker;
	taker.run(point.path(), QStringLiteral("absent"));
	QCOMPARE(taker.exit_code, 3);
	QVERIFY2(taker.output.contains("no such session"), taker.output.constData());
}

/*
 * A taker that asks and then vanishes before answering: the giver's
 * session was stopped for the exchange, and must come back on its own.
 */
void handoff_test::a_taker_that_vanishes_leaves_the_session_running() {
	runtime_dir_t runtime;
	bssh_session *session = new bssh_session(local_shell_profile());
	session->resize(60, 12);
	session->connect_to_host();
	QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);

	bssh_handoff_point point([session](const QString &) { return session; });
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));

	/* A taker in this process, by hand: ask, then hang up unanswered. */
	const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	const QByteArray path = QFile::encodeName(point.path());
	memcpy(address.sun_path, path.constData(), size_t(path.size()));
	QVERIFY(::connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
	QByteArray request;
	{
		QDataStream out(&request, QIODevice::WriteOnly);
		out.setVersion(QDataStream::Qt_6_0);
		out << QByteArray("take") << QStringLiteral("any");
	}
	quint64 length = quint64(request.size());
	QVERIFY(::write(socket, &length, sizeof(length)) == ssize_t(sizeof(length)));
	QVERIFY(::write(socket, request.constData(), size_t(request.size())) == request.size());
	/* Gone before the giver's state arrives back. */
	::shutdown(socket, SHUT_RD);
	::close(socket);

	/* The giver serves it -- export, send into nothing, no answer -- and
	 * resumes. */
	QTRY_VERIFY_WITH_TIMEOUT(!session->is_moving(), 30000);
	session->input_router()->send_text(QStringLiteral("echo still-$((2*3))\n"));
	QVERIFY2(screen_shows(session, QStringLiteral("still-6")),
	          qPrintable(session->emulator()->screen().to_text()));
	session->disconnect_from_host();
	delete session;
}
