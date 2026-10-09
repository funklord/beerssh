#ifndef BSSH_SSHD_TEST_H
#define BSSH_SSHD_TEST_H

#include <QObject>
#include <QTemporaryDir>

#include <memory>

#include "sshd/sshd.h"

/*
 * beersshd (project.md sec 19): the root a client is confined to, and the
 * server driven by OpenSSH's own sftp, scp and ssh -- a second witness
 * for every encoding, since a client written here would share whatever
 * this server got wrong.
 *
 * The live half skips when the OpenSSH client is not installed.
 */
class sshd_test : public QObject {
	Q_OBJECT

private slots:
	void init();
	void cleanup();

	void a_client_path_never_climbs_above_the_root();
	void a_symlink_out_of_the_root_is_not_followed();
	void the_root_itself_is_never_removed_or_replaced();

	void files_go_up_and_down_inside_the_root();
	void nothing_outside_the_root_is_reachable_over_the_wire();
	void only_a_listed_key_logs_in();
	void a_password_is_stored_as_a_salted_hash();
	void a_password_logs_in_once_it_is_set();
	void a_key_with_options_is_not_honoured_without_them();
	void sftp_is_all_it_serves();
	void scp_finishes_with_a_clean_exit();

private:
	bool start_server();
	int client(const QString &program, const QStringList &args, const QByteArray &input = QByteArray(),
	           QByteArray *output = nullptr, const QStringList &environment = QStringList());
	QStringList client_options(const QString &key) const;

	std::unique_ptr<QTemporaryDir> m_dir;
	std::unique_ptr<bssh_sshd> m_server;
	QString m_root;
	QString m_outside;
	QString m_config;
};

#endif /* BSSH_SSHD_TEST_H */
