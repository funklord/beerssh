#ifndef BSSH_SSHD_DEMO_H
#define BSSH_SSHD_DEMO_H

#include <QTemporaryDir>

#include "sshd/sshd_control.h"
#include "sshd/sshd_platform.h"

/*
 * What beersshd shows for a store screenshot (store_view, the play-store
 * skill): a server serving, keys and a log, all made up, so the listing
 * never publishes the user's own keys, address or connections. A demo
 * platform with its own throwaway data directory, and a control that
 * says it is serving without listening anywhere.
 */
class bssh_sshd_demo_platform : public bssh_sshd_platform {
	Q_OBJECT

public:
	bssh_sshd_demo_platform();

	QString storage_root() const override { return QStringLiteral("/storage/emulated/0"); }
	bool has_storage_access() const override { return access; }
	void request_storage_access() override {}
	QHostAddress listen_address() const override;
	void set_serving(bool, const QString &) override {}
	QString data_directory() const override { return m_dir.path(); }
	bool switched_on() const override { return true; }
	bool can_start_at_boot() const override { return true; }
	bool start_at_boot() const override { return true; }

	/* False for the first-run view, which asks for All files access. */
	bool access = true;

private:
	QTemporaryDir m_dir;
};

class bssh_sshd_demo_control : public bssh_sshd_control {
	Q_OBJECT

public:
	using bssh_sshd_control::bssh_sshd_control;

	bool active() const override { return serving; }
	QString status_line() const override;
	QString fingerprint() const override;
	void switch_on() override {}
	void switch_off() override {}
	void follow() override {}

	/* The log lines, emitted once the window is listening. */
	void tell();

	/* False for the first-run view: stopped, waiting for access. */
	bool serving = true;
};

#endif /* BSSH_SSHD_DEMO_H */
