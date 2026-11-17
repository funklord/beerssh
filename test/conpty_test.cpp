#include "conpty_test.h"

#include <QSignalSpy>
#include <QtTest>

#include <memory>

#include "ssh/ssh_session.h"

namespace {

struct shell_t {
	std::unique_ptr<bssh_ssh_session> session;
	QByteArray seen;
	int exit_status = -12345;
	bool exited = false;

	explicit shell_t(const QStringList &command = QStringList()) {
		session.reset(bssh_ssh_session::create_local(QString(), QString(), nullptr));
		if (!command.isEmpty()) session->set_command(command);
		QObject::connect(session.get(), &bssh_ssh_session::bytes_received,
		                 [this](const QByteArray &bytes) { seen += bytes; });
		QObject::connect(session.get(), &bssh_ssh_session::shell_exited, [this](int status) {
			exit_status = status;
			exited = true;
		});
		session->connect_to_host(QString(), 0, QString());
	}
};

}  // namespace

void conpty_test::a_command_prints_and_its_exit_status_arrives() {
	shell_t shell({ QStringLiteral("cmd.exe"), QStringLiteral("/d"), QStringLiteral("/c"),
	                QStringLiteral("echo beerssh-conpty-ok& exit 3") });
	QCOMPARE(shell.session->state(), bssh_ssh_session::CONNECTED);
	shell.session->open_shell(80, 24);
	QTRY_VERIFY_WITH_TIMEOUT(shell.exited, 20000);
	QVERIFY2(shell.seen.contains("beerssh-conpty-ok"), shell.seen.constData());
	QCOMPARE(shell.exit_status, 3);
	QTRY_COMPARE(shell.session->state(), bssh_ssh_session::DISCONNECTED);
}

void conpty_test::the_shell_reads_what_is_typed_and_exits_when_told() {
	shell_t shell;
	shell.session->open_shell(80, 24);
	shell.session->send("echo typed-%OS%-here\r");
	QTRY_VERIFY2_WITH_TIMEOUT(shell.seen.contains("typed-Windows_NT-here"), shell.seen.constData(), 20000);
	shell.session->send("exit 7\r");
	QTRY_VERIFY_WITH_TIMEOUT(shell.exited, 20000);
	QCOMPARE(shell.exit_status, 7);
}

void conpty_test::a_resize_reaches_the_console() {
	shell_t shell;
	shell.session->open_shell(80, 24);
	shell.session->resize(123, 41);
	shell.session->send("mode con\r");
	QTRY_VERIFY2_WITH_TIMEOUT(shell.seen.contains("123"), shell.seen.constData(), 20000);
	shell.session->send("exit\r");
	QTRY_VERIFY_WITH_TIMEOUT(shell.exited, 20000);
}

void conpty_test::closing_a_running_shell_ends_it() {
	shell_t shell;
	shell.session->open_shell(80, 24);
	QTRY_VERIFY_WITH_TIMEOUT(!shell.seen.isEmpty(), 20000);
	QElapsedTimer timer;
	timer.start();
	shell.session->disconnect_from_host();
	QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
	QCOMPARE(shell.session->state(), bssh_ssh_session::DISCONNECTED);
}

void conpty_test::a_program_that_does_not_exist_fails_with_a_reason() {
	shell_t shell({ QStringLiteral("beerssh-no-such-program.exe") });
	QSignalSpy failed(shell.session.get(), &bssh_ssh_session::failed);
	shell.session->open_shell(80, 24);
	QTRY_COMPARE(failed.size(), 1);
	const QString why = failed.first().first().toString();
	QVERIFY2(why.contains(QStringLiteral("beerssh-no-such-program")), qPrintable(why));
	QCOMPARE(shell.session->state(), bssh_ssh_session::FAILED);
}
