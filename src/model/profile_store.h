#ifndef BSSH_PROFILE_STORE_H
#define BSSH_PROFILE_STORE_H

#include <QList>
#include <QObject>
#include <QString>

#include "model/profile.h"

/*
 * bssh_profile_store -- saved hosts, in SQLite (project.md sec 10.6).
 *
 * SQLite rather than a settings file because sec 10.6 says so and because
 * of where this ends up: profiles, presets, layout state and known-hosts
 * in one place, with migrations from day one. Users accumulate profiles
 * they care about, and a store that cannot be migrated is a store that
 * eventually has to be thrown away.
 *
 * Secrets are NOT here. The database holds an id; the secret store holds
 * the material (sec 11). On desktop this file is an ordinary user file --
 * visible, backed up, openable in sqlite3 -- so it is written on the
 * assumption someone will read it.
 */
class bssh_profile_store : public QObject {
	Q_OBJECT

public:
	/*
	 * An empty path opens the per-user database under AppDataLocation.
	 * Tests pass their own path, and ":memory:" works too -- which is the
	 * main reason the path is a parameter at all.
	 */
	explicit bssh_profile_store(const QString &path = QString(), QObject *parent = nullptr);
	~bssh_profile_store() override;

	bool is_open() const { return m_open; }
	QString last_error() const { return m_last_error; }

	QList<bssh_profile_t> profiles() const;
	bssh_profile_t profile(int id) const;

	/* Inserts when id is 0, updates otherwise. Returns the id, or 0. */
	int save(const bssh_profile_t &profile);

	bool remove(int id);

	/*
	 * The workspace: which hosts were open, in what order, how many of
	 * them were side by side as panes, and which was active (sec 8.4).
	 *
	 * Stored as profile ids rather than session ids because sessions do
	 * not survive a restart -- plain SSH cannot resume one (sec 5.3), so
	 * what can honestly be restored is the ARRANGEMENT, not the
	 * connections.
	 */
	struct layout_state_t {
		QList<int> profile_ids;  /* tab order */
		int pane_count;          /* how many of the first N were split */
		int active_index;

		layout_state_t() : pane_count(1), active_index(0) {}
		bool is_empty() const { return profile_ids.isEmpty(); }
	};

	bool save_layout(const layout_state_t &layout);

	/* Ids that no longer exist are dropped -- a profile can be deleted
	 * between one run and the next. */
	layout_state_t layout() const;

signals:
	void changed();

private:
	bool open(const QString &path);
	bool migrate();

	QString m_connection_name;
	QString m_last_error;
	bool m_open;
};

#endif /* BSSH_PROFILE_STORE_H */
