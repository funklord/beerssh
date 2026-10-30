#include "handoff_test.h"

#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMimeData>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "input/input_router.h"
#include "model/app_settings.h"
#include "model/handoff_point.h"
#include "model/profile.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "ui/main_window.h"
#include "ui/terminal_view.h"
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
	if (qEnvironmentVariableIsSet("BSSH_TEST_OFFER_MULTIPLEXERS"))
		bssh_app_settings().set_offer_multiplexer_sessions(true);
	QString why;
	bssh_session *session = bssh_handoff_point::take(path, handoff_id, nullptr, &why);
	if (!session) {
		printf("take refused: %s\n", qPrintable(why));
		return 3;
	}
	const bool came = session->emulator()->screen().to_text().contains(QStringLiteral("before-move"));

	/* What tmux and screen are running, listed again on arrival. */
	int multiplexers = -1;
	QObject::connect(session, &bssh_session::multiplexers_found,
	                 [&multiplexers](const QList<bssh_mux_session_t> &found) { multiplexers = int(found.size()); });
	QElapsedTimer waited;
	waited.start();
	while (multiplexers < 0 && waited.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	printf("multiplexers: %d\n", multiplexers);

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

	/*
	 * A tmux that reports one session, first on the taker's PATH, so what
	 * it finds on arrival is known: a session taken from another process
	 * never connects there, and it is listed again rather than arriving
	 * with no tmux button (sec 10.2a, sec 8.7a).
	 */
	QTemporaryDir fakes;
	QVERIFY(fakes.isValid());
	QFile tmux(fakes.filePath(QStringLiteral("tmux")));
	QVERIFY(tmux.open(QIODevice::WriteOnly));
	tmux.write("#!/bin/sh\nprintf 'work\\t1\\t0\\n'\n");
	tmux.close();
	tmux.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	const QByteArray path_before = qgetenv("PATH");
	qputenv("PATH", fakes.path().toLocal8Bit() + ':' + path_before);
	/* The suite turns the offer off in every process it starts, the taker
	 * included; this one asks for it back, with the fake tmux in front. */
	qputenv("BSSH_TEST_OFFER_MULTIPLEXERS", "1");

	taker_t taker;
	taker.run(point.path(), QStringLiteral("the-one"));
	qputenv("PATH", path_before);
	qunsetenv("BSSH_TEST_OFFER_MULTIPLEXERS");
	QVERIFY2(taker.finished && taker.exit_code == 0, taker.output.constData());
	QVERIFY2(taker.output.contains("multiplexers: 1"), taker.output.constData());
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

int bssh_handoff_give_mode() {
	bssh_session *session = new bssh_session(local_shell_profile());
	session->resize(60, 12);
	session->connect_to_host();
	QElapsedTimer up;
	up.start();
	while (session->state() != bssh_ssh_session::CONNECTED && up.elapsed() < 10000)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	session->input_router()->send_text(QStringLiteral("echo given-$((3*5))\n"));
	screen_shows(session, QStringLiteral("given-15"));

	bool given = false;
	bssh_handoff_point point([session](const QString &id) {
		return id == session->handoff_id() ? session : nullptr;
	});
	QObject::connect(&point, &bssh_handoff_point::session_given,
	                 [&given](bssh_session *) { given = true; });
	QString why;
	if (!point.listen(&why)) {
		printf("cannot listen: %s\n", qPrintable(why));
		return 2;
	}
	printf("%s %s\n", qPrintable(point.path()), qPrintable(session->handoff_id()));
	fflush(stdout);

	QElapsedTimer serving;
	serving.start();
	while (!given && serving.elapsed() < 60000)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	delete session;
	return given ? 0 : 1;
}

/*
 * What a session drag carries to another BeerSSH: who to ask, and for
 * what. The path goes last because it is the one part that can hold a
 * colon, and a payload from this process is not a foreign one whatever it
 * carries.
 */
void handoff_test::a_drag_payload_names_where_to_ask() {
	QMimeData mime;
	mime.setData(bssh_session_mime_type(), QByteArray("1:4:abc-def:/run/user/1000/bee:ssh/handoff-1.sock"));
	QString path, id;
	QVERIFY(bssh_session_payload_handoff(&mime, &path, &id));
	QCOMPARE(id, QStringLiteral("abc-def"));
	QCOMPARE(path, QStringLiteral("/run/user/1000/bee:ssh/handoff-1.sock"));

	/* The older, shorter form from another process: foreign, and nothing
	 * to ask. */
	mime.setData(bssh_session_mime_type(), QByteArray("1:4"));
	QVERIFY(!bssh_session_payload_handoff(&mime, &path, &id));

	/* This process's own, however much it says. */
	mime.setData(bssh_session_mime_type(), bssh_session_payload(4, QStringLiteral("x"), QStringLiteral("/p")));
	QVERIFY(!bssh_session_payload_handoff(&mime, &path, &id));
	bool foreign = true;
	QCOMPARE(bssh_session_payload_index(&mime, &foreign), 4);
	QVERIFY(!foreign);
}

/*
 * A session another BeerSSH let go over this one: taken, joined to the
 * registry, shown in a window -- a new one, since it landed on none of
 * ours -- and answering.
 */
void handoff_test::a_dropped_session_lands_in_this_process() {
	runtime_dir_t runtime;
	QProcess giver;
	giver.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--handoff-give") });
	QVERIFY(giver.waitForStarted(10000));
	QElapsedTimer ready;
	ready.start();
	QByteArray line;
	while (!line.contains('\n') && ready.elapsed() < 30000 && giver.state() == QProcess::Running) {
		giver.waitForReadyRead(100);
		line += giver.readAllStandardOutput();
	}
	const QList<QByteArray> fields = line.trimmed().split(' ');
	QVERIFY2(fields.size() == 2, line.constData());

	bssh_session_registry *registry = bssh_session_registry::shared();
	const int before = registry->count();
	bssh_main_window::take_from_another_process(QString::fromUtf8(fields.at(0)),
	                                            QString::fromUtf8(fields.at(1)),
	                                            QPoint(-10000, -10000));
	QCOMPARE(registry->count(), before + 1);
	bssh_session *session = registry->at(registry->count() - 1);
	QVERIFY(session);
	QVERIFY2(session->emulator()->screen().to_text().contains(QStringLiteral("given-15")),
	          "the screen from the other process did not come along");
	bssh_main_window *window = nullptr;
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		if (auto *each = qobject_cast<bssh_main_window *>(widget))
			if (each->registry() && each->isVisible() && each->shown_sessions().contains(session)) window = each;
	}
	QVERIFY2(window, "the session arrived and no window shows it");

	session->input_router()->send_text(QStringLiteral("echo landed-$((4*4))\n"));
	QVERIFY2(screen_shows(session, QStringLiteral("landed-16")),
	          qPrintable(session->emulator()->screen().to_text()));

	/* Exited already, as likely as not: it stops serving once taken, and
	 * waitForFinished() is false for a process that is not running. */
	if (giver.state() != QProcess::NotRunning) QVERIFY(giver.waitForFinished(30000));
	QCOMPARE(giver.exitStatus(), QProcess::NormalExit);
	QCOMPARE(giver.exitCode(), 0);
	registry->close(session);
	window->close();
	QCoreApplication::processEvents();
}

/*
 * An upgrade: the real program, started with --adopt-from, takes every
 * live session from this process and asks it to quit. Each shell writes a
 * file between lines of output; nobody reading its terminal would block
 * it within a second, so a file still changing well after the move is the
 * new program holding the session, not a session left to nobody.
 */
void handoff_test::a_new_version_takes_every_session() {
	const QString program = QFileInfo(QCoreApplication::applicationDirPath() +
	                                  QStringLiteral("/../beerssh")).absoluteFilePath();
	if (!QFileInfo(program).isExecutable()) QSKIP("the app is not built beside the tests; run make");

	runtime_dir_t runtime;
	QTemporaryDir files;
	QList<bssh_session *> sessions;
	QStringList beats;
	for (int i = 0; i < 2; ++i) {
		bssh_profile_t profile = local_shell_profile();
		const QString beat = files.filePath(QStringLiteral("beat-%1").arg(i));
		beats.append(beat);
		profile.command = { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
		                    QStringLiteral("while :; do date +%s%N > %1; "
		                                   "head -c 1024 /dev/zero | tr '\\\\0' x; echo; "
		                                   "sleep 0.1; done").arg(beat) };
		bssh_session *session = new bssh_session(profile);
		session->resize(60, 12);
		session->connect_to_host();
		QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);
		sessions.append(session);
	}

	bssh_handoff_point point([&sessions](const QString &id) -> bssh_session * {
		for (bssh_session *s : sessions)
			if (s->handoff_id() == id && !s->is_moving()) return s;
		return nullptr;
	});
	point.set_lister([&sessions] {
		QStringList ids;
		for (bssh_session *s : sessions) ids.append(s->handoff_id());
		return ids;
	});
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	int given = 0;
	bool asked_to_quit = false;
	connect(&point, &bssh_handoff_point::session_given, this, [&given](bssh_session *) { ++given; });
	connect(&point, &bssh_handoff_point::quit_requested, this, [&asked_to_quit] { asked_to_quit = true; });

	QProcess app;
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
	app.setProcessEnvironment(environment);
	app.setProcessChannelMode(QProcess::MergedChannels);
	app.start(program, { QStringLiteral("--adopt-from"), point.path() });
	QVERIFY(app.waitForStarted(10000));

	QElapsedTimer timer;
	timer.start();
	while (!(given == 2 && asked_to_quit) && timer.elapsed() < 60000 &&
	       app.state() == QProcess::Running)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	QVERIFY2(given == 2, qPrintable(QStringLiteral("%1 of 2 given; the app said: %2")
	                                        .arg(given).arg(QString::fromLocal8Bit(app.readAll()))));
	QVERIFY(asked_to_quit);
	for (bssh_session *session : sessions) delete session;

	/* Well after the move, and with this process holding nothing: the
	 * shells still running means the new program is reading them. */
	QMap<QString, QByteArray> first;
	for (const QString &beat : beats) {
		QFile file(beat);
		QVERIFY(file.open(QIODevice::ReadOnly));
		first.insert(beat, file.readAll());
	}
	QTest::qWait(3000);
	for (const QString &beat : beats) {
		QFile file(beat);
		QVERIFY(file.open(QIODevice::ReadOnly));
		QVERIFY2(file.readAll() != first.value(beat),
		         qPrintable(QStringLiteral("%1 stopped: nobody is reading that shell").arg(beat)));
	}

	app.kill();
	app.waitForFinished(10000);
}

void handoff_test::a_torn_out_tab_and_a_new_window_start_processes_of_their_own() {
	/*
	 * One process per window, as TDE Konsole's are, so ending one window's
	 * process ends no other (the holder, 2026-10-25, sec 8.4). Seen here
	 * as what would be started -- the starter is replaced, so nothing is
	 * -- and the next test runs the program it names.
	 */
	runtime_dir_t runtime;
	QList<QStringList> started;
	bssh_main_window::set_process_starter([&started](const QString &, const QStringList &arguments) {
		started.append(arguments);
		return true;
	});

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	bssh_session_registry *registry = window.registry();

	QList<bssh_session *> sessions;
	for (int i = 0; i < 2; ++i) {
		bssh_session *session = registry->create(local_shell_profile());
		window.adopt_session(session);
		session->connect_to_host();
		QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);
		sessions.append(session);
	}

	bssh_handoff_point point([registry](const QString &id) -> bssh_session * {
		for (bssh_session *s : registry->sessions())
			if (s->handoff_id() == id) return s;
		return nullptr;
	});
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QCOMPARE(bssh_handoff_point::instance(), &point);

	/* Torn out: a program that takes this one session, at its size. */
	bssh_session *torn = sessions.last();
	QVERIFY2(!window.tear_out_tab(torn), "the tab became a window in this process");
	QCOMPARE(started.size(), 1);
	const QStringList &arguments = started.first();
	QCOMPARE(arguments.value(arguments.indexOf(QStringLiteral("--adopt-from")) + 1), point.path());
	QCOMPARE(arguments.value(arguments.indexOf(QStringLiteral("--adopt-session")) + 1),
	         torn->handoff_id());
	QCOMPARE(arguments.value(arguments.indexOf(QStringLiteral("--geometry")) + 1),
	         QStringLiteral("%1x%2").arg(torn->emulator()->screen().cols())
	                 .arg(torn->emulator()->screen().rows()));
	QVERIFY2(registry->sessions().contains(torn), "let go before anything took it");

	/* New Window: a program of its own, which opens its own shell. */
	QAction *new_window = nullptr;
	for (QAction *action : window.findChildren<QAction *>())
		if (action->text() == QStringLiteral("New &Window")) new_window = action;
	QVERIFY(new_window);
	new_window->trigger();
	QCOMPARE(started.size(), 2);
	QCOMPARE(started.last().value(0), QStringLiteral("--geometry"));
	QVERIFY(!started.last().contains(QStringLiteral("--adopt-from")));

	bssh_main_window::set_process_starter(nullptr);
	for (bssh_session *session : sessions) registry->close(session);
}

void handoff_test::a_torn_out_tab_arrives_in_a_process_of_its_own() {
	/*
	 * What the tear-out starts, run: --adopt-session takes the one
	 * session it names and leaves the other program running with the
	 * rest -- not asked to quit, as a restart's --adopt-from alone is.
	 */
	const QString program = QFileInfo(QCoreApplication::applicationDirPath() +
	                                  QStringLiteral("/../beerssh")).absoluteFilePath();
	if (!QFileInfo(program).isExecutable()) QSKIP("the app is not built beside the tests; run make");

	runtime_dir_t runtime;
	QTemporaryDir files;
	QList<bssh_session *> sessions;
	QStringList beats;
	for (int i = 0; i < 2; ++i) {
		bssh_profile_t profile = local_shell_profile();
		const QString beat = files.filePath(QStringLiteral("beat-%1").arg(i));
		beats.append(beat);
		profile.command = { QStringLiteral("/bin/sh"), QStringLiteral("-c"),
		                    QStringLiteral("while :; do date +%s%N > %1; echo tick; "
		                                   "sleep 0.1; done").arg(beat) };
		bssh_session *session = new bssh_session(profile);
		session->resize(60, 12);
		session->connect_to_host();
		QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);
		sessions.append(session);
	}

	bssh_handoff_point point([&sessions](const QString &id) -> bssh_session * {
		for (bssh_session *s : sessions)
			if (s->handoff_id() == id && !s->is_moving()) return s;
		return nullptr;
	});
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QList<bssh_session *> given;
	bool asked_to_quit = false;
	connect(&point, &bssh_handoff_point::session_given, this,
	        [&given](bssh_session *s) { given.append(s); });
	connect(&point, &bssh_handoff_point::quit_requested, this, [&asked_to_quit] { asked_to_quit = true; });

	QProcess app;
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
	app.setProcessEnvironment(environment);
	app.setProcessChannelMode(QProcess::MergedChannels);
	app.start(program, { QStringLiteral("--adopt-from"), point.path(),
	                     QStringLiteral("--adopt-session"), sessions.first()->handoff_id() });
	QVERIFY(app.waitForStarted(10000));

	QElapsedTimer timer;
	timer.start();
	while (given.isEmpty() && timer.elapsed() < 60000 && app.state() == QProcess::Running)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	QVERIFY2(given.size() == 1, qPrintable(QStringLiteral("nothing given; the app said: %1")
	                                       .arg(QString::fromLocal8Bit(app.readAll()))));
	QCOMPARE(given.first(), sessions.first());

	/* A while longer: nothing else is asked for, and nobody asks us to go. */
	QTest::qWait(1500);
	QCOMPARE(given.size(), 1);
	QVERIFY2(!asked_to_quit, "a torn-out tab asked the program it left to quit");
	QCOMPARE(sessions.last()->state(), bssh_ssh_session::CONNECTED);
	QVERIFY(app.state() == QProcess::Running);

	/* The taken shell goes on being read with this process holding none of it. */
	delete sessions.takeFirst();
	QFile file(beats.first());
	QVERIFY(file.open(QIODevice::ReadOnly));
	const QByteArray first = file.readAll();
	file.close();
	QTest::qWait(2000);
	QVERIFY(file.open(QIODevice::ReadOnly));
	QVERIFY2(file.readAll() != first, "the torn-out shell stopped: nobody is reading it");

	app.kill();
	app.waitForFinished(10000);
	for (bssh_session *session : sessions) delete session;
}
