#ifndef BSSH_SSH_CONFIG_IMPORT_H
#define BSSH_SSH_CONFIG_IMPORT_H

#include <QDialog>
#include <QList>

#include "model/ssh_config.h"

class QLabel;
class QListWidget;
class QPushButton;
class bssh_profile_store;

/*
 * bssh_ssh_config_import -- choosing what to take from an OpenSSH config
 * (project.md sec 10.5).
 *
 * A dialog rather than a one-shot action because a real `~/.ssh/config`
 * is not a list of hosts someone wants in a terminal app. It accumulates
 * over years: machines that no longer exist, one-off tunnels, aliases for
 * git forges that are not shells at all. Importing all of it and leaving
 * the user to prune the result is the rude version of this feature.
 *
 * So: everything found, listed, with what it will actually connect to,
 * and the ones this app cannot honour completely said out loud rather
 * than discovered at connect time. Nothing is written until OK.
 */
class bssh_ssh_config_import : public QDialog {
	Q_OBJECT

public:
	bssh_ssh_config_import(bssh_profile_store *store, QWidget *parent = nullptr);

	/*
	 * Runs the whole gesture: pick a file, parse it, offer what is in it.
	 * Returns how many hosts were added, or -1 if the user backed out at
	 * any point -- which is not an error and should not be reported as
	 * one.
	 */
	static int run(bssh_profile_store *store, QWidget *parent);

	void set_hosts(const QList<bssh_ssh_config_host_t> &hosts);
	int import_selected();

private slots:
	void update_summary();

private:
	bssh_profile_store *m_store;
	QListWidget *m_list;
	QLabel *m_summary;
	QLabel *m_caveat;
	QPushButton *m_ok;
	QList<bssh_ssh_config_host_t> m_hosts;
};

#endif /* BSSH_SSH_CONFIG_IMPORT_H */
