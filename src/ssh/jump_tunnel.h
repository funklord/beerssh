#ifndef BSSH_JUMP_TUNNEL_H
#define BSSH_JUMP_TUNNEL_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QThread>

#include <libssh/libssh.h>

/*
 * bssh_jump_tunnel -- reaching a host through a bastion (project.md
 * sec 10.5's ProxyJump).
 *
 * What OpenSSH's -J does, and why it is not the same as logging into the
 * bastion and typing ssh again: the SSH protocol to the TARGET runs
 * end-to-end inside a plain TCP forward that the bastion carries and
 * cannot read. The target's host key is checked by this client, the
 * target's authentication is done by this client, and no shell is ever
 * opened on the bastion. Hopping by hand gives the middle machine your
 * session; this does not.
 *
 * The shape libssh forces:
 *
 *   bastion session --> direct-tcpip channel --> target:port
 *                              ||
 *                          socketpair
 *                              ||
 *   target session (SSH_OPTIONS_FD) ---------> the app
 *
 * libssh will not take a channel as a session's transport, only a file
 * descriptor, so a socketpair sits between them and this class shuttles
 * bytes across. That shuttling has to happen on its own thread: the
 * target's ssh_connect() BLOCKS through its handshake, and every byte of
 * that handshake travels through this tunnel -- pumping it from the same
 * thread would deadlock on the first packet.
 *
 * One hop only. OpenSSH allows a comma-separated chain and the same
 * mechanism nests, but each hop needs its own host-key decision and its
 * own credentials, and a prompt for "the second machine on the way to the
 * third" is a design question rather than a loop.
 */
class bssh_jump_tunnel : public QThread {
	Q_OBJECT

public:
	explicit bssh_jump_tunnel(QObject *parent = nullptr);
	~bssh_jump_tunnel() override;

	/* [user@]host[:port], as an OpenSSH ProxyJump value. */
	struct hop_t {
		QString host;
		QString user;
		quint16 port = 22;
		bool valid = false;
	};

	static hop_t parse(const QString &spec, const QString &fallback_user);

	/*
	 * Opens the bastion connection and the forward to target:port, and
	 * starts pumping. Returns the descriptor to hand the target session,
	 * or -1 with why() set.
	 *
	 * known_hosts is the same file the target is checked against: a
	 * bastion is a host like any other and gets the same scrutiny.
	 */
	int open(const hop_t &hop, const QString &target_host, quint16 target_port,
	          const QString &known_hosts, const QString &identity_file);

	void close();

	/*
	 * Moving the tunnel with its session to another process (sec 8.4).
	 *
	 * pause() stops the pump between transfers -- never part-way through
	 * one -- and leaves the socket pair alone: what is in it stays in the
	 * kernel and travels with the descriptors. resume() starts it again
	 * when the move is called off. export_state() then hands back the
	 * jump host's transport and the two descriptors this side holds: the
	 * jump host's socket and the pump's end of the pair. release() lets go
	 * without a word to the jump host. adopt() is the receiving side:
	 * the jump host's session imported, the pump left stopped until
	 * resume() -- which the caller calls once nothing else can fail. The target session's end of the pair is its own, not the
	 * tunnel's, and is never closed here once handed over.
	 */
	bool pause();
	void resume();
	bool export_state(QByteArray &state, QList<int> &fds, QString &why);
	void release();
	bool adopt(const QByteArray &state, int jump_fd, int ours_fd, int theirs_fd, QString &why);

	QString why() const { return m_why; }

protected:
	void run() override;

private:
	bool authenticate();
	bool write_to_channel(const char *data, int size);
	bool drain_channel(char *buffer, int size);

	ssh_session m_session = nullptr;
	ssh_channel m_channel = nullptr;
	int m_ours = -1;      /* the end this class pumps */
	int m_theirs = -1;    /* the end the target session is given */
	QString m_why;
	volatile bool m_stop = false;
	volatile bool m_pause = false;
};

#endif /* BSSH_JUMP_TUNNEL_H */
