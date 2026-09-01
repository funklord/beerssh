#ifndef BSSH_PROFILE_STORE_H
#define BSSH_PROFILE_STORE_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "model/preset.h"
#include "model/profile.h"

/*
 * bssh_profile_store -- saved hosts, presets and layout, in INI files
 * (project.md sec 10.6).
 *
 * Three files in the app's config directory, and all three are meant to be
 * opened in an editor:
 *
 *   hosts.ini     [host-3]   name/host/user/port/preset, plus any
 *                            per-field overrides of the preset
 *   presets.ini   [servers]  the preset fields, section named by slug
 *   layout.ini    [layout]   which hosts were open, and how
 *
 * This replaced a SQLite database. The data is tens of rows with no query
 * that is not a full scan and no concurrent writer, so what the database
 * was actually costing was the ability to read, diff, copy between
 * machines or fix by hand the configuration of a terminal -- for a tool
 * whose users live in text files. Migrations become "read the old key if
 * the new one is absent", which is what a config file has always done.
 *
 * Secrets are NOT here and must never be (sec 11). A host carries
 * has_password, a boolean saying whether a credential exists in the
 * platform store; the credential itself stays in the keyring or the
 * encrypted file. That separation is what makes these files safe to copy
 * between machines, and it is the same rule that keeps a host or user out
 * of a preset (sec 10.3).
 */
class bssh_profile_store : public QObject {
	Q_OBJECT

public:
	/*
	 * An empty path uses the per-user config directory. Tests pass their
	 * own, which is the main reason it is a parameter at all. A path
	 * ending in .db is taken as the old database's location and its
	 * DIRECTORY is used, so a caller that has not been updated still
	 * writes its config somewhere sensible rather than failing to open.
	 */
	explicit bssh_profile_store(const QString &path = QString(), QObject *parent = nullptr);

	/*
	 * The one store over the app's own config directory (sec 8.4).
	 *
	 * Same rule as bssh_session_registry::shared() and for a sharper
	 * reason: the registry shared so a session opened in one window is
	 * visible in another, and this shares so a host saved in one window is
	 * not DELETED by the next save in another. Hosts live in a QList
	 * loaded at construction, and write_profiles() clears hosts.ini and
	 * writes that list back whole -- so two stores over one file are two
	 * full-file writers with no reader between them, and the second write
	 * erases whatever the first added. Measured, not feared: two windows,
	 * a host saved through each, and only the second survived on disk.
	 *
	 * The id collision is the half that reaches credentials. A new id is
	 * highest+1 over the store's own list, so two stale lists mint the
	 * same one, and secrets are keyed "profile:<id>:password" (sec 11).
	 *
	 * Constructed on first use and never destroyed, like the registry:
	 * windows hold it and outlive nothing, and tearing it down at
	 * static-destruction time would race whatever is still reading rows.
	 *
	 * The path-taking constructor above stays public and is what tests
	 * use; this is only for the app's real one.
	 */
	static bssh_profile_store *shared();
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

	/* --- presets (sec 10) ------------------------------------------ */

	QList<bssh_preset_t> presets() const;
	bssh_preset_t preset(int id) const;

	int save_preset(const bssh_preset_t &preset);

	/*
	 * Refuses while any host still follows it. Deleting a preset out from
	 * under seven hosts is not a decision to make silently -- the caller
	 * is expected to have shown hosts_using() first (sec 10.1).
	 */
	bool remove_preset(int id);

	/*
	 * How many hosts follow this preset. The number behind "used by 7
	 * hosts" -- the warning that has to appear BEFORE a save, because
	 * propagation is the feature and the surprise at the same time.
	 */
	int hosts_using(int preset_id) const;

	bool save_layout(const layout_state_t &layout);

	/* Ids that no longer exist are dropped -- a profile can be deleted
	 * between one run and the next. */
	layout_state_t layout() const;

	/*
	 * A name nothing else in the store is already using, for imports
	 * (sec 10.5).
	 *
	 * Here, beside unique_slug, because it is the same job one layer up:
	 * that one keeps the storage KEY distinct, this one keeps what the
	 * user reads distinct. Three call sites each spelled their own
	 * version -- append " (imported)" once, without re-checking -- and
	 * once is not enough. Importing the same preset three times produced
	 * three rows all called "BSD (imported)", distinguishable only by a
	 * slug nobody is shown, and sec 10.4 will not let a preset a host
	 * still follows be deleted, so tidying up meant guessing.
	 */
	QString unique_preset_name(const QString &wanted) const;
	QString unique_profile_name(const QString &wanted) const;

signals:
	void changed();


private:
	bool open(const QString &directory);
	void load();
	void load_presets();
	void load_profiles();
	/*
	 * False when the file could not be written -- a read-only home, a
	 * full disk, or a config directory somebody's permissions do not
	 * allow. QSettings reports that through status() and nothing asked,
	 * so a save that never reached the disk returned an id and emitted
	 * changed() exactly like one that had.
	 */
	bool write_presets();
	bool write_profiles();
	void sort_profiles();
	void sort_presets();

	QString hosts_path() const;
	QString presets_path() const;
	QString layout_path() const;

	/*
	 * Presets are addressed by slug in the files and by id in memory. The
	 * slug is what a host refers to and what a person reads; the id is
	 * what the UI holds while a dialog is open. This is the map between
	 * them, rebuilt on every load.
	 */
	QString unique_slug(const QString &base, int except_id) const;
	void migrate_hosts_if_needed();

	/* See the constructor: only the app's own store may use ~/.ssh. */
	bool m_uses_default_location = false;
	int id_for_slug(const QString &slug) const;

	QString m_directory;
	QString m_last_error;
	bool m_open;

	QList<bssh_profile_t> m_profiles;
	QList<bssh_preset_t> m_presets;
	QHash<int, QString> m_slugs;
};

/*
 * sec 11's locked-store half.
 *
 * Deleting a host erases its password, and on a libsecret store that
 * always works. The passphrase-derived file cannot erase while locked --
 * it re-encrypts the whole blob -- and prompting for a passphrase in
 * order to DELETE something is the wrong trade: the user asked to remove
 * a host, not to prove who they are.
 *
 * So the key is written down instead, and swept the next time the store
 * is unlocked for any reason. That moment arrives on its own: the store
 * is unlocked to release a remembered password, which is something the
 * user does routinely and never for this. Nothing has to be asked of
 * them, and the orphan does not outlive their next connection to a host
 * they saved a password for.
 *
 * Both are no-ops where there is nothing to do, so callers need not ask
 * first.
 */
bool bssh_note_orphaned_secret(const QString &key);
void bssh_sweep_orphaned_secrets();

#endif /* BSSH_PROFILE_STORE_H */
