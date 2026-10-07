#ifndef BSSH_SSHD_H
#define BSSH_SSHD_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

struct bssh_sshd_config_t {
	/* The directory served as "/". */
	QByteArray root;

	/* Created, as ed25519, if it does not exist. */
	QString host_key_path;

	/* OpenSSH's format; re-read at every login, so an edit applies to the next one. */
	QString authorized_keys_path;

	QByteArray bind_address = "0.0.0.0";

	/* 0 asks the system for any free port; port() says which. */
	int port = 8022;

	/* A connection beyond this is closed at once, before key exchange. */
	int max_connections = 4;
};

/*
 * beersshd's server: SFTP and nothing else, inside one directory, to keys
 * listed in one file (project.md sec 19).
 *
 * No shell, no exec, no forwarding, no password. Modern scp runs over
 * SFTP and so works; `scp -O`, the legacy protocol, is an exec request
 * and is refused with the rest.
 *
 * A thread accepts, and each connection gets its own thread for the life
 * of the connection -- libssh's SFTP server reads the channel blocking,
 * and the connection limit is what bounds the threads.
 */
class bssh_sshd : public QObject {
	Q_OBJECT

public:
	explicit bssh_sshd(QObject *parent = nullptr);
	~bssh_sshd() override;

	/* Listens. On failure nothing is left running and `error` says why. */
	bool start(const bssh_sshd_config_t &config, QString *error);

	/*
	 * Stops listening and drops every connection; returns once all have
	 * ended. Safe from any thread, and against a start() on another: on
	 * Android the time limit arrives on the platform's main thread, which
	 * is not Qt's, and has to stop the server there and then (sec 19.5).
	 */
	void stop();

	bool running() const { return m_running.load(); }
	int port() const { return m_port; }
	int connections() const { return m_live.load(); }

	/* "SHA256:..." as ssh(1) prints it, for the user to check on first connect. */
	QString host_key_fingerprint() const { return m_fingerprint; }

	const bssh_sshd_config_t &config() const { return m_config; }

	/*
	 * How many lines of an authorized-keys file would let a key in, by
	 * the same parse a login uses -- so a line with options, which a
	 * login skips, is not counted either.
	 */
	static int usable_keys(const QByteArray &text);

	/* Opaque; named here only so libssh's C callbacks can reach it. */
	struct connection_t;

signals:
	void connections_changed(int count);

	/* One line about a connection: who logged in, what was refused. */
	void logged(const QString &line);

private:
	void accept_loop();
	void serve(connection_t *connection);
	void reap(bool all);

	bssh_sshd_config_t m_config;
	void *m_bind = nullptr;   /* ssh_bind, kept out of this header */
	int m_port = 0;
	QString m_fingerprint;

	int m_wake[2] = {-1, -1};
	std::atomic<bool> m_stopping{false};
	std::thread m_accepting;
	std::atomic<bool> m_running{false};
	std::mutex m_lifecycle;   /* start() and stop() against each other */

	std::mutex m_mutex;
	std::list<std::unique_ptr<connection_t>> m_connections;
	std::atomic<int> m_live{0};
};

#endif /* BSSH_SSHD_H */
