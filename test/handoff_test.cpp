#include "handoff_test.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QSpinBox>
#include <QTimer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <cstring>
#include <thread>

#include "input/input_router.h"
#include "model/app_settings.h"
#include "model/arrangement.h"
#include "model/handoff_point.h"
#include "model/profile.h"
#include "model/profile_store.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "ui/layout_manager.h"
#include "ui/main_window.h"
#include "model/program_watch.h"
#include "ui/pane_bar.h"
#include "ui/pane_container.h"
#include "ui/terminal_view.h"
#include "ui/terminals_panel.h"
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
/* TMPDIR is the runtime directory on macOS (bssh_handoff_point::directory). */
#ifdef Q_OS_MACOS
#define RUNTIME_VARIABLE "TMPDIR"
#else
#define RUNTIME_VARIABLE "XDG_RUNTIME_DIR"
#endif

struct runtime_dir_t {
	QTemporaryDir dir;
	QByteArray before = qgetenv(RUNTIME_VARIABLE);
	runtime_dir_t() {
		QFile::setPermissions(dir.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
		qputenv(RUNTIME_VARIABLE, dir.path().toUtf8());
	}
	~runtime_dir_t() {
		if (before.isEmpty()) qunsetenv(RUNTIME_VARIABLE);
		else qputenv(RUNTIME_VARIABLE, before);
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

int bssh_handoff_terminals_mode(const QString &path) {
	QString why;
	const QList<bssh_handoff_point::listed_t> found = bssh_handoff_point::terminals(path, &why, 5000);
	for (const bssh_handoff_point::listed_t &entry : found)
		printf("%s|%s\n", qPrintable(entry.id), qPrintable(entry.title));
	printf("peers: %d\n", int(bssh_handoff_point::peers().size()));
	fflush(stdout);
	return 0;
}

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
	/*
	 * And a screen with none, as screen says it: the machine's own screen
	 * sessions were counted otherwise, and the test failed whenever
	 * anyone on it had one open -- "multiplexers: 20", 2026-11-12.
	 */
	QFile screen(fakes.filePath(QStringLiteral("screen")));
	QVERIFY(screen.open(QIODevice::WriteOnly));
	screen.write("#!/bin/sh\necho 'No Sockets found in /nonexistent.'\nexit 1\n");
	screen.close();
	screen.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
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

/* Sockets left by processes a signal ended are swept; a live one is not. */
void handoff_test::only_sockets_nobody_listens_on_are_swept() {
	runtime_dir_t runtime;
	bssh_handoff_point live([](const QString &) -> bssh_session * { return nullptr; });
	QString why;
	QVERIFY2(live.listen(&why), qPrintable(why));

	/* A socket bound and abandoned, as a killed process leaves it. */
	const QString dead = bssh_handoff_point::path_for(999999);
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	const QByteArray encoded = QFile::encodeName(dead);
	QVERIFY(encoded.size() < int(sizeof(address.sun_path)));
	std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()));
	const int abandoned = ::socket(AF_UNIX, SOCK_STREAM, 0);
	QVERIFY(abandoned >= 0);
	QVERIFY(::bind(abandoned, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
	::close(abandoned);
	QVERIFY(QFileInfo::exists(dead));

	QCOMPARE(bssh_handoff_point::sweep_stale(), 1);
	QVERIFY2(!QFileInfo::exists(dead), "the abandoned socket stayed");
	QVERIFY2(QFileInfo::exists(live.path()), "the live socket was swept");
}

/* A drag from another process is over this one, and then is not: the
 * hover message carries the point, and the empty one ends it (sec 8.7a). */
void handoff_test::a_hover_says_where_and_then_that_it_left() {
	runtime_dir_t runtime;
	bssh_handoff_point point([](const QString &) -> bssh_session * { return nullptr; });
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QSignalSpy hovered(&point, &bssh_handoff_point::hovered);

	/*
	 * Sent from a thread of its own: hover() waits for the answer, and the
	 * point answering it is this process's, served from the event loop
	 * that has to keep turning meanwhile. In use the two are different
	 * processes.
	 */
	const auto send = [&point](const QPoint &at, bool active) {
		std::atomic<int> result { -1 };
		std::thread sender([&] { result = bssh_handoff_point::hover(point.path(), at, active) ? 1 : 0; });
		QElapsedTimer waited;
		waited.start();
		while (result < 0 && waited.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		sender.join();
		return result == 1;
	};

	QVERIFY(send(QPoint(120, 340), true));
	QTRY_COMPARE(hovered.count(), 1);
	QCOMPARE(hovered.at(0).at(0).toPoint(), QPoint(120, 340));
	QCOMPARE(hovered.at(0).at(1).toBool(), true);

	QVERIFY(send(QPoint(), false));
	QTRY_COMPARE(hovered.count(), 2);
	QCOMPARE(hovered.at(1).at(1).toBool(), false);

	/*
	 * A process that takes the hover and never answers holds the drag a
	 * moment, not the hand-off's ten seconds: hover() runs every 30 ms of
	 * a drag, on the dragging window's thread.
	 */
	const QString silent_path = bssh_handoff_point::directory() + QStringLiteral("/handoff-999997.sock");
	const int silent = ::socket(AF_UNIX, SOCK_STREAM, 0);
	QVERIFY(silent >= 0);
	const auto unlisten = qScopeGuard([silent, silent_path] {
		::close(silent);
		QFile::remove(silent_path);
	});
	sockaddr_un address;
	std::memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, QFile::encodeName(silent_path).constData(), sizeof address.sun_path - 1);
	QVERIFY(::bind(silent, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
	QVERIFY(::listen(silent, 64) == 0);
	QElapsedTimer held;
	held.start();
	QVERIFY(!bssh_handoff_point::hover(silent_path, QPoint(1, 1), true));
	QVERIFY2(held.elapsed() < 1000, qPrintable(QStringLiteral("a hover held the drag %1 ms").arg(held.elapsed())));
}

/*
 * File > Restart, Keeping Sessions, in a window built before the hand-off
 * point listens -- main()'s order, so every window the program opened.
 * Asked at build time, the entry was never there (holder, 2026-11-21:
 * "Never seen that menu entry").
 */
void handoff_test::the_restart_is_offered_once_the_point_listens() {
	runtime_dir_t runtime;
	QVERIFY(!bssh_handoff_point::instance());
	bssh_main_window window;
	window.show();
	QAction *restart = window.findChild<QAction *>(QStringLiteral("menu_restart_keeping_sessions"));
	QVERIFY(restart);
	QMenu *file = nullptr;
	for (QMenu *menu : window.findChildren<QMenu *>())
		if (menu->actions().contains(restart)) file = menu;
	QVERIFY(file);
	const auto offered = [&] {
		file->popup(window.mapToGlobal(QPoint(10, 10)));
		const bool shown = restart->isVisible();
		file->hide();
		return shown;
	};
	QVERIFY2(!offered(), "offered with nothing to hand sessions over through");

	bssh_handoff_point point([](const QString &) { return nullptr; });
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QVERIFY2(offered(), "not offered once this process can hand its sessions over");
}

/*
 * A package upgrade renames a new file over the program; the watch says
 * so once, and not for anything else written beside it.
 */
void handoff_test::an_installed_new_version_is_noticed() {
	QTemporaryDir dir;
	const QString program = dir.filePath(QStringLiteral("beerssh"));
	const auto write = [](const QString &path, const QByteArray &body) {
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly)) return false;
		file.write(body);
		file.close();
		return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	};
	QVERIFY(write(program, "old"));
	bssh_program_watch watch(program);
	watch.set_settle_ms(50);
	QSignalSpy replaced(&watch, &bssh_program_watch::program_replaced);

	QVERIFY(write(dir.filePath(QStringLiteral("beerssh-session")), "neighbour"));
	QTest::qWait(400);
	QCOMPARE(replaced.count(), 0);

	/* dpkg's way: the new file beside the old, then renamed over it. */
	const QString incoming = dir.filePath(QStringLiteral("beerssh.dpkg-new"));
	QVERIFY(write(incoming, "new"));
	QVERIFY(::rename(QFile::encodeName(incoming).constData(), QFile::encodeName(program).constData()) == 0);
	QTRY_COMPARE(replaced.count(), 1);
	QVERIFY(watch.replaced());
	QTest::qWait(300);
	QCOMPARE(replaced.count(), 1);

	/* And a second upgrade, again. */
	QVERIFY(write(incoming, "newer"));
	QVERIFY(::rename(QFile::encodeName(incoming).constData(), QFile::encodeName(program).constData()) == 0);
	QTRY_COMPARE(replaced.count(), 2);
}

/*
 * Told of the upgrade, a window shows the bar over its terminals, a
 * window made after shows it too, and Later puts it away.
 */
void handoff_test::every_window_offers_the_restart_after_an_upgrade() {
	bssh_main_window before;
	before.show();
	QVERIFY(!before.update_bar());
	const auto forget = qScopeGuard([] { bssh_main_window::offer_restart(false); });
	bssh_main_window::offer_restart();
	QVERIFY(before.update_bar());
	QTRY_VERIFY(before.update_bar()->isVisible());
	QVERIFY(before.update_bar()->findChild<QPushButton *>(QStringLiteral("update_restart")));

	bssh_main_window after;
	after.show();
	QVERIFY(after.update_bar());
	QTRY_VERIFY(after.update_bar()->isVisible());

	QPushButton *later = after.update_bar()->findChild<QPushButton *>(QStringLiteral("update_later"));
	QVERIFY(later);
	QTest::mouseClick(later, Qt::LeftButton);
	QVERIFY(!after.update_bar()->isVisible());
	QVERIFY(before.update_bar()->isVisible());
}

/*
 * Every window is a process, so the bar's Restart asks the others to
 * restart too: the request is answered and signalled, a process that
 * never answers holds the window only a moment, and the bar sends it to
 * every other process before restarting its own.
 */
void handoff_test::a_restart_is_asked_of_the_other_windows_processes() {
	runtime_dir_t runtime;
	bssh_handoff_point point([](const QString &) -> bssh_session * { return nullptr; });
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));
	QSignalSpy asked(&point, &bssh_handoff_point::restart_requested);

	/* From a thread: the point answering is this process's own. */
	std::atomic<int> result { -1 };
	std::thread sender([&] { result = bssh_handoff_point::request_restart(point.path()) ? 1 : 0; });
	QElapsedTimer waited;
	waited.start();
	while (result < 0 && waited.elapsed() < 5000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	sender.join();
	QCOMPARE(int(result), 1);
	QTRY_COMPARE(asked.count(), 1);

	/* Another process, listening and never answering. */
	const QString other = bssh_handoff_point::directory() + QStringLiteral("/handoff-999996.sock");
	const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
	QVERIFY(listener >= 0);
	const auto unlisten = qScopeGuard([listener, other] {
		::close(listener);
		QFile::remove(other);
	});
	sockaddr_un address;
	std::memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, QFile::encodeName(other).constData(), sizeof address.sun_path - 1);
	QVERIFY(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
	QVERIFY(::listen(listener, 8) == 0);
	/* Asked whether anything came, an accept() must not wait for it. */
	QVERIFY(::fcntl(listener, F_SETFL, ::fcntl(listener, F_GETFL) | O_NONBLOCK) == 0);
	QElapsedTimer held;
	held.start();
	QVERIFY(!bssh_handoff_point::request_restart(other));
	QVERIFY2(held.elapsed() < 1000, qPrintable(QStringLiteral("held %1 ms").arg(held.elapsed())));
	const auto heard = [listener] {
		QByteArray bytes;
		for (;;) {
			const int one = ::accept(listener, nullptr, nullptr);
			if (one < 0) break;
			char buffer[256];
			const ssize_t got = ::recv(one, buffer, sizeof buffer, MSG_DONTWAIT);
			if (got > 0) bytes.append(buffer, int(got));
			::close(one);
			pollfd more = { listener, POLLIN, 0 };
			if (::poll(&more, 1, 0) <= 0) break;
		}
		return bytes;
	};
	QVERIFY(heard().contains("restart"));

	/* The bar: the other process asked, and this one restarted. */
	QList<QStringList> started;
	bssh_main_window::set_process_starter([&started](const QString &, const QStringList &arguments) {
		started.append(arguments);
		return true;
	});
	const auto unset = qScopeGuard([] { bssh_main_window::set_process_starter(nullptr); });
	const auto forget = qScopeGuard([] { bssh_main_window::offer_restart(false); });
	bssh_main_window window;
	window.show();
	bssh_main_window::offer_restart();
	QPushButton *restart = window.update_bar()->findChild<QPushButton *>(QStringLiteral("update_restart"));
	QVERIFY(restart);
	QTest::mouseClick(restart, Qt::LeftButton);
	QVERIFY(heard().contains("restart"));
	QCOMPARE(started.size(), 1);
	QCOMPARE(started.first(), (QStringList{ QStringLiteral("--adopt-from"), point.path() }));
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
	/* The id last, after the path, which may hold a space. */
	const QByteArray trimmed = line.trimmed();
	const qsizetype gap = trimmed.lastIndexOf(' ');
	QVERIFY2(gap > 0, line.constData());
	const QList<QByteArray> fields = { trimmed.left(gap), trimmed.mid(gap + 1) };

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

/*
 * The Terminals sidebar's question to another process (the holder,
 * 2026-11-20): what it would give, by title. Asked from a child process,
 * as the sidebar asks from another window's, while this one serves. A
 * title carrying the framing's own characters arrives as one line.
 */
void handoff_test::another_process_lists_its_terminals_by_title() {
	runtime_dir_t runtime;
	bssh_handoff_point point([](const QString &) -> bssh_session * { return nullptr; });
	point.set_cataloguer([] {
		QList<bssh_handoff_point::listed_t> entries;
		entries.append({ QStringLiteral("aaa"), QStringLiteral("build host") });
		entries.append({ QStringLiteral("bbb"), QStringLiteral("tab\there\nnewline") });
		return entries;
	});
	QString why;
	QVERIFY2(point.listen(&why), qPrintable(why));

	QProcess asker;
	asker.setProcessChannelMode(QProcess::MergedChannels);
	asker.start(QCoreApplication::applicationFilePath(),
	            { QStringLiteral("--handoff-terminals"), point.path() });
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 20000 && asker.state() != QProcess::NotRunning) {
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
		asker.waitForFinished(0);
	}
	if (asker.state() != QProcess::NotRunning) {
		asker.kill();
		asker.waitForFinished(5000);
		QFAIL("the asking process did not finish");
	}
	const QString output = QString::fromLocal8Bit(asker.readAll());
	QVERIFY2(output.contains(QStringLiteral("aaa|build host\n")), qPrintable(output));
	QVERIFY2(output.contains(QStringLiteral("bbb|tab here newline\n")), qPrintable(output));

	/* This process is the asker's one peer: its own socket is not counted. */
	QVERIFY2(output.contains(QStringLiteral("peers: 1\n")), qPrintable(output));
}

void handoff_test::an_arrangement_reads_back_what_it_wrote() {
	bssh_arrangement_t written;
	bssh_arrangement_t::window_t first;
	first.geometry = QRect(10, 20, 800, 600);
	first.maximized = true;
	first.terminals_panel = true;
	first.shape = QStringLiteral("H(a,V(b,c))");
	first.terminals = { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };
	first.current = QStringLiteral("c");
	bssh_arrangement_t::window_t second;
	second.terminals = { QStringLiteral("d") };
	written.windows = { first, second };

	QString why;
	const bssh_arrangement_t read = bssh_arrangement_t::from_text(written.to_text(), &why);
	QVERIFY2(why.isEmpty(), qPrintable(why));
	QCOMPARE(read.windows.size(), 2);
	QCOMPARE(read.windows.at(0).geometry, first.geometry);
	QVERIFY(read.windows.at(0).maximized);
	QVERIFY(!read.windows.at(0).full_screen);
	QVERIFY(read.windows.at(0).terminals_panel);
	QCOMPARE(read.windows.at(0).shape, first.shape);
	QCOMPARE(read.windows.at(0).terminals, first.terminals);
	QCOMPARE(read.windows.at(0).current, first.current);
	QCOMPARE(read.windows.at(1).terminals, second.terminals);
	QVERIFY(!read.windows.at(1).geometry.isValid());

	/* Not one, another version's, or a window of nothing: refused or left out. */
	QVERIFY(bssh_arrangement_t::from_text(QStringLiteral("ids\nabc")).is_empty());
	QVERIFY(bssh_arrangement_t::from_text(QStringLiteral("{\"format\":\"beerssh-arrangement\",\"version\":2,"
	                                                     "\"windows\":[{\"terminals\":[\"a\"]}]}"), &why)
	                .is_empty());
	QVERIFY(why.contains(QStringLiteral("version")));
	QVERIFY(bssh_arrangement_t::from_text(QStringLiteral("{\"format\":\"beerssh-arrangement\",\"version\":1,"
	                                                     "\"windows\":[{\"terminals\":[]}]}"))
	                .is_empty());
}

int bssh_handoff_arranged_mode() {
	bssh_session_registry *registry = bssh_session_registry::shared();
	QHash<QString, bssh_session *> shell;
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
	                               QStringLiteral("d") }) {
		bssh_session *session = registry->create(local_shell_profile());
		session->resize(60, 12);
		session->connect_to_host();
		QElapsedTimer up;
		up.start();
		while (session->state() != bssh_ssh_session::CONNECTED && up.elapsed() < 10000)
			QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
		session->input_router()->send_text(QStringLiteral("echo pane-$(echo %1)\n").arg(letter));
		if (!screen_shows(session, QStringLiteral("pane-") + letter)) {
			printf("shell %s did not start\n", qPrintable(letter));
			return 2;
		}
		shell.insert(letter, session);
	}

	bssh_main_window first;
	first.resize(1200, 700);
	first.show();
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") })
		first.adopt_session(shell.value(letter));
	first.layout_manager()->set_tree(bssh_layout_manager::parse_shape(
	        QStringLiteral("H(a,V(b,c))"), [&shell](const QString &name) { return shell.value(name); }));
	QCoreApplication::processEvents();
	first.panes()->focus_session(shell.value(QStringLiteral("c")));
	first.set_terminals_panel_shown(true);

	bssh_main_window second;
	second.resize(700, 500);
	second.show();
	second.adopt_session(shell.value(QStringLiteral("d")));
	QCoreApplication::processEvents();

	bssh_handoff_point point([registry](const QString &id) -> bssh_session * {
		for (bssh_session *session : registry->sessions()) {
			if (session->handoff_id() == id && session->state() == bssh_ssh_session::CONNECTED)
				return session;
		}
		return nullptr;
	});
	point.set_arranger([] { return bssh_main_window::describe_arrangement().to_text(); });
	point.set_cataloguer([registry] {
		QList<bssh_handoff_point::listed_t> entries;
		for (bssh_session *session : registry->sessions()) {
			if (session->state() == bssh_ssh_session::CONNECTED)
				entries.append({ session->handoff_id(), session->title() });
		}
		return entries;
	});
	QObject::connect(&point, &bssh_handoff_point::session_given, registry,
	                 &bssh_session_registry::give_away);
	QString why;
	if (!point.listen(&why)) {
		printf("cannot listen: %s\n", qPrintable(why));
		return 2;
	}
	printf("%s\n", qPrintable(point.path()));
	fflush(stdout);

	QElapsedTimer serving;
	serving.start();
	while (registry->count() > 0 && serving.elapsed() < 60000)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	return registry->count() == 0 ? 0 : 1;
}

/*
 * An upgrade keeps the windows, not only the terminals (the holder,
 * 2026-11-20): the old version, another process, describes its two
 * windows; this one takes the first -- the panes' shape, the current
 * terminal, the sidebar -- and would start a program for the second,
 * which it then plays itself, as that program would.
 */
void handoff_test::a_new_version_takes_the_windows_as_they_were() {
	runtime_dir_t runtime;
	bssh_session_registry *shared = bssh_session_registry::shared();
	const auto tidy = qScopeGuard([shared] {
		while (shared->count() > 0) shared->close(shared->at(0));
	});

	QProcess old_version;
	old_version.setProcessChannelMode(QProcess::SeparateChannels);
	old_version.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--handoff-arranged") });
	const auto stop = qScopeGuard([&old_version] {
		if (old_version.state() == QProcess::NotRunning) return;
		old_version.kill();
		old_version.waitForFinished(5000);
	});
	QVERIFY(old_version.waitForStarted(10000));
	QElapsedTimer timer;
	timer.start();
	while (!old_version.canReadLine() && timer.elapsed() < 30000 && old_version.state() != QProcess::NotRunning)
		old_version.waitForReadyRead(100);
	const QString path = QString::fromLocal8Bit(old_version.readLine()).trimmed();
	QVERIFY2(path.endsWith(QStringLiteral(".sock")), qPrintable(path + old_version.readAllStandardError()));

	QList<QStringList> started;
	bssh_main_window::set_process_starter([&started](const QString &, const QStringList &arguments) {
		started.append(arguments);
		return true;
	});
	const auto unset = qScopeGuard([] { bssh_main_window::set_process_starter(nullptr); });

	const auto marker = [](bssh_session *session) {
		const QString text = session ? session->emulator()->screen().to_text() : QString();
		for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
		                               QStringLiteral("d") }) {
			if (text.contains(QStringLiteral("pane-") + letter)) return letter;
		}
		return QString();
	};

	bssh_main_window first;
	first.resize(900, 600);
	first.show();
	QCOMPARE(first.take_arranged_from(path), 3);
	QCOMPARE(started.size(), 1);
	QCOMPARE(started.first().size(), 4);
	QCOMPARE(started.first().mid(0, 3), (QStringList{ QStringLiteral("--adopt-from"), path,
	                                                  QStringLiteral("--adopt-window") }));
	QCoreApplication::processEvents();
	QCOMPARE(bssh_layout_manager::shape_text(first.layout_manager()->tree(), marker),
	         QStringLiteral("H(a,V(b,c))"));
	const auto focused = [&first, &marker] {
		const QString current = first.describe_window().current;
		for (bssh_session *session : first.shown_sessions())
			if (session->handoff_id() == current) return marker(session);
		return QString();
	};
	QTest::qWait(100);
	QCOMPARE(focused(), QStringLiteral("c"));
	QVERIFY(first.terminals_panel_shown());

	/* The window the program started for it would be. */
	bssh_main_window second;
	second.resize(900, 600);
	second.show();
	/* A terminal it does not have names no window to take; one it has, its window. */
	QCOMPARE(second.take_arranged_from(path, QStringLiteral("no-such-terminal")), -1);
	QCOMPARE(second.take_arranged_from(path, started.first().last()), 1);
	QCOMPARE(started.size(), 1);
	QCOMPARE(second.shown_sessions().size(), 1);
	QCOMPARE(marker(second.shown_sessions().first()), QStringLiteral("d"));
	QVERIFY(!second.terminals_panel_shown());

	/* The shells came with them, and answer here. */
	bssh_session *moved = second.shown_sessions().first();
	moved->input_router()->send_text(QStringLiteral("echo after-$((6*7))\n"));
	QVERIFY(screen_shows(moved, QStringLiteral("after-42")));

	/* Nothing left in the old version, so it ends of its own accord. */
	timer.restart();
	while (old_version.state() != QProcess::NotRunning && timer.elapsed() < 20000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
		old_version.waitForFinished(50);
	}
	QCOMPARE(old_version.state(), QProcess::NotRunning);
	QCOMPARE(old_version.exitCode(), 0);
}

void handoff_test::a_move_rebuilds_each_window_once() {
	bssh_session_registry *registry = bssh_session_registry::shared();
	const auto tidy = qScopeGuard([registry] {
		while (registry->count() > 0) registry->close(registry->at(0));
	});
	QHash<QString, bssh_session *> made;
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
	                               QStringLiteral("d"), QStringLiteral("e") }) {
		bssh_session *session = registry->create(local_shell_profile());
		session->connect_to_host();
		made.insert(letter, session);
	}
	for (bssh_session *session : made) QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);

	bssh_main_window window;
	window.resize(1600, 700);
	window.show();
	bssh_main_window other;
	other.resize(800, 500);
	other.show();
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
	                               QStringLiteral("d"), QStringLiteral("e") })
		window.adopt_session(made.value(letter));
	window.layout_manager()->set_tree(bssh_layout_manager::parse_shape(
	        QStringLiteral("H(a,b,c,*d|e)"), [&made](const QString &name) { return made.value(name); }));
	QTest::qWait(300);

	int built_here = 0, built_there = 0;
	connect(window.panes(), &bssh_pane_container::panes_rebuilt, this, [&built_here] { ++built_here; });
	connect(other.panes(), &bssh_pane_container::panes_rebuilt, this, [&built_there] { ++built_there; });
	/* Every size each screen takes, as it is redrawn. */
	QHash<bssh_session *, QList<QSize>> sizes;
	for (bssh_session *session : made) {
		connect(session->emulator(), &bssh_emulator::damaged, this, [session, &sizes] {
			const QSize now(session->emulator()->screen().cols(), session->emulator()->screen().rows());
			if (sizes.value(session).value(sizes.value(session).size() - 1) != now) sizes[session].append(now);
		});
	}
	const auto move = [&](const char *what, const std::function<void()> &act) {
		built_here = built_there = 0;
		sizes.clear();
		for (bssh_session *session : made)
			sizes[session] = { QSize(session->emulator()->screen().cols(), session->emulator()->screen().rows()) };
		act();
		QTest::qWait(400);
		QVERIFY2(built_here <= 1, qPrintable(QStringLiteral("%1: this window built %2 times").arg(what).arg(built_here)));
		QVERIFY2(built_there <= 1, qPrintable(QStringLiteral("%1: the other built %2 times").arg(what).arg(built_there)));
		for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
			QString seen;
			for (const QSize &size : it.value()) seen += QStringLiteral(" %1x%2").arg(size.width()).arg(size.height());
			QVERIFY2(it.value().size() <= 2, qPrintable(QStringLiteral("%1: a terminal went through%2").arg(what, seen)));
		}
	};
	move("a split", [&] { window.drop_session_on(made.value("d"), made.value("a"), bssh_layout_manager::EDGE_LEFT); });
	move("a swap", [&] { window.drop_session_on(made.value("b"), made.value("c"), bssh_layout_manager::EDGE_MIDDLE); });
	move("a tab", [&] { window.drop_session_as_tab(made.value("a"), made.value("c"), 0); });
	move("to another window", [&] { other.adopt_session(made.value("c")); });
	move("and back", [&] { window.drop_session_on(made.value("c"), made.value("b"), bssh_layout_manager::EDGE_RIGHT); });
	QCOMPARE(window.shown_sessions().size(), 5);
}

void handoff_test::closing_a_tab_behind_rebuilds_once() {
	bssh_session_registry *registry = bssh_session_registry::shared();
	const auto tidy = qScopeGuard([registry] {
		while (registry->count() > 0) registry->close(registry->at(0));
	});
	QHash<QString, bssh_session *> made;
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
	                               QStringLiteral("d"), QStringLiteral("e") }) {
		bssh_session *session = registry->create(local_shell_profile());
		session->connect_to_host();
		made.insert(letter, session);
	}
	for (bssh_session *session : made) QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	for (const QString &letter : { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
	                               QStringLiteral("d"), QStringLiteral("e") })
		window.adopt_session(made.value(letter));
	window.layout_manager()->set_tree(bssh_layout_manager::parse_shape(
	        QStringLiteral("H(*a|d|e,V(b,c))"), [&made](const QString &name) { return made.value(name); }));
	window.panes()->focus_session(made.value(QStringLiteral("b")));
	QTest::qWait(300);

	int built = 0;
	connect(window.panes(), &bssh_pane_container::panes_rebuilt, this, [&built] { ++built; });
	QHash<bssh_session *, int> resized;
	for (bssh_session *session : made) {
		const QSize was(session->emulator()->screen().cols(), session->emulator()->screen().rows());
		connect(session->emulator(), &bssh_emulator::damaged, this, [session, was, &resized] {
			if (QSize(session->emulator()->screen().cols(), session->emulator()->screen().rows()) != was)
				++resized[session];
		});
	}

	/* The tab behind a, closed as its bar's x closes it. */
	registry->close(made.value(QStringLiteral("d")));
	QTest::qWait(400);
	QVERIFY2(built <= 1, qPrintable(QStringLiteral("the panes were built %1 times").arg(built)));
	QVERIFY2(resized.isEmpty(), "a terminal on screen changed size for a tab behind another");
	QCOMPARE(bssh_layout_manager::shape_text(window.layout_manager()->tree(),
	                                         [&made](bssh_session *session) { return made.key(session); }),
	         QStringLiteral("H(*a|e,V(b,c))"));
}

void handoff_test::each_pane_gesture_rebuilds_once() {
	bssh_session_registry *registry = bssh_session_registry::shared();
	const auto tidy = qScopeGuard([registry] {
		while (registry->count() > 0) registry->close(registry->at(0));
	});
	QList<bssh_session *> made;
	for (int i = 0; i < 4; ++i) {
		bssh_session *session = registry->create(local_shell_profile());
		session->connect_to_host();
		made.append(session);
	}
	for (bssh_session *session : made) QTRY_COMPARE(session->state(), bssh_ssh_session::CONNECTED);

	bssh_main_window window;
	window.resize(1600, 700);
	window.show();
	for (bssh_session *session : made) window.adopt_session(session);
	QTest::qWait(300);

	int built = 0;
	connect(window.panes(), &bssh_pane_container::panes_rebuilt, this, [&built] { ++built; });
	const auto once = [&](const char *what, const std::function<void()> &act) {
		built = 0;
		act();
		QTest::qWait(300);
		QVERIFY2(built <= 1, qPrintable(QStringLiteral("%1: the panes were built %2 times").arg(what).arg(built)));
		QVERIFY2(built == 1, qPrintable(QStringLiteral("%1: the panes were not built").arg(what)));
	};
	once("a new tab", [&] {
		const QList<bssh_pane_bar *> bars = window.findChildren<bssh_pane_bar *>();
		QVERIFY(!bars.isEmpty());
		emit bars.first()->new_tab_requested();
	});
	once("split with a tab", [&] { window.split_with(made.at(1)); });
	once("move a pane", [&] { window.move_pane(made.at(1), -1); });
	once("a grid", [&] { window.open_grid(2, 2); });
	once("split beside", [&] { window.split_beside_current(); });

	/* A swap with a pane of another window: each window builds once. */
	bssh_main_window other;
	other.resize(800, 500);
	other.show();
	bssh_session *theirs = registry->create(local_shell_profile());
	theirs->connect_to_host();
	QTRY_COMPARE(theirs->state(), bssh_ssh_session::CONNECTED);
	other.adopt_session(theirs);
	QTest::qWait(300);
	int built_there = 0;
	connect(other.panes(), &bssh_pane_container::panes_rebuilt, this, [&built_there] { ++built_there; });
	once("a swap with another window", [&] {
		window.drop_session_on(theirs, made.at(0), bssh_layout_manager::EDGE_MIDDLE);
	});
	QVERIFY2(built_there <= 1, qPrintable(QStringLiteral("the other window was built %1 times").arg(built_there)));
	QCOMPARE(other.shown_sessions(), QList<bssh_session *>{ made.at(0) });

	/* And with a tab behind the other window's pane. */
	bssh_session *behind = registry->create(local_shell_profile());
	behind->connect_to_host();
	QTRY_COMPARE(behind->state(), bssh_ssh_session::CONNECTED);
	other.adopt_session(behind);
	other.layout_manager()->bring_to_front(made.at(0));
	QTest::qWait(300);
	QVERIFY(other.layout_manager()->tabs(made.at(0)).contains(behind));
	built_there = 0;
	once("a swap with a tab behind", [&] {
		window.drop_session_on(behind, window.layout_manager()->visible_panes().first(),
		                       bssh_layout_manager::EDGE_MIDDLE);
	});
	QVERIFY2(built_there <= 1,
	         qPrintable(QStringLiteral("a tab behind: the other window was built %1 times").arg(built_there)));
	QVERIFY(window.shown_sessions().contains(behind));
}

void handoff_test::the_terminals_list_asks_other_processes_without_waiting() {
	runtime_dir_t runtime;
	bssh_session_registry *shared = bssh_session_registry::shared();
	const auto tidy = qScopeGuard([shared] {
		while (shared->count() > 0) shared->close(shared->at(0));
	});

	/* A process that takes the question and never answers it. */
	QDir().mkpath(bssh_handoff_point::directory());
	QFile::setPermissions(bssh_handoff_point::directory(),
	                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	const QString silent_path = bssh_handoff_point::directory() + QStringLiteral("/handoff-999999.sock");
	const int silent = ::socket(AF_UNIX, SOCK_STREAM, 0);
	QVERIFY(silent >= 0);
	const auto unlisten = qScopeGuard([silent, silent_path] {
		::close(silent);
		QFile::remove(silent_path);
	});
	sockaddr_un address;
	std::memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, QFile::encodeName(silent_path).constData(), sizeof address.sun_path - 1);
	QVERIFY(::bind(silent, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0);
	QVERIFY(::listen(silent, 1) == 0);
	/*
	 * Its queue filled, as a wedged process's fills: a connect that waited
	 * would wait for ever. Connections are queued until accepted, closed
	 * or not, so these are held until the end.
	 */
	QList<int> queued;
	const auto unqueue = qScopeGuard([&queued] {
		for (int fd : queued) ::close(fd);
	});
	for (int i = 0; i < 8; ++i) {
		/* fcntl rather than SOCK_NONBLOCK, which macOS does not have. */
		const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
		if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
			::close(fd);
			break;
		}
		queued.append(fd);
	}
	QVERIFY2(!queued.isEmpty() && queued.size() < 8, "the silent process's queue did not fill");

	/* And one with room left, which takes the question and sits on it. */
	const QString slow_path = bssh_handoff_point::directory() + QStringLiteral("/handoff-999998.sock");
	const int slow = ::socket(AF_UNIX, SOCK_STREAM, 0);
	QVERIFY(slow >= 0);
	const auto unlisten_slow = qScopeGuard([slow, slow_path] {
		::close(slow);
		QFile::remove(slow_path);
	});
	sockaddr_un slow_address = address;
	std::memset(slow_address.sun_path, 0, sizeof slow_address.sun_path);
	std::strncpy(slow_address.sun_path, QFile::encodeName(slow_path).constData(),
	             sizeof slow_address.sun_path - 1);
	QVERIFY(::bind(slow, reinterpret_cast<sockaddr *>(&slow_address), sizeof slow_address) == 0);
	QVERIFY(::listen(slow, 64) == 0);

	/* And one that answers: the old version's four terminals. */
	QProcess old_version;
	old_version.start(QCoreApplication::applicationFilePath(), { QStringLiteral("--handoff-arranged") });
	const auto stop = qScopeGuard([&old_version] {
		old_version.kill();
		old_version.waitForFinished(5000);
	});
	QVERIFY(old_version.waitForStarted(10000));
	QElapsedTimer timer;
	timer.start();
	while (!old_version.canReadLine() && timer.elapsed() < 30000 && old_version.state() != QProcess::NotRunning)
		old_version.waitForReadyRead(100);
	const QString path = QString::fromLocal8Bit(old_version.readLine()).trimmed();
	QVERIFY2(path.endsWith(QStringLiteral(".sock")), qPrintable(path));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	timer.restart();
	window.set_terminals_panel_shown(true);
	window.refresh_terminals_panel();
	QVERIFY2(timer.elapsed() < 250,
	         qPrintable(QStringLiteral("the window waited %1 ms for other processes").arg(timer.elapsed())));

	const auto from_there = [&window, &path] {
		int count = 0;
		for (const bssh_terminal_entry_t &entry : window.terminals_panel()->entries())
			if (entry.place == bssh_terminal_entry_t::OTHER_PROCESS && entry.path == path) ++count;
		return count;
	};
	QTRY_COMPARE_WITH_TIMEOUT(from_there(), 4, 10000);
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

	/*
	 * And not last time's hosts: main() restores on any plain start, so
	 * New Window came up with whatever window had closed last instead of
	 * a shell, until it said otherwise.
	 */
	QVERIFY(started.last().contains(QStringLiteral("--no-restore")));

	/*
	 * New Window Grid: asked how many, and a program of its own opening
	 * that many (the holder, 2026-11-02). Three across, two down, of a
	 * local shell, so nothing names a host.
	 */
	QAction *new_grid = nullptr;
	for (QAction *action : window.findChildren<QAction *>())
		if (action->text() == QStringLiteral("New Window &Grid...")) new_grid = action;
	QVERIFY(new_grid);
	bool answered = false;
	QTimer::singleShot(0, [&answered] {
		QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog || dialog->objectName() != QStringLiteral("grid_dialog")) return;
		dialog->findChild<QSpinBox *>(QStringLiteral("grid_columns"))->setValue(3);
		dialog->findChild<QSpinBox *>(QStringLiteral("grid_rows"))->setValue(2);
		dialog->findChild<QComboBox *>(QStringLiteral("grid_profile"))->setCurrentIndex(0);
		answered = true;
		dialog->accept();
	});
	new_grid->trigger();
	QVERIFY2(answered, "no grid dialog came up");
	QCOMPARE(started.size(), 3);
	const QStringList grid = { QStringLiteral("--grid"), QStringLiteral("3x2"),
	                           QStringLiteral("--no-restore") };
	QCOMPARE(started.last(), grid);

	bssh_main_window::set_process_starter(nullptr);
	for (bssh_session *session : sessions) registry->close(session);
}

/*
 * A start brings back every window, each a process again (sec 8.4): this
 * one takes the first saved layout and starts the program for the others,
 * which is what the holder's windows being processes costs layout.ini. A
 * window still open elsewhere holds its layout and is left alone.
 */
void handoff_test::a_start_brings_back_every_window_not_open_elsewhere() {
	QList<QStringList> started;
	bssh_main_window::set_process_starter([&started](const QString &, const QStringList &arguments) {
		started.append(arguments);
		return true;
	});

	bssh_profile_store *store = bssh_profile_store::shared();
	bssh_profile_t host;
	host.name = QStringLiteral("alpha");
	host.host = QStringLiteral("alpha.example");
	host.user = QStringLiteral("nabbe");
	const int id = store->save(host);
	QVERIFY(id > 0);

	bssh_profile_store::layout_state_t layout;
	layout.profile_ids = { id };
	layout.geometry = QRect(40, 30, 700, 500);
	const QString path = store->directory() + QStringLiteral("/test.db");

	/* Saved by windows of two other programs, one of them still open. */
	{
		bssh_profile_store closed_one(path);
		bssh_profile_store closed_two(path);
		closed_one.use_window_layouts();
		closed_two.use_window_layouts();
		QVERIFY(closed_one.claim_layout(0));
		QVERIFY(closed_two.claim_layout(1));
		QVERIFY(closed_one.save_layout(layout));
		QVERIFY(closed_two.save_layout(layout));
	}
	bssh_profile_store still_open(path);
	still_open.use_window_layouts();
	QVERIFY(still_open.claim_layout(2));
	QVERIFY(still_open.save_layout(layout));

	{
		bssh_main_window window;
		window.restore_windows();
		QVERIFY2(window.has_sessions(), "this window did not take the first layout");
		QCOMPARE(window.size(), QSize(700, 500));
		const QStringList expected = { QStringLiteral("--restore-window"), QStringLiteral("1") };
		QCOMPARE(started.size(), 1);
		QCOMPARE(started.first(), expected);
		for (bssh_session *session : window.registry()->sessions()) {
			window.registry()->close(session);
		}
	}

	/* What this test wrote is the suite's shared store's, so it goes. */
	store->release_layout();
	still_open.release_layout();
	QVERIFY(store->claim_layout(1));
	store->release_layout();
	store->remove(id);
	bssh_main_window::set_process_starter(nullptr);
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
