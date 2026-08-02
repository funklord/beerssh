#include "model/session.h"

#include "input/input_router.h"
#include "term/emulator.h"

bssh_session::bssh_session(const bssh_profile_t &profile, QObject *parent)
    : QObject(parent),
      m_profile(profile),
      m_transport(nullptr),
      m_user_disconnected(false),
      m_unseen_bell(false),
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

void bssh_session::connect_to_host(const QString &password) {
	if (m_transport) return;

	m_user_disconnected = false;
	m_password = password;
	m_transport = bssh_ssh_session::create(this);

	connect(m_transport, &bssh_ssh_session::bytes_received,
	         m_emulator, &bssh_emulator::feed);
	connect(m_transport, &bssh_ssh_session::failed, this, &bssh_session::failed);

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
		if (state == bssh_ssh_session::AUTHENTICATING) {
			/*
			 * Kept, not cleared: without it a reconnect cannot
			 * authenticate, and prompting again for every network blip
			 * would make auto-reconnect useless. That is a real
			 * trade-off -- the password now lives in process memory for
			 * the life of the session -- and it is what sec 11's
			 * Keystore work at M6 replaces with a key.
			 */
			m_transport->authenticate_password(m_password);
		} else if (state == bssh_ssh_session::CONNECTED) {
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

	const QString password = m_password;
	connect_to_host(password);
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
