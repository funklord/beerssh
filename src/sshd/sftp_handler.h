#ifndef BSSH_SFTP_HANDLER_H
#define BSSH_SFTP_HANDLER_H

#include <QSet>

/*
 * libssh declares its server API only under WITH_SERVER. Set here, in the
 * source, rather than by each build system: qmake carried it and fmake,
 * which reads no .pro, built this file without it and failed.
 */
#ifndef WITH_SERVER
#define WITH_SERVER
#endif
#include <libssh/sftp.h>

#include "sshd/root_path.h"

/*
 * The SFTP requests of one channel, answered inside one root (project.md
 * sec 19).
 *
 * Written rather than taken from libssh, whose default handlers pass the
 * client's path straight to open(2) -- no root at all -- implement
 * symlink creation, and answer rename with "unsupported". Here every
 * path goes through bssh_root_path, links are neither made nor read, and
 * rename works, since a client uploading to a temporary name and
 * renaming into place is the common case rather than the exotic one.
 *
 * Not thread-safe; one per channel, driven by the connection's thread.
 */
class bssh_sftp_handler {
public:
	bssh_sftp_handler(sftp_session sftp, const bssh_root_path &root);
	~bssh_sftp_handler();

	bssh_sftp_handler(const bssh_sftp_handler &) = delete;
	bssh_sftp_handler &operator=(const bssh_sftp_handler &) = delete;

	/*
	 * Answers one request. False only when the reply could not be sent,
	 * which means the channel is gone; a refused request is still a
	 * request answered.
	 */
	bool handle(sftp_client_message msg);

	/* Largest data reply to a read: a client asking for more gets less, which SFTP allows. */
	static constexpr int READ_LIMIT = 64 * 1024;

private:
	struct open_t;

	bool reply_errno(sftp_client_message msg, int error);
	bool reply_ok(sftp_client_message msg);
	open_t *lookup(sftp_client_message msg);

	bool do_open(sftp_client_message msg);
	bool do_close(sftp_client_message msg);
	bool do_read(sftp_client_message msg);
	bool do_write(sftp_client_message msg);
	bool do_opendir(sftp_client_message msg);
	bool do_readdir(sftp_client_message msg);
	bool do_stat(sftp_client_message msg, bool fstat);
	bool do_setstat(sftp_client_message msg, bool fsetstat);
	bool do_mkdir(sftp_client_message msg);
	bool do_remove(sftp_client_message msg, bool directory);
	bool do_rename(sftp_client_message msg, bool overwrite);
	bool do_realpath(sftp_client_message msg);
	bool do_extended(sftp_client_message msg);

	sftp_session m_sftp;
	const bssh_root_path &m_root;
	QSet<open_t *> m_open;
};

#endif /* BSSH_SFTP_HANDLER_H */
