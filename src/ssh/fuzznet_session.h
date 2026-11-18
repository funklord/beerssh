#ifndef BSSH_FUZZNET_SESSION_H
#define BSSH_FUZZNET_SESSION_H

#include <QByteArray>
#include <QMutex>
#include <QStringList>

#include <atomic>
#include <thread>

#include "ssh/ssh_session.h"

/*
 * bssh_fuzznet_session -- a host reached over fuzznet rather than SSH
 * (project.md sec 5.6): this device paired to the host's beersshd, its words
 * to the host's UDP port, and inside them the reliable stream that carries
 * what an SSH exec channel carries to the session helper.
 *
 * Kept sessions only. beersshd --fuzznet --sessions answers nothing but the
 * helper's two commands, so a shell that does not outlive its connection --
 * the raw channel off -- is refused at open_shell rather than attempted.
 *
 * The device's calls block (fuzznet's device/ waits for its answers), so the
 * link runs on a thread of its own, and what it hears reaches this object's
 * thread queued. Linux only while fuzznet is (BSSH_FUZZNET).
 */
class bssh_fuzznet_session : public bssh_ssh_session {
	Q_OBJECT

public:
	/* `image` is the device image text fuzznet's pairing gave this device. */
	explicit bssh_fuzznet_session(const QString &image, QObject *parent = nullptr);
	~bssh_fuzznet_session() override;

	/* `host` must be an IPv4 address or a name that resolves to one. */
	void connect_to_host(const QString &host, quint16 port, const QString &user) override;
	void open_shell(int cols, int rows) override;
	void set_command(const QStringList &command) override { m_command = command; }
	void set_raw_channel(bool raw) override { m_raw = raw; }
	/* A kept session says its size in a frame; there is no channel to tell. */
	void resize(int cols, int rows) override {
		Q_UNUSED(cols);
		Q_UNUSED(rows);
	}
	void send(const QByteArray &bytes) override;
	void disconnect_from_host() override;
	QString kind() const override { return QStringLiteral("fuzznet"); }

	/*
	 * The helper's probe, "beerssh-session version", which a session asks
	 * before it attaches: asked on this link's stream and its answer handed
	 * back, the attach that follows then getting a stream of its own, as
	 * beersshd holds one per host. Anything else has no side to run on and
	 * is answered with nothing, as the base does.
	 */
	void run_on_the_side(const QString &command) override;

	/* What SSH has and fuzznet does not need: its pairing is the trust. */
	void authenticate_password(const QString &password) override { Q_UNUSED(password); }
	void set_keepalive(int seconds) override { Q_UNUSED(seconds); }
	void set_term(const QString &term) override { Q_UNUSED(term); }
	void set_compression(bool compression) override { Q_UNUSED(compression); }
	void set_identity_file(const QString &path) override { Q_UNUSED(path); }
	void set_proxy_jump(const QString &spec) override { Q_UNUSED(spec); }
	void accept_host_key() override {}
	void supply_key_passphrase(const QString &passphrase) override { Q_UNUSED(passphrase); }

private:
	void run(QByteArray image, QByteArray address, quint16 port);
	void start_link();
	void stop();
	/* From the link's thread, onto this object's. */
	void post_bytes(const QByteArray &bytes);
	void post_connected();
	void post_failed(const QString &reason);
	void queue(const QByteArray &bytes);

	QString m_image;
	QString m_host;
	quint16 m_port = 0;
	QStringList m_command;
	bool m_raw = false;

	/* A probe asked and not yet answered, what of its answer has come, and
	 * whether the stream it used is spent. This object's thread only. */
	bool m_probing = false;
	QByteArray m_probe_answer;
	bool m_stream_spent = false;
	/* A shell is open: what send() is given goes to it. Before that, as on an
	 * SSH connection with no channel yet, it goes nowhere. */
	bool m_shell_open = false;

	std::thread m_worker;
	std::atomic<bool> m_stopping{ false };
	QMutex m_lock;
	QByteArray m_outgoing; /* written by send(), taken by the link's thread */
};

#endif /* BSSH_FUZZNET_SESSION_H */
