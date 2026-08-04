#ifndef BSSH_JUMP_TUNNEL_H
#define BSSH_JUMP_TUNNEL_H

#include <QByteArray>
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
};

#endif /* BSSH_JUMP_TUNNEL_H */
