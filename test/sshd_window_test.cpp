#include "sshd_window_test.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <thread>

#include "sshd/sshd_platform.h"
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
	QTcpSocket probe;
	probe.connectToHost(QHostAddress::LocalHost, quint16(window.server().port()));
	QVERIFY(!probe.waitForConnected(2000));

	/* The screen catches up when Qt does. */
	QVERIFY(platform.serving);
	QTRY_VERIFY(!platform.serving);
	QVERIFY(child<QSpinBox>(window, "sshd-port")->isEnabled());
	QVERIFY(child<QPlainTextEdit>(window, "sshd-log")->toPlainText().contains(QStringLiteral("six hours")));
}

void sshd_window_test::losing_wifi_stops_and_a_new_address_restarts() {
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

	/* Another address of this machine's, standing in for a new network. */
	platform.address = QHostAddress(QStringLiteral("127.0.0.2"));
	window.follow_address();
	QVERIFY(window.serving());
	QCOMPARE(window.server().config().bind_address, QByteArray("127.0.0.2"));

	platform.address = QHostAddress();
	window.follow_address();
	QVERIFY(!window.serving());
	QVERIFY(!platform.serving);
	QVERIFY(!child<QPushButton>(window, "sshd-toggle")->isEnabled());
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
