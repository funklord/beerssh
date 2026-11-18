#include "fuzznet_session_test.h"

#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#ifdef BSSH_FUZZNET
#include "model/profile.h"
#include "model/session.h"
#include "platform/secret_store.h"
#include "ssh/fuzznet_session.h"
#include "term/emulator.h"
#include "ui/host_editor.h"

#include <QCheckBox>
#include <QLineEdit>
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

/* A store held in memory, unlocked, for the device image a host keeps. */
class memory_store_t : public bssh_secret_store {
public:
	QString backend_name() const override { return QStringLiteral("memory"); }
	bool is_hardware_backed() const override { return false; }
	bool is_unlocked() const override { return true; }
	bool needs_passphrase() const override { return false; }
	bool unlock(const QString &) override { return true; }
	void lock() override {}
	bool store(const QString &id, const QString &secret) override {
		m_secrets.insert(id, secret);
		return true;
	}
	QString load(const QString &id) const override { return m_secrets.value(id); }
	bool erase(const QString &id) override { return m_secrets.remove(id) > 0; }

private:
	QHash<QString, QString> m_secrets;
};

struct store_left_as_found_t {
	~store_left_as_found_t() { bssh_secret_store::set_instance(nullptr); }
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

void fuzznet_session_test::a_fuzznet_host_keeps_its_shell_through_a_drop() {
#ifndef BSSH_FUZZNET
	QSKIP("fuzznet is not built here");
#else
	/*
	 * The whole of sec 5.6 over fuzznet, as a person meets it: a host
	 * marked as reached over fuzznet, its device image in the secret
	 * store, a shell through beersshd's helper -- and after a drop, the
	 * same shell, with what it wrote meanwhile.
	 */
	QString sshd, helper;
	const QString missing = binaries(&sshd, &helper);
	if (!missing.isEmpty()) QSKIP(qPrintable(missing));

	QTemporaryDir runtime;
	QVERIFY(runtime.isValid());
	struct environment_t {
		QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
		QByteArray shell = qgetenv("SHELL");
		~environment_t() {
			qputenv("XDG_RUNTIME_DIR", runtime);
			qputenv("SHELL", shell);
		}
	} restore;
	qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime.path()));
	qputenv("SHELL", "/bin/sh");

	server_t server;
	QVERIFY2(server.start(sshd, helper), "beersshd --fuzznet did not pair and serve");

	memory_store_t store;
	const store_left_as_found_t put_back;
	bssh_secret_store::set_instance(&store);

	bssh_profile_t profile;
	profile.id = 4242;
	profile.name = QStringLiteral("over fuzznet");
	profile.host = QStringLiteral("127.0.0.1");
	profile.port = server.port;
	profile.fuzznet = true;
	QVERIFY(store.store(bssh_profile_fuzznet_key(profile.id), server.image));
	QVERIFY2(profile.resume_session(), "a fuzznet host does not keep its session");

	bssh_session session(profile);
	const auto shows = [&session](const QRegularExpression &pattern, int ms) {
		QElapsedTimer timer;
		timer.start();
		QRegularExpressionMatch match;
		while (timer.elapsed() < ms) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
			match = pattern.match(session.emulator()->screen().to_text());
			if (match.hasMatch()) return match;
			QTest::qWait(20);
		}
		return match;
	};
	const auto type = [&session](const QString &text) {
		session.emulator()->send_paste(text);
		session.emulator()->send_char(U'\r', 0);
	};
	struct hang_up_t {
		bssh_session &session;
		~hang_up_t() {
			session.disconnect_from_host();
			QTest::qWait(300);
		}
	} hang_up{ session };

	session.connect_to_host();

	QRegularExpressionMatch first;
	for (int tries = 0; tries < 10 && !first.hasMatch(); ++tries) {
		type(QStringLiteral("KEPT=$$; echo \"kept\"\"=$KEPT.\""));
		first = shows(QRegularExpression(QStringLiteral("kept=(\\d+)\\.")), 2000);
	}
	QVERIFY2(first.hasMatch(), qPrintable(session.emulator()->screen().to_text()));
	const QString shell = first.captured(1);

	/* Written while away, and found on the return, from the same shell. */
	type(QStringLiteral("(sleep 2; echo \"while\"\"-away:$KEPT.\") &"));
	QTest::qWait(300);
	session.reconnect();

	QVERIFY2(shows(QRegularExpression(QStringLiteral("while-away:") + shell + QStringLiteral("\\.")), 15000)
	                 .hasMatch(),
	         qPrintable(session.emulator()->screen().to_text()));
	type(QStringLiteral("echo \"sa\"\"me:$$.\""));
	QVERIFY2(shows(QRegularExpression(QStringLiteral("same:") + shell + QStringLiteral("\\.")), 10000)
	                 .hasMatch(),
	         qPrintable(session.emulator()->screen().to_text()));
#endif
}

void fuzznet_session_test::a_fuzznet_host_without_its_device_says_so() {
#ifndef BSSH_FUZZNET
	QSKIP("fuzznet is not built here");
#else
	/* A store that holds no image for it: said, not a hang or an SSH attempt. */
	memory_store_t store;
	const store_left_as_found_t put_back;
	bssh_secret_store::set_instance(&store);

	bssh_profile_t profile;
	profile.id = 4243;
	profile.host = QStringLiteral("127.0.0.1");
	profile.port = 9;
	profile.fuzznet = true;
	bssh_session session(profile);
	QSignalSpy failed(&session, &bssh_session::failed);
	session.connect_to_host();
	QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 5000);
	QVERIFY2(failed.at(0).at(0).toString().contains(QLatin1String("not in the secret store")),
	         qPrintable(failed.at(0).at(0).toString()));
	session.disconnect_from_host();
#endif
}

void fuzznet_session_test::the_editor_takes_a_device_image_and_keeps_it_out_of_the_profile() {
#ifndef BSSH_FUZZNET
	QSKIP("fuzznet is not built here");
#else
	bssh_host_editor editor;
	bssh_profile_t host;
	host.host = QStringLiteral("10.0.0.7");
	editor.set_profile(host);
	QCheckBox *over = editor.findChild<QCheckBox *>(QStringLiteral("host_fuzznet"));
	QLineEdit *image = editor.findChild<QLineEdit *>(QStringLiteral("host_fuzznet_image"));
	QVERIFY(over && image);
	QCOMPARE(image->echoMode(), QLineEdit::Password);

	/* Not asked for: nothing taken, whatever is in the field. */
	image->setText(QStringLiteral("FZD1:00ff"));
	QVERIFY(!editor.profile().fuzznet);
	QVERIFY(editor.fuzznet_image().isEmpty());

	/* Asked for, with something that is not an image: refused, no flag. */
	over->setChecked(true);
	image->setText(QStringLiteral("hello"));
	QVERIFY(editor.fuzznet_image().isEmpty());
	QVERIFY2(!editor.profile().fuzznet, "a host over fuzznet with no device to speak as");

	/* An image: handed to the caller for the secret store, flagged in the row. */
	image->setText(QStringLiteral("  FZD1:00ff  "));
	QCOMPARE(editor.fuzznet_image(), QStringLiteral("FZD1:00ff"));
	QVERIFY(editor.profile().fuzznet);

	/* An edit of a host that has one kept: still over fuzznet with the field empty. */
	host.fuzznet = true;
	editor.set_profile(host);
	QVERIFY(over->isChecked());
	QVERIFY(image->text().isEmpty());
	QVERIFY(editor.profile().fuzznet);
	over->setChecked(false);
	QVERIFY(!editor.profile().fuzznet);
#endif
}
