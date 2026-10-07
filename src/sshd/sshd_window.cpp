#include "sshd/sshd_window.h"

#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include "sshd/sshd_platform.h"

namespace {

/*
 * Above 1024, which an app may bind without privilege, and the number
 * Termux's sshd made familiar on phones.
 */
const int DEFAULT_PORT = 8022;

const int ADDRESS_POLL_MS = 5000;

QLabel *wrapped_label(const QString &text = QString()) {
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	return label;
}

} /* namespace */

bssh_sshd_window::bssh_sshd_window(bssh_sshd_platform *platform, QWidget *parent)
    : QWidget(parent), m_platform(platform) {
	setWindowTitle(QStringLiteral("beersshd"));

	auto *content = new QWidget;
	auto *layout = new QVBoxLayout(content);

	m_status = wrapped_label();
	m_status->setObjectName(QStringLiteral("sshd-status"));
	m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_status);

	/*
	 * A read-only line rather than a label. The fingerprint is one word of
	 * fifty characters with nowhere to wrap, and as a label it set the
	 * page's minimum width: on a Fold's cover screen the whole page grew
	 * past the right edge and took the Stop button with it. A line scrolls
	 * instead, and copies whole -- which a client's first connection needs.
	 */
	m_fingerprint = new QLineEdit;
	m_fingerprint->setObjectName(QStringLiteral("sshd-fingerprint"));
	m_fingerprint->setReadOnly(true);
	m_fingerprint->setFrame(false);
	m_fingerprint->setPlaceholderText(tr("Host key: shown once started"));
	layout->addWidget(m_fingerprint);

	m_toggle = new QPushButton(tr("Start"));
	m_toggle->setObjectName(QStringLiteral("sshd-toggle"));
	connect(m_toggle, &QPushButton::clicked, this, [this] {
		if (serving()) stop();
		else start();
	});
	layout->addWidget(m_toggle);

	/*
	 * Shown until access is granted, and the reason given in the app's own
	 * words before the system's switch: Settings says only "Allow access to
	 * manage all files", which reads alarming without the why.
	 */
	m_access = new QWidget;
	m_access->setObjectName(QStringLiteral("sshd-access"));
	auto *access_layout = new QVBoxLayout(m_access);
	access_layout->setContentsMargins(0, 0, 0, 0);
	access_layout->addWidget(wrapped_label(
	        tr("beersshd serves your phone's shared storage -- Downloads, DCIM, Documents "
	           "and the rest -- to the keys you list below. Android calls that \"All files "
	           "access\", and it is turned on in Settings.")));
	auto *grant = new QPushButton(tr("Open Settings"));
	grant->setObjectName(QStringLiteral("sshd-grant"));
	connect(grant, &QPushButton::clicked, this, [this] { m_platform->request_storage_access(); });
	access_layout->addWidget(grant);
	layout->addWidget(m_access);

	auto *form = new QFormLayout;
	m_port = new QSpinBox;
	m_port->setObjectName(QStringLiteral("sshd-port"));
	m_port->setRange(1024, 65535);
	m_port->setValue(QSettings().value(QStringLiteral("port"), DEFAULT_PORT).toInt());
	connect(m_port, &QSpinBox::valueChanged, this, [](int port) {
		QSettings().setValue(QStringLiteral("port"), port);
	});
	form->addRow(tr("Port"), m_port);
	layout->addLayout(form);

	layout->addWidget(wrapped_label(
	        tr("Authorized keys, one public key per line, as in ~/.ssh/authorized_keys. "
	           "A line with options such as from= is not used.")));
	m_keys = new QPlainTextEdit;
	m_keys->setObjectName(QStringLiteral("sshd-keys"));
	m_keys->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_keys->setMinimumHeight(fontMetrics().lineSpacing() * 5);
	layout->addWidget(m_keys);

	auto *save = new QPushButton(tr("Save keys"));
	save->setObjectName(QStringLiteral("sshd-save-keys"));
	connect(save, &QPushButton::clicked, this, &bssh_sshd_window::save_keys);
	layout->addWidget(save);

	m_key_count = wrapped_label();
	m_key_count->setObjectName(QStringLiteral("sshd-key-count"));
	layout->addWidget(m_key_count);

	m_log = new QPlainTextEdit;
	m_log->setObjectName(QStringLiteral("sshd-log"));
	m_log->setReadOnly(true);
	m_log->setMaximumBlockCount(200);
	m_log->setMinimumHeight(fontMetrics().lineSpacing() * 5);
	layout->addWidget(m_log);

	auto *scroll = new QScrollArea;
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(scroll);

	connect(&m_server, &bssh_sshd::logged, this, &bssh_sshd_window::log);
	connect(&m_server, &bssh_sshd::connections_changed, this, &bssh_sshd_window::update_status);

	/*
	 * The server only: the screen is Qt's, and is brought up to date by
	 * time_limit_reached below once Qt's thread runs again.
	 */
	m_platform->set_stop_hook([this] { m_server.stop(); });

	connect(m_platform, &bssh_sshd_platform::time_limit_reached, this, [this] {
		if (m_bound.isNull()) return;
		stop();
		/*
		 * What happened, and no promise about when Start works again: the
		 * Fold started fine an hour after a timeout, and whether opening
		 * the app renewed the allowance or the test's own reset of the
		 * limit did was not separated (project.md sec 19.6).
		 */
		log(tr("Stopped: Android ended the file-transfer service. It allows one "
		       "six hours in any 24 while the app is in the background."));
	});

	/*
	 * Coming back from Settings is how access arrives, so look again then
	 * -- and at the keys, which may have been changed from outside: they
	 * are a file, and the box only shows it.
	 */
	connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (state != Qt::ApplicationActive) return;
		reload_keys();
		refresh();
	});

	m_address_timer = new QTimer(this);
	m_address_timer->setInterval(ADDRESS_POLL_MS);
	connect(m_address_timer, &QTimer::timeout, this, &bssh_sshd_window::follow_address);
	m_address_timer->start();

	reload_keys();
	refresh();
}

void bssh_sshd_window::reload_keys() {
	/*
	 * Not over edits the user has not saved: those are theirs, and the
	 * file is only what was there before them.
	 */
	if (m_keys->document()->isModified()) return;

	QFile file(authorized_keys_path());
	const QByteArray text = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	if (QString::fromUtf8(text) != m_keys->toPlainText()) m_keys->setPlainText(QString::fromUtf8(text));
	m_keys->document()->setModified(false);
	m_key_count->setText(tr("%n usable key(s).", nullptr, bssh_sshd::usable_keys(text)));
}

bssh_sshd_window::~bssh_sshd_window() {
	m_platform->set_stop_hook(nullptr);
	stop();
}

QString bssh_sshd_window::authorized_keys_path() const {
	return m_platform->data_directory() + QStringLiteral("/authorized_keys");
}

void bssh_sshd_window::refresh() {
	const bool access = m_platform->has_storage_access();
	m_access->setVisible(!access);
	m_toggle->setEnabled(serving() || (access && !m_platform->listen_address().isNull()));
	update_status();
}

QString bssh_sshd_window::status_line() const {
	if (serving()) {
		return tr("Serving %1 at %2, port %3 -- %n connection(s)", nullptr, m_server.connections())
		        .arg(m_server.config().root.isEmpty() ? QString() : QFile::decodeName(m_server.config().root),
		             m_bound.toString())
		        .arg(m_server.port());
	}
	if (!m_platform->has_storage_access()) return tr("Stopped. Needs All files access to serve storage.");
	if (m_platform->listen_address().isNull()) return tr("Stopped. Connect to Wi-Fi to serve files.");
	return tr("Stopped.");
}

void bssh_sshd_window::update_status() {
	m_status->setText(status_line());
	m_toggle->setText(serving() ? tr("Stop") : tr("Start"));
	if (serving()) {
		m_platform->set_serving(true, tr("Serving at %1:%2 -- %n connection(s)", nullptr,
		                                 m_server.connections())
		                              .arg(m_bound.toString()).arg(m_server.port()));
	}
}

void bssh_sshd_window::start() {
	if (serving()) return;

	const QHostAddress address = m_platform->listen_address();
	if (!m_platform->has_storage_access() || address.isNull()) {
		refresh();
		return;
	}

	const QString data = m_platform->data_directory();
	QDir().mkpath(data);

	bssh_sshd_config_t config;
	config.root = QFile::encodeName(m_platform->storage_root());
	config.host_key_path = data + QStringLiteral("/host_ed25519_key");
	config.authorized_keys_path = authorized_keys_path();
	config.bind_address = address.toString().toLatin1();
	config.port = m_port->value();

	QString error;
	if (!m_server.start(config, &error)) {
		log(error);
		refresh();
		return;
	}

	m_bound = address;
	m_port->setEnabled(false);
	m_platform->request_notifications();
	m_fingerprint->setText(m_server.host_key_fingerprint());
	m_fingerprint->setCursorPosition(0);
	log(tr("Listening on %1:%2").arg(address.toString()).arg(m_server.port()));
	update_status();
	refresh();
}

void bssh_sshd_window::stop() {
	/*
	 * By the bound address rather than serving(): the platform's stop hook
	 * may already have stopped the server, and the rest of this is still
	 * owed -- the port unlocked, the notification gone, the log told.
	 */
	if (m_bound.isNull()) return;
	m_server.stop();
	m_bound = QHostAddress();
	m_port->setEnabled(true);
	m_platform->set_serving(false, QString());
	log(tr("Stopped listening"));
	refresh();
}

void bssh_sshd_window::follow_address() {
	if (!serving()) {
		refresh();
		return;
	}

	const QHostAddress now = m_platform->listen_address();
	if (now == m_bound) return;

	if (now.isNull()) {
		stop();
		log(tr("Stopped: Wi-Fi is gone, and the server listens on Wi-Fi only."));
		return;
	}

	log(tr("The Wi-Fi address changed to %1; listening there instead.").arg(now.toString()));
	stop();
	start();
}

void bssh_sshd_window::save_keys() {
	const QByteArray text = m_keys->toPlainText().toUtf8();
	QDir().mkpath(m_platform->data_directory());

	QSaveFile file(authorized_keys_path());
	if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit()) {
		log(tr("The keys could not be saved."));
		return;
	}

	m_keys->document()->setModified(false);
	const int usable = bssh_sshd::usable_keys(text);
	m_key_count->setText(tr("%n usable key(s).", nullptr, usable));
	log(tr("Saved %n usable key(s); the next login reads them.", nullptr, usable));
}

void bssh_sshd_window::log(const QString &line) {
	m_log->appendPlainText(QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line);
}
