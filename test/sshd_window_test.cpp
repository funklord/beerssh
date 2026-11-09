#include "sshd_window_test.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSettings>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <thread>

#include "sshd/sshd_platform.h"
#include "sshd/sshd_demo.h"
#include "sshd/sshd_keeper.h"
#include "sshd/sshd_remote.h"
#include "sshd/sshd_window.h"

namespace {

class fake_platform : public bssh_sshd_platform {
public:
	explicit fake_platform(const QString &directory)
	    : m_root(directory + QStringLiteral("/root")), m_data(directory + QStringLiteral("/data")) {
		QDir().mkpath(m_root);
	}

	QString storage_root() const override { return m_root; }
	bool has_storage_access() const override { return access; }
	void request_storage_access() override { ++requests; }
	QHostAddress listen_address() const override { return address; }
	QString data_directory() const override { return m_data; }

	void request_notifications() override { ++notification_requests; }

	bool switched_on() const override { return on; }
	void set_switched_on(bool now) override { on = now; }

	bool battery_restricted() const override { return restricted; }
	void request_battery_exemption() override { ++battery_requests; }

	void start_server_process() override { ++process_starts; }
	void stop_server_process() override { ++process_stops; }

	void set_serving(bool now, const QString &status) override {
		serving = now;
		last_status = status;
	}

	/* As Android does it: the hook on its own thread, the signal queued to Qt's. */
	void time_out_from(std::thread &thread) {
		thread = std::thread([this] { run_stop_hook(); });
		thread.join();
		QMetaObject::invokeMethod(this, [this] { emit time_limit_reached(); }, Qt::QueuedConnection);
	}

	bool access = true;
	QHostAddress address = QHostAddress(QHostAddress::LocalHost);
	int requests = 0;
	int notification_requests = 0;
	bool serving = false;
	bool on = false;
	bool restricted = false;
	int battery_requests = 0;
	int process_starts = 0;
	int process_stops = 0;
	QString last_status;

private:
	QString m_root;
	QString m_data;
};

/* A port nothing is using right now: bound by the system, then let go. */
int free_port() {
	QTcpServer probe;
	probe.listen(QHostAddress::LocalHost, 0);
	return probe.serverPort();
}

template <typename widget_t>
widget_t *child(QWidget &window, const char *name) {
	return window.findChild<widget_t *>(QString::fromLatin1(name));
}

} /* namespace */

void sshd_window_test::without_access_it_asks_and_does_not_start() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	platform.access = false;

	bssh_sshd_window window(&platform);
	QVERIFY(!child<QWidget>(window, "sshd-access")->isHidden());
	QVERIFY(!child<QPushButton>(window, "sshd-toggle")->isEnabled());

	child<QPushButton>(window, "sshd-grant")->click();
	QCOMPARE(platform.requests, 1);

	/* Called directly too: a disabled button is not the only door. */
	window.start();
	QVERIFY(!window.serving());
	QVERIFY(!platform.serving);

	/* Granted in Settings; coming back is what the window looks again on. */
	platform.access = true;
	window.refresh();
	QVERIFY(child<QWidget>(window, "sshd-access")->isHidden());
	QVERIFY(child<QPushButton>(window, "sshd-toggle")->isEnabled());
}

void sshd_window_test::it_serves_the_platform_root_on_the_platform_address() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());

	bssh_sshd_window window(&platform);
	child<QSpinBox>(window, "sshd-port")->setValue(free_port());
	child<QPushButton>(window, "sshd-toggle")->click();
	QVERIFY(window.serving());

	QCOMPARE(QFile::decodeName(window.server().config().root), platform.storage_root());
	QCOMPARE(window.server().config().bind_address, QByteArray("127.0.0.1"));

	/* The host key and the keys stay in the data directory, never under the root served. */
	QVERIFY(window.server().config().host_key_path.startsWith(platform.data_directory()));
	QVERIFY(QFile::exists(window.server().config().host_key_path));
	QVERIFY(!window.server().config().host_key_path.startsWith(platform.storage_root()));

	QVERIFY(platform.serving);
	QVERIFY(platform.last_status.contains(QStringLiteral("127.0.0.1")));

	/* Asked at Start, which is when the notification first has something to say. */
	QCOMPARE(platform.notification_requests, 1);
	QVERIFY(!child<QSpinBox>(window, "sshd-port")->isEnabled());
	QCOMPARE(child<QLineEdit>(window, "sshd-fingerprint")->text(), window.server().host_key_fingerprint());
	QVERIFY(window.server().host_key_fingerprint().startsWith(QStringLiteral("SHA256:")));

	/*
	 * And the page still fits a phone. The fingerprint as a label set the
	 * minimum width of everything under the scroll area, and on a Fold's
	 * cover screen the Stop button went past the edge. 360 is the narrow
	 * end of phone widths in dp, which is what Qt's pixels are there.
	 */
	auto *scroll = window.findChild<QScrollArea *>();
	QVERIFY(scroll && scroll->widget());
	QVERIFY2(scroll->widget()->minimumSizeHint().width() <= 360,
	         qPrintable(QString::number(scroll->widget()->minimumSizeHint().width())));

	child<QPushButton>(window, "sshd-toggle")->click();
	QVERIFY(!window.serving());
	QVERIFY(!platform.serving);
}

void sshd_window_test::the_time_limit_stops_the_server_and_says_why() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());

	bssh_sshd_window window(&platform);
	child<QSpinBox>(window, "sshd-port")->setValue(free_port());
	window.start();
	QVERIFY(window.serving());

	/*
	 * Stopped by the hook alone, before Qt's thread has run anything: on a
	 * phone that thread may not run again until the app is reopened, and
	 * Android freezes the process in the meantime.
	 */
	std::thread android_thread;
	platform.time_out_from(android_thread);
	QVERIFY(!window.serving());
	/*
	 * Not served is no banner, not a refused connect: on macOS a connect
	 * to the port the server just closed was once reported made, while
	 * lsof showed it CLOSED and nothing listening anywhere (CI run
	 * 42636780896).
	 */
	QTcpSocket probe;
	probe.connectToHost(QHostAddress::LocalHost, quint16(window.server().port()));
	if (probe.waitForConnected(2000)) {
		probe.waitForReadyRead(2000);
		QVERIFY2(!probe.readAll().startsWith("SSH-2.0-"), "the server answered after it was stopped");
	}

	/* The screen catches up when Qt does. */
	QVERIFY(platform.serving);
	QTRY_VERIFY(!platform.serving);
	QVERIFY(child<QSpinBox>(window, "sshd-port")->isEnabled());
	QVERIFY(child<QPlainTextEdit>(window, "sshd-log")->toPlainText().contains(QStringLiteral("would not keep the file server running")));
}

void sshd_window_test::losing_wifi_waits_and_a_new_address_restarts() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());

	bssh_sshd_window window(&platform);
	child<QSpinBox>(window, "sshd-port")->setValue(free_port());
	window.start();
	QVERIFY(window.serving());

	/* Unchanged: nothing happens, and in particular no restart. */
	const int port = window.server().port();
	window.follow_address();
	QVERIFY(window.serving());
	QCOMPARE(window.server().port(), port);

	/*
	 * Another address of this machine's, standing in for a new network.
	 * Linux answers on all of 127/8; macOS only on 127.0.0.1 unless an
	 * alias is added, which a test has no business doing.
	 */
	{
		QTcpServer second;
		if (!second.listen(QHostAddress(QStringLiteral("127.0.0.2")), 0))
			QSKIP("127.0.0.2 is not this machine's address here");
	}
	platform.address = QHostAddress(QStringLiteral("127.0.0.2"));
	window.follow_address();
	QVERIFY(window.serving());
	QCOMPARE(window.server().config().bind_address, QByteArray("127.0.0.2"));

	/*
	 * Wi-Fi gone: still on, because the user switched it on and it is to
	 * be reachable when the phone is back (sec 19.6). Stop stays offered.
	 */
	platform.address = QHostAddress();
	window.follow_address();
	QVERIFY(window.serving());
	QVERIFY(platform.serving);
	QVERIFY(child<QPushButton>(window, "sshd-toggle")->isEnabled());

	/* And back on another network: followed there. */
	platform.address = QHostAddress(QStringLiteral("127.0.0.1"));
	window.follow_address();
	QVERIFY(window.serving());
	QCOMPARE(window.server().config().bind_address, QByteArray("127.0.0.1"));

	child<QPushButton>(window, "sshd-toggle")->click();
	QVERIFY(!window.serving());
	QVERIFY(!platform.serving);
}

void sshd_window_test::saved_keys_are_counted_by_the_login_parse() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	bssh_sshd_window window(&platform);

	const QByteArray key =
	        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHbnA/tFqV7FQBuhHUrneXu68d4fql+jwb1LshPNYTyw test";
	const QByteArray text = "# a comment\n" + key + "\n"
	                        "from=\"10.0.0.1\" " + key + "\n"
	                        "not a key at all\n";

	child<QPlainTextEdit>(window, "sshd-keys")->setPlainText(QString::fromUtf8(text));
	child<QPushButton>(window, "sshd-save-keys")->click();

	QFile saved(window.authorized_keys_path());
	QVERIFY(saved.open(QIODevice::ReadOnly));
	QCOMPARE(saved.readAll(), text);
	QVERIFY(window.authorized_keys_path().startsWith(platform.data_directory()));

	/* One, not two: the restricted line is skipped at login, so it is not counted here. */
	QCOMPARE(bssh_sshd::usable_keys(text), 1);
	QVERIFY2(child<QLabel>(window, "sshd-key-count")->text().startsWith(QStringLiteral("1 ")),
	         qPrintable(child<QLabel>(window, "sshd-key-count")->text()));
}

void sshd_window_test::the_keys_box_follows_the_file_but_not_over_edits() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	bssh_sshd_window window(&platform);
	auto *keys = child<QPlainTextEdit>(window, "sshd-keys");

	/* Changed from outside -- adb, a file manager, another copy of the app. */
	const QByteArray key =
	        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHbnA/tFqV7FQBuhHUrneXu68d4fql+jwb1LshPNYTyw test\n";
	QDir().mkpath(platform.data_directory());
	{
		QFile file(window.authorized_keys_path());
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write(key);
	}
	window.reload_keys();
	QCOMPARE(keys->toPlainText(), QString::fromUtf8(key));
	QVERIFY(child<QLabel>(window, "sshd-key-count")->text().startsWith(QStringLiteral("1 ")));

	/* An unsaved edit is the user's, and the file does not replace it. */
	keys->setPlainText(QStringLiteral("half-typed"));
	keys->document()->setModified(true);
	window.reload_keys();
	QCOMPARE(keys->toPlainText(), QStringLiteral("half-typed"));
}

/*
 * Only Start and Stop change whether it is switched on, and a window
 * opened while it is on serves again -- at once, or once Wi-Fi is back,
 * since an app Android ended or a phone that rebooted should not need
 * Start pressed a second time to be reachable (sec 19.6).
 */
void sshd_window_test::switched_on_lasts_and_serves_again_when_it_can() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	const int port = free_port();

	{
		bssh_sshd_window window(&platform);
		child<QSpinBox>(window, "sshd-port")->setValue(port);
		QVERIFY(!platform.on);
		child<QPushButton>(window, "sshd-toggle")->click();
		QVERIFY(window.serving());
		QVERIFY(platform.on);
	}

	/* A new window, as after Android ended the app: on, so serving. */
	{
		bssh_sshd_window window(&platform);
		QVERIFY(window.serving());

		/* The system stopping it leaves it switched on. */
		std::thread android_thread;
		platform.time_out_from(android_thread);
		QTRY_VERIFY(!platform.serving);
		QVERIFY(platform.on);
	}

	/* Opened with no Wi-Fi: waits, then serves once an address comes. */
	platform.address = QHostAddress();
	{
		bssh_sshd_window window(&platform);
		QVERIFY(!window.serving());
		platform.address = QHostAddress(QHostAddress::LocalHost);
		window.follow_address();
		QVERIFY(window.serving());

		child<QPushButton>(window, "sshd-toggle")->click();
		QVERIFY(!window.serving());
		QVERIFY(!platform.on);
	}

	/* Switched off: a new window stays off. */
	bssh_sshd_window window(&platform);
	QVERIFY(!window.serving());
	window.follow_address();
	QVERIFY(!window.serving());
}

/*
 * The window over the server in another process, as on Android (sec
 * 19.6): Start and Stop go to the process, and what is shown comes from
 * the two files that process writes. Here the "process" is a keeper in
 * this one, reporting the way the :server process does, and the process
 * check is told any live process will do.
 */
void sshd_window_test::the_remote_window_shows_what_the_server_process_reports() {
	/* Android's alone (sshd/main_android.cpp), and it asks /proc whether the process lives. */
	if (!QFileInfo::exists(QStringLiteral("/proc/self/cmdline")))
		QSKIP("the remote window is Android's and reads /proc, which this system lacks");
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	bssh_sshd_remote::set_process_suffix(QByteArray());
	QSettings().setValue(QStringLiteral("port"), free_port());

	auto *remote = new bssh_sshd_remote(&platform);
	bssh_sshd_window window(&platform, remote);
	QVERIFY(!window.serving());
	QCOMPARE(child<QPushButton>(window, "sshd-toggle")->text(), QStringLiteral("Start"));

	child<QPushButton>(window, "sshd-toggle")->click();
	QVERIFY(platform.on);
	QCOMPARE(platform.process_starts, 1);
	QVERIFY(window.serving());
	QVERIFY(child<QLabel>(window, "sshd-status")->text().contains(QStringLiteral("Starting")));

	/* The process comes up, and serves because it is switched on. */
	auto server = std::make_unique<bssh_sshd_keeper>(&platform);
	bssh_sshd_remote::report_from(server.get(), &platform);
	server->start_if_switched_on();
	QVERIFY(server->serving());

	QTRY_VERIFY(child<QLabel>(window, "sshd-status")->text().startsWith(QStringLiteral("Serving")));
	QCOMPARE(child<QLineEdit>(window, "sshd-fingerprint")->text(), server->fingerprint());
	QTRY_VERIFY(child<QPlainTextEdit>(window, "sshd-log")->toPlainText().contains(QStringLiteral("Listening on")));
	QVERIFY(!child<QSpinBox>(window, "sshd-port")->isEnabled());

	/* Stop goes to the process, which ends. */
	child<QPushButton>(window, "sshd-toggle")->click();
	QVERIFY(!platform.on);
	QCOMPARE(platform.process_stops, 1);
	server.reset();
	QTRY_VERIFY(!child<QLabel>(window, "sshd-status")->text().startsWith(QStringLiteral("Serving")));
	QCOMPARE(child<QPushButton>(window, "sshd-toggle")->text(), QStringLiteral("Start"));
	QCOMPARE(platform.process_starts, 1);

	bssh_sshd_remote::set_process_suffix(QByteArrayLiteral(":server"));
}

/*
 * Switched on and the process gone -- Android ended it -- is asked to
 * start again, and a status naming a process that is not alive is not
 * believed.
 */
void sshd_window_test::a_dead_server_process_is_started_again_while_switched_on() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	platform.on = true;
	QDir().mkpath(platform.data_directory());

	/* A status left behind by a process that is gone: pid 0 is nobody. */
	QFile stale(bssh_sshd_remote::status_path(&platform));
	QVERIFY(stale.open(QIODevice::WriteOnly));
	stale.write("pid=0\nserving=1\nstatus=Serving /x at 1.2.3.4, port 1 -- 0 connection(s)\n");
	stale.close();

	bssh_sshd_remote remote(&platform);
	QCOMPARE(platform.process_starts, 1);
	QCOMPARE(remote.status_line(), QStringLiteral("Starting..."));

	/* Not asked again while that start is on its way. */
	remote.follow();
	QCOMPARE(platform.process_starts, 1);
}

/*
 * The battery line (sec 5.4): shown while the platform may pause the app,
 * asking only from its button, and gone once allowed -- looked at again
 * when the app comes back from the system's dialog, as refresh() does.
 */
void sshd_window_test::the_battery_line_shows_until_the_app_is_allowed() {
	QTemporaryDir dir;
	fake_platform platform(dir.path());
	platform.restricted = true;

	bssh_sshd_window window(&platform);
	QWidget *line = window.findChild<QWidget *>(QStringLiteral("sshd-battery"));
	QVERIFY(line);
	QVERIFY(!line->isHidden());
	QCOMPARE(platform.battery_requests, 0);

	child<QPushButton>(window, "sshd-battery-allow")->click();
	QCOMPARE(platform.battery_requests, 1);

	platform.restricted = false;
	window.refresh();
	QVERIFY(line->isHidden());
}

/*
 * The store screenshots' window (main_android.cpp, the play-store skill):
 * made-up content throughout -- the demo's address, host key and two
 * keys that parse -- from a data directory of its own, so the listing
 * can never show the user's keys or log.
 */
void sshd_window_test::the_store_demo_shows_only_made_up_content() {
	bssh_sshd_demo_platform platform;
	auto *control = new bssh_sshd_demo_control;
	bssh_sshd_window window(&platform, control);
	control->tell();

	QVERIFY(window.serving());
	QVERIFY(child<QLabel>(window, "sshd-status")->text().contains(QStringLiteral("192.168.1.23")));
	QVERIFY(child<QLineEdit>(window, "sshd-fingerprint")->text().startsWith(QStringLiteral("SHA256:")));
	QCOMPARE(child<QLabel>(window, "sshd-key-count")->text(), QStringLiteral("2 usable key(s)."));
	QVERIFY(child<QPlainTextEdit>(window, "sshd-log")->toPlainText().contains(QStringLiteral("started SFTP")));
	QVERIFY(window.authorized_keys_path().startsWith(platform.data_directory()));
}
