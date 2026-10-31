#ifndef BSSH_SSH_SESSION_H
#define BSSH_SSH_SESSION_H

#include <QByteArray>
#include <QList>
#include <QStringList>

#include <functional>
#include <QMetaType>
#include <QObject>
#include <QString>

#include "ssh/working_directory.h"

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
 * test/, not here -- the shipping binary has no business carrying a
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

	/*
	 * Builds a LOCAL transport -- a shell on a pseudo-terminal rather
	 * than an SSH connection (sec 2, sec 3). Empty shell means $SHELL,
	 * then /bin/sh.
	 *
	 * Beside create() rather than a flag on it, because the two build
	 * genuinely different things and a boolean parameter at every call
	 * site reads as a mode rather than as a choice of backend. Consults
	 * the same injected factory, so a test fakes a local session exactly
	 * as it fakes an SSH one.
	 */
	static bssh_ssh_session *create_local(const QString &shell,
	                                       const QString &working_directory = QString(),
	                                       QObject *parent = nullptr);

	/*
	 * Replaces what create() builds. This is the seam sec 12.3 asks for:
	 * "a fake implementation of this one interface, replaying a captured
	 * byte stream, is how the emulator gets tested without a server". The
	 * fake existed already, but nothing could get one INTO a
	 * bssh_session, so everything between the session and the transport
	 * -- reconnection, keepalive, how a clean exit differs from a drop --
	 * was reachable only by running against a real host.
	 *
	 * Null restores the built-in backend. Tests must do that in cleanup,
	 * or the next test builds a transport out of a factory whose captured
	 * state is gone.
	 */
	static void set_factory(std::function<bssh_ssh_session *(QObject *)> factory);

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

	/*
	 * Run this instead of a shell (project.md sec 8.6's -e), argv-style
	 * and empty for the ordinary case. Set before the shell is opened;
	 * afterwards there is nothing left to decide.
	 *
	 * On the interface because both transports answer it, and they answer
	 * it the way their own tradition does. A local shell EXECS the words
	 * as given, with no shell between them, which is what xterm's -e
	 * means. SSH has no argv on the wire -- an exec request carries one
	 * string, which the far end hands to the account's shell -- so the
	 * words are joined with spaces there, which is what `ssh host ls -l`
	 * does and is why that one needs quoting and the local one does not.
	 * The difference is the protocols', not ours, and hiding it would
	 * mean pretending one of them can do what it cannot.
	 */
	virtual void set_command(const QStringList &command) = 0;
	virtual void resize(int cols, int rows) = 0;

	virtual void send(const QByteArray &bytes) = 0;
	virtual void disconnect_from_host() = 0;

	/*
	 * How often to prove the connection is still there, in seconds; 0
	 * disables it (project.md sec 4, sec 5.3).
	 *
	 * This is on the interface rather than inside one implementation
	 * because it is a property of the connection, not of the library: a
	 * libssh2 backend would satisfy it with libssh2_keepalive_config, and
	 * the fake used by the tests satisfies it by recording the value.
	 *
	 * The interval belongs to the host's preset (keepalive_seconds), so a
	 * jump box on a hostile NAT can be told to speak up more often than a
	 * machine on the LAN.
	 */
	virtual void set_keepalive(int seconds) = 0;

	/*
	 * The TERM string the remote is told about, and whether to ask for
	 * compression (sec 10.2). Both come from the host's preset and both
	 * must be set BEFORE the shell is opened -- TERM is sent with the PTY
	 * request, and compression is negotiated during the handshake, so
	 * neither can be changed on a live connection.
	 *
	 * TERM matters more than it looks: it is the whole contract with the
	 * remote about what this terminal can do, and a switch that only
	 * understands "vt100" gets a different answer from a modern box.
	 */
	virtual void set_term(const QString &term) = 0;
	virtual void set_compression(bool compression) = 0;

	/*
	 * A specific private key to offer, or empty for the default
	 * identities (sec 10.5, sec 11). Set before authentication; it is an
	 * ADDITION to what would be tried anyway rather than a restriction,
	 * which is what OpenSSH's IdentityFile means in the absence of
	 * IdentitiesOnly.
	 *
	 * On the interface for the same reason keepalive is: it is a property
	 * of the connection rather than of the library. This is the field an
	 * imported `~/.ssh/config` fills in -- without it, a host whose key is
	 * not one of the four default names silently falls through to a
	 * password prompt it may have no way to answer.
	 *
	 * The PATH only. The key itself is never read here, never copied into
	 * the profile store, and never leaves the disk it lives on.
	 */
	virtual void set_identity_file(const QString &path) = 0;

	/*
	 * The bastion to reach the host through, as OpenSSH's ProxyJump
	 * writes it, or empty for a direct connection (sec 10.5). Set before
	 * connecting -- the route to a host is chosen when the connection is
	 * made and cannot be changed under a live one.
	 *
	 * The SSH session to the host runs end-to-end INSIDE the tunnel: the
	 * host key checked, the credentials offered, and the shell opened are
	 * all the target's, and the bastion carries bytes it cannot read.
	 * That is the whole point of ProxyJump over logging in and typing
	 * ssh again, and it is why this is a transport option rather than a
	 * different way of spelling the host.
	 */
	virtual void set_proxy_jump(const QString &spec) = 0;

	/* Answers host_key_unknown/host_key_changed: trust it and continue. */
	virtual void accept_host_key() = 0;

	/* Answers key_passphrase_required. */
	virtual void supply_key_passphrase(const QString &passphrase) = 0;

	/*
	 * Moving this live connection to another process (project.md sec 8.4).
	 *
	 * The giving side calls export_transport(), which stops the connection
	 * between packets and hands back its state and the descriptors that
	 * carry it -- the SSH socket, or the pty master. Nothing is sent and
	 * nothing is consumed, so if the move does not happen,
	 * resume_transport() carries on as before. Once the receiver has it,
	 * release_transport() lets go without sending anything: no
	 * disconnect, no channel close, no SIGHUP. The connection is the
	 * receiver's now.
	 *
	 * The receiving side makes an empty transport of the same kind() with
	 * create_for_import() and calls import_transport() before anything
	 * else. On success it is CONNECTED and the descriptors are its own; on
	 * failure they are still the caller's to close.
	 *
	 * A kind() of "" cannot move: the default for a transport that has
	 * not been taught to, which says so rather than pretending.
	 */
	virtual QString kind() const { return QString(); }
	virtual bool export_transport(QByteArray &state, QList<int> &fds, QString *why);
	virtual void release_transport() {}
	virtual void resume_transport() {}
	virtual bool import_transport(const QByteArray &state, const QList<int> &fds, QString *why);
	static bssh_ssh_session *create_for_import(const QString &kind, QObject *parent = nullptr);

	/*
	 * Runs a short command beside the shell, never in it, and answers with
	 * side_output_ready(): what it printed on stdout, at most 64 KiB, after
	 * it ends or after five seconds. Over SSH a second channel of the same
	 * connection; locally a process. What lists tmux and screen sessions
	 * on connect (model/multiplexer.h).
	 *
	 * The default answers empty, a turn of the event loop later: a
	 * transport that cannot run one says nothing ran, not something did.
	 */
	virtual void run_on_the_side(const QString &command);

	/*
	 * Ends the shell and starts this command in its place, on the same
	 * connection, at the same size -- without announcing an exit, since
	 * nothing ended as far as the session is concerned. How a tab
	 * switches to another tmux or screen session (sec 10.2a): the old
	 * client sees its terminal hang up and detaches, and the new command
	 * attaches. Words as set_command() takes them.
	 *
	 * The default does nothing: a transport that cannot is one where the
	 * drawer's choice has no effect, which is better than a guess.
	 */
	virtual void restart_shell(const QStringList &command) { Q_UNUSED(command); }

	/*
	 * Where the shell on the other end is, for a file dropped on its
	 * terminal (sec 6.4); answered by working_directory_found(). Over SSH
	 * the probe in ssh/working_directory.h on a channel of its own, so it
	 * never waits on the tmux listing; locally the child's /proc. The
	 * default answers that nothing is known, a turn later.
	 */
	virtual void find_working_directory();

	/*
	 * Copies local files and directories into `directory` on the other
	 * end -- empty for the home directory -- never replacing what is
	 * there (bssh_free_name). Over SSH by SFTP on this connection, a piece
	 * at a time so the shell beside it keeps running; locally a copy on
	 * a thread of its own. upload_progress() as it goes, upload_finished()
	 * once, with the names it wrote. The default refuses.
	 */
	virtual void upload(const QStringList &paths, const QString &directory);

signals:
	void working_directory_found(const bssh_working_directory_t &found);
	void upload_progress(qint64 done, qint64 total);
	void upload_finished(bool ok, const QString &directory, const QStringList &written,
	                     const QString &why);

	/* What run_on_the_side() printed; empty when it printed nothing or failed. */
	void side_output_ready(const QByteArray &output);

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
	 * The server refused the password just offered. The connection stays
	 * open for another: a refusal is not an outage, and failing the
	 * connection here is what let the reconnect machinery dial again and
	 * offer the same wrong password, ten times, to a server counting
	 * failures -- the way to be blackholed (sec 11).
	 */
	void password_rejected();

	/*
	 * A private key exists but did not get us in unaided, which usually
	 * means it is passphrase-protected and no agent is loaded. Asked
	 * before falling back to a password, because unlocking the key is
	 * what the user almost certainly wants.
	 */
	void key_passphrase_required();

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

	/*
	 * The host's key appears in a known-hosts file marked @revoked
	 * (sec 11). Not a prompt and never offered as one: somebody went out
	 * of their way to mark this key as no longer trusted, and the answer
	 * to "shall I trust it anyway?" is not the app's to ask. libssh
	 * reports such a key as merely unknown, so this is detected here
	 * rather than taken from it.
	 */
	void host_key_revoked(const QString &fingerprint);

protected:
	void set_state(state_t state);

	/* Null unless a test installed a factory; create() consults it first. */
	static bssh_ssh_session *create_from_factory(QObject *parent);

private:
	state_t m_state;
};

Q_DECLARE_METATYPE(bssh_working_directory_t)

#endif /* BSSH_SSH_SESSION_H */
