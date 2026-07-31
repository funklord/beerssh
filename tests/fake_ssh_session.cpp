#include "fake_ssh_session.h"

bssh_fake_ssh_session::bssh_fake_ssh_session(QObject *parent)
    : bssh_ssh_session(parent),
      m_cols(0),
      m_rows(0),
      m_resize_count(0) {
}

void bssh_fake_ssh_session::connect_to_host(const QString &host, quint16 port, const QString &user) {
	Q_UNUSED(host);
	Q_UNUSED(port);
	Q_UNUSED(user);

	if (!m_fail_reason.isEmpty()) {
		set_state(FAILED);
		emit failed(m_fail_reason);
		return;
	}

	/*
	 * The full transition, not a jump straight to CONNECTED: a status UI
	 * that only ever sees the endpoints is a status UI whose intermediate
	 * states have never been rendered even once.
	 */
	set_state(RESOLVING);
	set_state(CONNECTING);
	set_state(AUTHENTICATING);
}

void bssh_fake_ssh_session::authenticate_password(const QString &password) {
	Q_UNUSED(password);
	set_state(CONNECTED);
}

void bssh_fake_ssh_session::open_shell(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;

	for (const QByteArray &chunk : m_script) {
		emit bytes_received(chunk);
	}
}

void bssh_fake_ssh_session::resize(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;
	++m_resize_count;
}

void bssh_fake_ssh_session::send(const QByteArray &bytes) {
	m_written.append(bytes);
}

void bssh_fake_ssh_session::disconnect_from_host() {
	set_state(DISCONNECTED);
}
