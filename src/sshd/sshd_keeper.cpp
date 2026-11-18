#include "sshd/sshd_keeper.h"
#include "sshd/fuzznet_pair.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QTimer>

#include "sshd/sshd_platform.h"

namespace {

/*
 * Above 1024, which an app may bind without privilege, and the number
 * Termux's sshd made familiar on phones.
 */
const int DEFAULT_PORT = 8022;

const int ADDRESS_POLL_MS = 5000;

} /* namespace */

QString bssh_sshd_keeper::authorized_keys_path(const bssh_sshd_platform *platform) {
	return platform->data_directory() + QStringLiteral("/authorized_keys");
}

QString bssh_sshd_keeper::password_path(const bssh_sshd_platform *platform) {
	return platform->data_directory() + QStringLiteral("/password");
}

bssh_sshd_keeper::bssh_sshd_keeper(bssh_sshd_platform *platform, QObject *parent)
    : bssh_sshd_control(parent), m_platform(platform) {
	/*
	 * Stamped on the server's thread, when it happened, and only then
	 * queued to this one: on Android Qt's thread may not run while the app
	 * is in the background, so a line stamped on arrival said when the
	 * user next opened the app -- a night's Wi-Fi drop and return all read
	 * as the same second (sec 19.6). Direct, then queued; stop() joins the
	 * server's threads before this goes.
	 */
	connect(&m_server, &bssh_sshd::logged, this, [this](const QString &line) {
		const QTime when = QTime::currentTime();
		QMetaObject::invokeMethod(this, [this, when, line] { emit logged(when, line); },
		                          Qt::QueuedConnection);
	}, Qt::DirectConnection);
	connect(&m_server, &bssh_sshd::connections_changed, this, &bssh_sshd_keeper::report);
	/* On the connection's thread: the platform's call takes any. */
	connect(&m_server, &bssh_sshd::file_changed, this,
	        [this](const QString &path) { m_platform->file_changed(path); }, Qt::DirectConnection);
	connect(&m_server, &bssh_sshd::listening_changed, this, &bssh_sshd_keeper::report);

	/*
	 * The server only, on the thread the news arrives on: the rest is
	 * brought up to date by time_limit_reached below once Qt's thread runs.
	 */
	m_platform->set_stop_hook([this] { m_server.stop(); });

	connect(m_platform, &bssh_sshd_platform::time_limit_reached, this, [this] {
		if (m_bound.isNull()) return;
		/*
		 * Still switched on, but not started again by the poll: Android
		 * said no, and asking every five seconds would only hear it again.
		 * The next time the app comes to the front, it is.
		 */
		m_held_by_system = true;
		stop();
		/*
		 * What happened, and no promise about when Start works again: the
		 * Fold started fine an hour after a timeout, and whether opening
		 * the app renewed the allowance or the test's own reset of the
		 * limit did was not separated (project.md sec 19.6).
		 */
		log(tr("Stopped: Android would not keep the file server running "
		       "in the background."));
	});

	m_address_timer = new QTimer(this);
	m_address_timer->setInterval(ADDRESS_POLL_MS);
	connect(m_address_timer, &QTimer::timeout, this, &bssh_sshd_keeper::follow_address);
	m_address_timer->start();
}

bssh_sshd_keeper::~bssh_sshd_keeper() {
	m_platform->set_stop_hook(nullptr);
	stop();
}

void bssh_sshd_keeper::log(const QString &line) {
	emit logged(QTime::currentTime(), line);
}

void bssh_sshd_keeper::start_if_switched_on() {
	if (m_platform->switched_on()) start();
}

void bssh_sshd_keeper::switch_on() {
	start();
	if (serving()) m_platform->set_switched_on(true);
}

void bssh_sshd_keeper::switch_off() {
	stop();
	m_platform->set_switched_on(false);
}

void bssh_sshd_keeper::resumed() {
	m_held_by_system = false;
	follow_address();
}

QString bssh_sshd_keeper::status_line() const {
	const QString root = m_server.config().root.isEmpty() ? QString()
	                     : QFile::decodeName(m_server.config().root);
	if (serving() && !m_server.listening()) {
		return tr("Waiting for Wi-Fi: serves %1 again at %2, port %3, when it is back.")
		        .arg(root, m_bound.toString())
		        .arg(m_server.port());
	}
	if (serving()) {
		return tr("Serving %1 at %2, port %3 -- %n connection(s)", nullptr, m_server.connections())
		        .arg(root, m_bound.toString())
		        .arg(m_server.port());
	}
	if (!m_platform->has_storage_access()) return tr("Stopped. Needs All files access to serve storage.");
	if (m_platform->listen_address().isNull()) return tr("Stopped. Connect to Wi-Fi to serve files.");
	return tr("Stopped.");
}

void bssh_sshd_keeper::report() {
	if (serving() && !m_server.listening()) {
		m_platform->set_serving(true, tr("Waiting for Wi-Fi"));
	} else if (serving()) {
		m_platform->set_serving(true, tr("Serving at %1:%2 -- %n connection(s)", nullptr,
		                                 m_server.connections())
		                              .arg(m_bound.toString()).arg(m_server.port()));
	}
	emit changed();
}

void bssh_sshd_keeper::start() {
	if (serving()) return;

	const QHostAddress address = m_platform->listen_address();
	if (!m_platform->has_storage_access() || address.isNull()) {
		emit changed();
		return;
	}

	const QString data = m_platform->data_directory();
	QDir().mkpath(data);

	bssh_sshd_config_t config;
	config.root = QFile::encodeName(m_platform->storage_root());
	config.host_key_path = data + QStringLiteral("/host_ed25519_key");
	config.authorized_keys_path = authorized_keys_path(m_platform);
	config.password_path = password_path(m_platform);
	config.bind_address = address.toString().toLatin1();
	config.port = QSettings().value(QStringLiteral("port"), DEFAULT_PORT).toInt();

	/*
	 * BeerSSH's kept sessions (sec 5.6, sec 19.1), where they are switched
	 * on and this build carries the helper: a shell as this app.
	 */
	if (QSettings().value(QStringLiteral("sessions"), false).toBool()) {
		const QString helper = m_platform->session_helper();
		if (helper.isEmpty()) {
			log(tr("Sessions are on, but this build has no session helper"));
		} else {
			prepare_session_environment(data);
			config.session_helper = QFile::encodeName(helper);
			/*
			 * And BeerSSH's Pair Over Fuzznet answered here, as by a
			 * desktop's beersshd: the window's own pairing, on this
			 * object's thread, which owns the host it stops and starts.
			 */
			if (can_pair()) {
				config.pair_device = [this](bssh_sshd_config_t::pair_done_t done) {
					QMetaObject::invokeMethod(this, [this, done] {
						QString why;
						const QString image = pair_device(&why);
						done(image.toLatin1(), why);
					}, Qt::QueuedConnection);
				};
			}
		}
	}

	QString error;
	if (!m_server.start(config, &error)) {
		log(error);
		emit changed();
		return;
	}

	m_bound = address;
	m_platform->request_notifications();
	log(tr("Listening on %1:%2").arg(address.toString()).arg(m_server.port()));
	if (!config.session_helper.isEmpty()) start_fuzznet(m_server.port());
	report();
}

QString bssh_sshd_keeper::fuzznet_store() const {
	return m_platform->data_directory() + QStringLiteral("/fuzznet");
}

bool bssh_sshd_keeper::fuzznet_running() const {
	return m_fuzznet && m_fuzznet->state() != QProcess::NotRunning;
}

bool bssh_sshd_keeper::can_pair() const {
	return !m_platform->fuzznet_node().isEmpty();
}

/*
 * The host's own estate, made the first time: this app's store in its own
 * data, so there is no mistyped directory to become one by accident -- the
 * reason beersshd's command line will not make one (fuzznet_pair.h).
 */
bool bssh_sshd_keeper::ensure_fuzznet_store(QString *why) {
	const QString store = fuzznet_store();
	if (QDir(store).exists() && !QDir(store).isEmpty()) return true;
	QProcess root;
	root.start(m_platform->fuzznet_node(), { QStringLiteral("--fuzznet"),
	                                         QStringLiteral("--fuzznet-dir=") + store,
	                                         QStringLiteral("--new-root") });
	if (!root.waitForFinished(30000) || root.exitStatus() != QProcess::NormalExit || root.exitCode() != 0) {
		root.kill();
		root.waitForFinished(2000);
		*why = tr("could not make this server's fuzznet store: %1")
		               .arg(QString::fromLocal8Bit(root.readAllStandardError()).trimmed());
		return false;
	}
	log(tr("Made this server's fuzznet store"));
	return true;
}

QString bssh_sshd_keeper::pair_device(QString *why) {
	QString ignored;
	if (!why) why = &ignored;
	if (!can_pair()) return bssh_sshd_control::pair_device(why);
	if (!ensure_fuzznet_store(why)) return QString();
	/*
	 * Stopped for the pairing and started again after it, whichever way it
	 * went. A host serving read its store when it started and would not
	 * know the device; and beside it the pairing and the host both wrote
	 * the store's journal, which fuzznet now refuses (its sec 1275). The
	 * sessions it carried are attachments; each goes on in its own process.
	 */
	const bool serving = fuzznet_running();
	if (serving) stop_fuzznet();
	const QString image = bssh_fuzznet_pair(m_platform->fuzznet_node(),
	                                        { QStringLiteral("--fuzznet-dir=") + fuzznet_store() }, why);
	if (serving) start_fuzznet(m_server.port());
	if (!image.isEmpty()) log(tr("Paired a device for BeerSSH over fuzznet"));
	return image;
}

/*
 * Kept sessions over fuzznet beside those over SSH (sec 5.6), where this
 * build carries the host: on the same port number, UDP, with the helper the
 * SSH side uses, which the host finds beside itself. Its own process, as
 * fuzznet's node runs one per process; it ends with the server.
 */
void bssh_sshd_keeper::start_fuzznet(int port) {
	const QString node = m_platform->fuzznet_node();
	if (node.isEmpty() || fuzznet_running()) return;
	QString why;
	if (!ensure_fuzznet_store(&why)) {
		log(why);
		return;
	}
	const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", m_platform->data_directory());
	if (!m_fuzznet) {
		m_fuzznet = new QProcess(this);
		/* Its log is fuzznet's own, in the app's data; a pipe nobody read would stop it once full. */
		m_fuzznet->setStandardOutputFile(QProcess::nullDevice());
		m_fuzznet->setStandardErrorFile(QProcess::nullDevice());
		connect(m_fuzznet, &QProcess::finished, this, [this](int code) {
			log(tr("The fuzznet host stopped (%1)").arg(code));
		});
	}
	m_fuzznet->start(node, { QStringLiteral("--fuzznet"), QStringLiteral("--fuzznet-dir=") + fuzznet_store(),
	                         QStringLiteral("--socket=") + runtime + QStringLiteral("/fuzznet.sock"),
	                         QStringLiteral("--udp-port=%1").arg(port),
	                         QStringLiteral("--log-dir=") + m_platform->data_directory()
	                                 + QStringLiteral("/fuzznet-log"),
	                         QStringLiteral("--sessions") });
	if (m_fuzznet->waitForStarted(5000))
		log(tr("Keeping sessions over fuzznet too, on UDP port %1").arg(port));
	else
		log(tr("The fuzznet host did not start: %1").arg(m_fuzznet->errorString()));
}

void bssh_sshd_keeper::stop_fuzznet() {
	if (!fuzznet_running()) return;
	m_fuzznet->terminate();
	if (!m_fuzznet->waitForFinished(3000)) {
		m_fuzznet->kill();
		m_fuzznet->waitForFinished(2000);
	}
}

/*
 * What the helper and the shell it starts read from the environment, and
 * an app process lacks: no SHELL, no HOME a shell can use, and no runtime
 * directory, whose /tmp fallback does not exist on Android. Only what is
 * unset is set, so a desktop's own values stand.
 */
void bssh_sshd_keeper::prepare_session_environment(const QString &data) {
	if (qEnvironmentVariableIsEmpty("SHELL")) {
		qputenv("SHELL", QFile::exists(QStringLiteral("/system/bin/sh")) ? "/system/bin/sh" : "/bin/sh");
	}
	if (qEnvironmentVariableIsEmpty("HOME")) {
		const QString home = data + QStringLiteral("/home");
		QDir().mkpath(home);
		qputenv("HOME", QFile::encodeName(home));
	}
	if (qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR")) {
		const QString runtime = data + QStringLiteral("/run");
		QDir().mkpath(runtime);
		QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
		qputenv("XDG_RUNTIME_DIR", QFile::encodeName(runtime));
	}
}

void bssh_sshd_keeper::stop() {
	/*
	 * By the bound address rather than serving(): the platform's stop hook
	 * may already have stopped the server, and the rest of this is still
	 * owed -- the notification gone, the log told.
	 */
	if (m_bound.isNull()) return;
	stop_fuzznet();
	m_server.stop();
	m_bound = QHostAddress();
	m_platform->set_serving(false, QString());
	log(tr("Stopped listening"));
	emit changed();
}

void bssh_sshd_keeper::follow_address() {
	if (!serving()) {
		/* Switched on and stopped by something else: serve again once it can. */
		if (m_platform->switched_on() && !m_held_by_system && m_platform->has_storage_access()
		    && !m_platform->listen_address().isNull()) {
			start();
			return;
		}
		emit changed();
		return;
	}

	const QHostAddress now = m_platform->listen_address();
	if (now == m_bound) return;

	/*
	 * Wi-Fi gone is not a reason to stop (the holder, 2026-11-03, sec
	 * 19.6). The address is kept; the server listens again on it by itself
	 * when the same network returns, which is the common case, and a
	 * different address is followed the next time this runs.
	 */
	if (now.isNull()) return;

	log(tr("The Wi-Fi address changed to %1; listening there instead.").arg(now.toString()));
	stop();
	start();
}
