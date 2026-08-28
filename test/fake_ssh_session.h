#ifndef BSSH_FAKE_SSH_SESSION_H
#define BSSH_FAKE_SSH_SESSION_H

#include <QList>

#include "ssh/ssh_session.h"

/*
 * A bssh_ssh_session that talks to nobody (project.md sec 4, sec 12.3).
 *
 * It replays a scripted byte stream as if a server had sent it, and
 * records everything the client wrote. That is the whole test seam for
 * three of the components most likely to regress: the emulator (bytes in,
 * screen out), the input router (keys in, bytes out -- M3), and the
 * reconnection FSM (state transitions without a network to flap).
 *
 * It lives in test/ on purpose. The shipping app must not carry a
 * pretend SSH backend that could ever be selected by accident.
 *
 * Delivery is synchronous: open_shell() emits the script immediately
 * rather than on a timer, so tests assert without waiting. When timing
 * itself is under test -- chunk boundaries splitting an escape sequence,
 * say -- feed the chunks by hand with emit_chunk() instead.
 */
class bssh_fake_ssh_session : public bssh_ssh_session {
	Q_OBJECT

public:
	explicit bssh_fake_ssh_session(QObject *parent = nullptr);

	/* What the "server" sends once a shell is opened. */
	void set_script(const QList<QByteArray> &chunks) { m_script = chunks; }

	/* The remote shell ends by itself -- `exit` at the prompt. */
	void simulate_remote_exit(int status) {
		emit shell_exited(status);
		set_state(DISCONNECTED);
	}

	/* Keys were tried and did not get in. */
	void simulate_password_required() { emit password_required(); }

	/* Keys got in; no password was ever asked for. */
	void simulate_key_auth_success() { set_state(CONNECTED); }

	/* The connection drops underneath a live shell. */
	void simulate_connection_lost() {
		emit failed(QStringLiteral("connection reset"));
		set_state(DISCONNECTED);
	}

	/* Send one chunk now, whatever the script says. */
	void emit_chunk(const QByteArray &bytes) { emit bytes_received(bytes); }

	/* Everything the client has written, concatenated. */
	QByteArray written() const { return m_written; }

	/*
	 * Forget it, so the next assertion is about one exchange rather than
	 * about everything since the connection opened. A test that asks
	 * several questions in turn otherwise has to match a growing
	 * haystack, and "the answer is in there somewhere" is a weaker claim
	 * than "this is what came back".
	 */
	void clear_written() { m_written.clear(); }

	int cols() const { return m_cols; }
	int rows() const { return m_rows; }
	int resize_count() const { return m_resize_count; }

	/* Make the next connect_to_host() fail, to exercise the sad path. */
	void set_fail_on_connect(const QString &reason) { m_fail_reason = reason; }

	void connect_to_host(const QString &host, quint16 port, const QString &user) override;
	void authenticate_password(const QString &password) override;
	void open_shell(int cols, int rows) override;
	void resize(int cols, int rows) override;
	void send(const QByteArray &bytes) override;
	void disconnect_from_host() override;

	/* Recorded rather than acted on -- what the tests check is that the
	 * host's interval reached the transport at all. */
	void set_keepalive(int seconds) override { m_keepalive_seconds = seconds; }
	int keepalive_seconds() const { return m_keepalive_seconds; }

	void set_term(const QString &term) override { m_term = term; }
	QString term() const { return m_term; }

	/* Recorded rather than run, so a test can ask what -e reached (sec 8.6). */
	void set_command(const QStringList &command) override { m_command = command; }
	QStringList command() const { return m_command; }

	void set_compression(bool compression) override { m_compression = compression; }
	bool compression() const { return m_compression; }

	void set_identity_file(const QString &path) override { m_identity_file = path; }
	void set_proxy_jump(const QString &spec) override { m_proxy_jump = spec; }
	QString identity_file() const { return m_identity_file; }
	QString proxy_jump() const { return m_proxy_jump; }
	void accept_host_key() override { m_host_key_accepted = true; }
	void supply_key_passphrase(const QString &passphrase) override { m_passphrase = passphrase; }

	QString key_passphrase() const { return m_passphrase; }
	void simulate_key_passphrase_required() { emit key_passphrase_required(); }

	bool host_key_accepted() const { return m_host_key_accepted; }

	void simulate_host_key_unknown(const QString &fingerprint) { emit host_key_unknown(fingerprint); }
	void simulate_host_key_changed(const QString &fingerprint) { emit host_key_changed(fingerprint); }

private:
	QString m_identity_file;
	QString m_proxy_jump;
	QList<QByteArray> m_script;
	QByteArray m_written;
	QString m_fail_reason;
	int m_cols;
	int m_rows;
	int m_resize_count;
	bool m_host_key_accepted = false;
	int m_keepalive_seconds = 0;
	QString m_term;
	QStringList m_command;
	bool m_compression = false;
	QString m_passphrase;
};

#endif /* BSSH_FAKE_SSH_SESSION_H */
