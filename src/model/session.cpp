#include "model/session.h"

#include "input/input_router.h"
#include "term/emulator.h"

bssh_session::bssh_session(const bssh_profile_t &profile, QObject *parent)
    : QObject(parent),
      m_profile(profile),
      m_transport(nullptr),
      m_user_disconnected(false),
      m_unseen_bell(false),
      m_remote_exited(false),
      m_exit_status(0),
      m_used_key_auth(false),
      m_password_requested(false),
      m_on_screen(false),
      m_cols(80),
      m_rows(24) {
	/*
	 * The emulator exists before the connection does, so output has
	 * somewhere to land from the first byte -- and so a session that
	 * never connects still has a screen to show its error on.
	 */
	m_emulator = new bssh_emulator(m_cols, m_rows, this);
	m_input_router = new bssh_input_router(m_emulator, this);

	connect(m_emulator, &bssh_emulator::bell, this, [this] {
		if (m_on_screen) return;

		m_unseen_bell = true;
		emit state_changed();
	});

	connect(m_emulator, &bssh_emulator::title_changed, this, [this](const QString &title) {
		m_remote_title = title;
		emit title_changed();
	});
}

bssh_session::~bssh_session() {
}

void bssh_session::set_settings(const QJsonObject &settings) {
	if (settings == m_settings) return;

	m_settings = settings;
	emit settings_changed();
}

QString bssh_session::title() const {
	if (!m_remote_title.isEmpty()) return m_remote_title;
	if (!m_profile.name.isEmpty()) return m_profile.name;
	return m_profile.target();
}

bssh_ssh_session::state_t bssh_session::state() const {
	return m_transport ? m_transport->state() : bssh_ssh_session::DISCONNECTED;
}

bool bssh_session::is_live() const {
	const bssh_ssh_session::state_t current = state();
	return current != bssh_ssh_session::DISCONNECTED && current != bssh_ssh_session::FAILED;
}

void bssh_session::connect_to_host() {
	if (m_transport) return;

	m_user_disconnected = false;
	m_remote_exited = false;
	m_exit_status = 0;
	m_used_key_auth = false;
	m_password_requested = false;
	m_transport = bssh_ssh_session::create(this);

	connect(m_transport, &bssh_ssh_session::bytes_received,
	         m_emulator, &bssh_emulator::feed);
	connect(m_transport, &bssh_ssh_session::failed, this, [this](const QString &reason) {
		note_connection_lost();
		emit failed(reason);
	});
	connect(m_transport, &bssh_ssh_session::shell_exited,
	         this, &bssh_session::note_remote_exit);

	connect(m_transport, &bssh_ssh_session::key_passphrase_required,
	         this, &bssh_session::needs_key_passphrase);
	connect(m_transport, &bssh_ssh_session::host_key_unknown,
	         this, &bssh_session::host_key_unknown);
	connect(m_transport, &bssh_ssh_session::host_key_changed,
	         this, &bssh_session::host_key_changed);

	connect(m_transport, &bssh_ssh_session::password_required, this, [this] {
		m_password_requested = true;

		/*
		 * A reconnect can answer for itself if a password was given
		 * before; a first connection has to ask. Keeping the password
		 * only once it has actually been needed is narrower than
		 * keeping it always, which is what this did before keys were
		 * tried.
		 */
		if (!m_password.isEmpty()) {
			m_transport->authenticate_password(m_password);
			return;
		}

		emit needs_password();
	});

	/*
	 * The emulator's replies -- cursor reports, device attributes, and
	 * every keystroke the router encodes -- go back out the transport.
	 * Wired once here rather than in a window, so they keep flowing while
	 * no view is attached at all.
	 */
	connect(m_emulator, &bssh_emulator::output_ready, this, [this] {
		const QByteArray reply = m_emulator->take_output();
		if (!reply.isEmpty() && m_transport) m_transport->send(reply);
	});

	connect(m_transport, &bssh_ssh_session::state_changed,
	         this, [this](bssh_ssh_session::state_t state) {
		if (state == bssh_ssh_session::CONNECTED) {
			/*
			 * Reached without a password ever being asked for means the
			 * key worked -- and such a session holds no credential at
			 * all, so it can reconnect after a drop with nothing kept in
			 * memory (sec 11).
			 */
			if (!m_password_requested) m_used_key_auth = true;
			m_transport->open_shell(m_cols, m_rows);
		}
		emit state_changed();
	});

	m_transport->connect_to_host(m_profile.host, m_profile.port, m_profile.user);
}

void bssh_session::disconnect_from_host() {
	m_user_disconnected = true;
	if (m_transport) m_transport->disconnect_from_host();
}

void bssh_session::reconnect() {
	if (m_user_disconnected) return;

	/*
	 * A fresh transport rather than a revived one: the old libssh
	 * session's socket, channel and state are gone, and reusing the
	 * object would mean reasoning about which parts of it survived a
	 * failure. The emulator, screen and scrollback are untouched, so the
	 * user keeps their history across the drop -- but the remote shell
	 * is genuinely new (sec 5.3).
	 */
	if (m_transport) {
		m_transport->disconnect_from_host();
		m_transport->deleteLater();
		m_transport = nullptr;
	}

	connect_to_host();
}

void bssh_session::supply_password(const QString &password) {
	m_password = password;
	if (m_transport) m_transport->authenticate_password(password);
}

void bssh_session::supply_key_passphrase(const QString &passphrase) {
	if (m_transport) m_transport->supply_key_passphrase(passphrase);
}

void bssh_session::accept_host_key() {
	if (m_transport) m_transport->accept_host_key();
}

void bssh_session::note_remote_exit(int exit_status) {
	m_remote_exited = true;
	m_exit_status = exit_status;

	emit shell_exited(exit_status);
	emit state_changed();
}

void bssh_session::note_connection_lost() {
	m_remote_exited = false;
	emit state_changed();
}

void bssh_session::mark_seen() {
	if (!m_unseen_bell) return;

	m_unseen_bell = false;
	emit state_changed();
}

void bssh_session::set_on_screen(bool on_screen) {
	m_on_screen = on_screen;
	if (on_screen) mark_seen();
}

void bssh_session::resize(int cols, int rows) {
	if (cols <= 0 || rows <= 0) return;
	if (cols == m_cols && rows == m_rows) return;

	m_cols = cols;
	m_rows = rows;

	m_emulator->resize(cols, rows);
	if (m_transport) m_transport->resize(cols, rows);
}
