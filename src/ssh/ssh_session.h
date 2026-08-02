#ifndef BSSH_SSH_SESSION_H
#define BSSH_SSH_SESSION_H

#include <QByteArray>
#include <QObject>
#include <QString>

/*
 * bssh_ssh_session -- the SSH transport, behind an interface (project.md
 * sec 4).
 *
 * The design constraint this file exists to enforce: no libssh (or
 * libssh2) type may appear in this header, or in the terminal and UI
 * layers behind it. The library choice is still open -- libssh for its
 * higher-level PTY/keepalive handling, libssh2 for its BSD licence -- and
 * it stays swappable only for as long as nothing outside
 * src/ssh/ssh_session_libssh.cpp knows which one is in use.
 *
 * The second reason for the interface is testing: a fake implementation
 * replaying a captured byte stream is how the emulator and the input
 * router get tested without a server (sec 12.3). That fake lives in
 * tests/, not here -- the shipping binary has no business carrying a
 * pretend SSH backend.
 *
 * There is no local PTY anywhere in this picture, on either platform. The
 * SSH channel IS the byte stream that feeds the emulator (sec 3, core
 * principle 1).
 */
class bssh_ssh_session : public QObject {
	Q_OBJECT

public:
	/*
	 * Where this one session is, per project.md sec 5.3. The session only
	 * reports its state; the machine that drives reconnection -- backoff,
	 * jitter, network-change handling -- is bssh_conn_manager at M5, and
	 * deliberately not in here.
	 */
	enum state_t {
		DISCONNECTED,
		RESOLVING,
		CONNECTING,
		AUTHENTICATING,
		CONNECTED,
		FAILED
	};
	Q_ENUM(state_t)

	explicit bssh_ssh_session(QObject *parent = nullptr);
	~bssh_ssh_session() override;

	/*
	 * Builds the concrete transport. Defined by whichever backend is
	 * compiled in, which is how callers create a session without naming
	 * -- or including a header from -- the SSH library in use.
	 */
	static bssh_ssh_session *create(QObject *parent = nullptr);

	state_t state() const { return m_state; }

	/*
	 * Named connect_to_host/disconnect_from_host rather than the obvious
	 * connect/disconnect: those two names are QObject's, and shadowing
	 * them in a QObject subclass is a trap for every caller that then has
	 * to qualify the base-class version.
	 */
	virtual void connect_to_host(const QString &host, quint16 port, const QString &user) = 0;
	virtual void authenticate_password(const QString &password) = 0;

	/* cols/rows, not pixels -- the remote side lays out a character grid. */
	virtual void open_shell(int cols, int rows) = 0;
	virtual void resize(int cols, int rows) = 0;

	virtual void send(const QByteArray &bytes) = 0;
	virtual void disconnect_from_host() = 0;

	/* Answers host_key_unknown/host_key_changed: trust it and continue. */
	virtual void accept_host_key() = 0;

signals:
	void bytes_received(const QByteArray &bytes);
	void state_changed(bssh_ssh_session::state_t state);
	void failed(const QString &reason);

	/*
	 * The remote shell ended by itself -- the user typed `exit`, or the
	 * command finished. This is NOT a dropped connection, and everything
	 * downstream has to be able to tell the difference: reconnecting a
	 * shell somebody deliberately exited reopens it forever.
	 */
	void shell_exited(int exit_status);

	/*
	 * Key authentication did not get us in, so a password is needed.
	 * Emitted instead of assuming one: prompting before trying a key is
	 * how a client asks for a credential the user does not need to give,
	 * and it is what every other SSH client gets right.
	 */
	void password_required();

	/*
	 * The host is not in our known-hosts store. The connection is HELD
	 * open, not failed: nothing continues until someone answers
	 * accept_host_key() or disconnects. Never accepted silently (sec 11).
	 */
	void host_key_unknown(const QString &fingerprint);

	/*
	 * The host's key is not the one we recorded. Same hold, different
	 * weight: this is what a man-in-the-middle looks like, and it is also
	 * what a reinstalled server looks like, so it must be answered by a
	 * person rather than by a policy.
	 */
	void host_key_changed(const QString &fingerprint);

protected:
	void set_state(state_t state);

private:
	state_t m_state;
};

#endif /* BSSH_SSH_SESSION_H */
