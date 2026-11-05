#include "sshd/sshd_window.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QHBoxLayout>
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

#include "sshd/sshd_keeper.h"
#include "sshd/sshd_platform.h"

namespace {

/*
 * Above 1024, which an app may bind without privilege, and the number
 * Termux's sshd made familiar on phones.
 */
const int DEFAULT_PORT = 8022;

QLabel *wrapped_label(const QString &text = QString()) {
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	return label;
}

} /* namespace */

bssh_sshd_window::bssh_sshd_window(bssh_sshd_platform *platform, bssh_sshd_control *control,
                                   QWidget *parent)
    : QWidget(parent), m_platform(platform), m_control(control) {
	if (!m_control) {
		m_keeper = new bssh_sshd_keeper(platform, this);
		m_control = m_keeper;
	} else {
		m_control->setParent(this);
	}

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
		if (m_control->active()) m_control->switch_off();
		else m_control->switch_on();
	});
	layout->addWidget(m_toggle);

	/*
	 * Configurable, and off until turned on: the holder, 2026-11-03, "it
	 * restarts if configured to do so. That is the whole point of an
	 * sshd." With it off, a restart with the server on leaves a
	 * notification to tap instead (sec 19.6).
	 */
	/*
	 * Shown while Android may still pause the app to save battery (sec
	 * 5.4): a vendor's battery manager is the likeliest thing to make a
	 * server on all day unreachable at night. Asked only from this button.
	 */
	m_battery = new QWidget;
	m_battery->setObjectName(QStringLiteral("sshd-battery"));
	auto *battery_layout = new QVBoxLayout(m_battery);
	battery_layout->setContentsMargins(0, 0, 0, 0);
	battery_layout->addWidget(wrapped_label(
	        tr("Android may pause beersshd to save battery, and a paused server cannot be "
	           "reached. Allow it to run in the background to keep it reachable.")));
	auto *allow = new QPushButton(tr("Allow running in the background"));
	allow->setObjectName(QStringLiteral("sshd-battery-allow"));
	connect(allow, &QPushButton::clicked, this, [this] { m_platform->request_battery_exemption(); });
	battery_layout->addWidget(allow);
	layout->addWidget(m_battery);

	if (m_platform->can_start_at_boot()) {
		auto *boot = new QCheckBox(tr("Serve again after the phone restarts"));
		boot->setObjectName(QStringLiteral("sshd-start-at-boot"));
		boot->setChecked(m_platform->start_at_boot());
		connect(boot, &QCheckBox::toggled, this, [this](bool on) { m_platform->set_start_at_boot(on); });
		layout->addWidget(boot);

		/*
		 * Said here because the wording above promises more than a phone
		 * with a lock can give: its storage stays encrypted, and
		 * BOOT_COMPLETED unsent, until the first unlock (sec 19.6).
		 */
		layout->addWidget(wrapped_label(tr("With a screen lock, serving starts at the first unlock "
		                                   "after the restart: until then Android keeps your files "
		                                   "encrypted.")));
	}

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
	           "and the rest -- to the keys you list below, and to a password if you set "
	           "one. Android calls that \"All files access\", and it is turned on in "
	           "Settings.")));
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

	/*
	 * A password, for a computer whose key is not on the phone yet -- the
	 * holder, 2026-10-09: a key has to be carried here first, and a
	 * password does not. Off until set. Only its salted hash is written
	 * (bssh_sshd::set_password), and like the keys it is read at each
	 * login, so setting or removing it applies to the next one.
	 */
	layout->addWidget(wrapped_label(
	        tr("Password: anyone on this Wi-Fi who has it can read and change your files. "
	           "Leave it unset to allow keys only.")));
	m_password = new QLineEdit;
	m_password->setObjectName(QStringLiteral("sshd-password"));
	m_password->setEchoMode(QLineEdit::Password);
	m_password->setPlaceholderText(tr("New password"));
	layout->addWidget(m_password);

	auto *password_buttons = new QHBoxLayout;
	auto *set_button = new QPushButton(tr("Set password"));
	set_button->setObjectName(QStringLiteral("sshd-set-password"));
	connect(set_button, &QPushButton::clicked, this, [this] {
		if (!m_password->text().isEmpty()) set_password(m_password->text());
	});
	auto *remove_button = new QPushButton(tr("Remove password"));
	remove_button->setObjectName(QStringLiteral("sshd-remove-password"));
	connect(remove_button, &QPushButton::clicked, this, [this] { set_password(QString()); });
	password_buttons->addWidget(set_button);
	password_buttons->addWidget(remove_button);
	layout->addLayout(password_buttons);

	m_password_state = wrapped_label();
	m_password_state->setObjectName(QStringLiteral("sshd-password-state"));
	layout->addWidget(m_password_state);

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

	connect(m_control, &bssh_sshd_control::logged, this, &bssh_sshd_window::log_at);
	connect(m_control, &bssh_sshd_control::changed, this, &bssh_sshd_window::refresh);

	/*
	 * Coming back from Settings is how access arrives, so look again then
	 * -- and at the keys, which may have been changed from outside: they
	 * are a file, and the box only shows it.
	 */
	connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
		if (state != Qt::ApplicationActive) return;
		reload_keys();
		m_control->resumed();
		refresh();
	});

	/*
	 * Left on when the app last ran: serving again at once, or as soon as
	 * Wi-Fi and access allow, which the keeper's poll checks.
	 */
	reload_keys();
	refresh_password_state();
	refresh();
	if (m_keeper) m_keeper->start_if_switched_on();
}

QString bssh_sshd_window::password_path() const {
	return bssh_sshd_keeper::password_path(m_platform);
}

void bssh_sshd_window::set_password(const QString &password) {
	QString error;
	if (!bssh_sshd::set_password(password_path(), password, &error)) {
		log(tr("Password not changed: %1").arg(error));
		return;
	}
	m_password->clear();
	log(password.isEmpty() ? tr("Password removed: keys only.") : tr("Password set."));
	refresh_password_state();
}

void bssh_sshd_window::refresh_password_state() {
	m_password_state->setText(bssh_sshd::has_password(password_path())
	                                  ? tr("Password login is on.")
	                                  : tr("Password login is off."));
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
	/*
	 * First, and unhooked: stopping a keeper reports, and a report that
	 * arrived while ~QWidget was deleting children would refresh a window
	 * that is half gone.
	 */
	m_control->disconnect(this);
	delete m_control;
}

bool bssh_sshd_window::serving() const {
	return m_control->active();
}

const bssh_sshd &bssh_sshd_window::server() const {
	Q_ASSERT(m_keeper);
	return m_keeper->server();
}

QString bssh_sshd_window::authorized_keys_path() const {
	return bssh_sshd_keeper::authorized_keys_path(m_platform);
}

void bssh_sshd_window::refresh() {
	const bool access = m_platform->has_storage_access();
	const bool active = m_control->active();
	m_access->setVisible(!access);
	m_battery->setVisible(m_platform->battery_restricted());
	m_toggle->setEnabled(active || (access && !m_platform->listen_address().isNull()));
	m_toggle->setText(active ? tr("Stop") : tr("Start"));
	m_port->setEnabled(!active);
	m_status->setText(m_control->status_line());
	const QString fingerprint = m_control->fingerprint();
	if (fingerprint != m_fingerprint->text()) {
		m_fingerprint->setText(fingerprint);
		m_fingerprint->setCursorPosition(0);
	}
}

void bssh_sshd_window::start() {
	if (m_keeper) m_keeper->start();
	else m_control->switch_on();
}

void bssh_sshd_window::stop() {
	if (m_keeper) m_keeper->stop();
	else m_control->switch_off();
}

void bssh_sshd_window::follow_address() {
	m_control->follow();
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
	log_at(QTime::currentTime(), line);
}

void bssh_sshd_window::log_at(const QTime &when, const QString &line) {
	m_log->appendPlainText(when.toString(QStringLiteral("HH:mm:ss ")) + line);
}
