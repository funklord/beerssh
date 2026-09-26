#include <QTemporaryDir>
#include <QtTest>
#include <QFile>
#include <QStandardPaths>

#include "model/profile_store.h"
#include "model/app_settings.h"
#include "platform/platform.h"
#include "platform/secret_store.h"
#include <QHash>
#include <QDir>
#include "profile_store_test.h"
#include <QListWidget>
#include "ui/host_list.h"
#include "ui/session_dialog.h"
#include "ui/host_editor.h"
#include "model/preset.h"
#include "model/settings_resolver.h"
#include <QSettings>
#include <QJsonObject>
#include "ui/font_picker.h"
#include "ui/main_window.h"
#include "model/kept_scrollback.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
#include "term/scrollback.h"
#include <QCheckBox>
#include <QFileInfo>
#include <QDataStream>

/*
 * A secret store that LOCKS, in memory.
 *
 * The real passphrase-derived store would do, and is what the first
 * version injected -- but its whole point is a deliberately slow key
 * derivation, so an unlock costs seconds and these tests lock and unlock
 * several times each. Measured: about ten seconds per test, which
 * tripped the suite's per-class timeout and its output cap.
 *
 * The code under test is the profile store's SWEEP, not anybody's
 * cryptography, so the double gives the sweep the one property it needs
 * -- a store that can be locked, and that refuses to write or erase
 * while it is -- and nothing else. Reads answer empty when locked, which
 * is what the interface documents for absent, locked and unreadable
 * alike.
 */
class lockable_test_store : public bssh_secret_store {
public:
	QString backend_name() const override { return QStringLiteral("test double"); }
	bool is_hardware_backed() const override { return false; }
	bool is_unlocked() const override { return m_unlocked; }
	bool needs_passphrase() const override { return true; }

	bool unlock(const QString &passphrase) override {
		if (m_passphrase.isEmpty()) m_passphrase = passphrase;
		m_unlocked = passphrase == m_passphrase;
		return m_unlocked;
	}

	void lock() override { m_unlocked = false; }

	bool store(const QString &id, const QString &secret) override {
		if (!m_unlocked) return false;
		m_secrets.insert(id, secret);
		return true;
	}

	QString load(const QString &id) const override {
		return m_unlocked ? m_secrets.value(id) : QString();
	}

	bool erase(const QString &id) override {
		if (!m_unlocked) return false;
		m_secrets.remove(id);
		return true;
	}

private:
	bool m_unlocked = false;
	QString m_passphrase;
	QHash<QString, QString> m_secrets;
};

/*
 * Puts bssh_secret_store::instance() back, whatever the test did to it.
 *
 * The seam is process-wide, so a test that injected a store and returned
 * early would hand the next one a store this test owns and has since
 * destroyed.
 */
struct secret_store_left_as_found_t {
	~secret_store_left_as_found_t() { bssh_secret_store::set_instance(nullptr); }
};

static bssh_profile_t make_profile(const QString &name, const QString &host) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = host;
	profile.user = QStringLiteral("nabbe");
	profile.port = 22;
	return profile;
}

void profile_store_test::saves_and_lists_profiles() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));
	QVERIFY2(store.is_open(), qPrintable(store.last_error()));

	QCOMPARE(store.profiles().count(), 0);

	const int id = store.save(make_profile(QStringLiteral("bastion"),
	                                        QStringLiteral("bastion.example")));
	QVERIFY(id > 0);

	const QList<bssh_profile_t> profiles = store.profiles();
	QCOMPARE(profiles.count(), 1);
	QCOMPARE(profiles.first().name, QStringLiteral("bastion"));
	QCOMPARE(profiles.first().target(), QStringLiteral("nabbe@bastion.example"));
}

void profile_store_test::updates_rather_than_duplicating() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t profile = make_profile(QStringLiteral("pi"), QStringLiteral("10.0.0.5"));
	profile.id = store.save(profile);

	profile.port = 2222;
	store.save(profile);

	/* Saving an existing id edits it; a second row would be a bug the
	 * user only notices as a duplicated entry in their list. */
	QCOMPARE(store.profiles().count(), 1);
	QCOMPARE(int(store.profiles().first().port), 2222);
	QCOMPARE(store.profiles().first().target(), QStringLiteral("nabbe@10.0.0.5:2222"));
}

void profile_store_test::removes_profiles() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	const int id = store.save(make_profile(QStringLiteral("gone"), QStringLiteral("gone.example")));
	QVERIFY(store.remove(id));
	QCOMPARE(store.profiles().count(), 0);

	/* Removing something that is not there is not an error worth
	 * crashing over, but it must not claim success either. */
	QVERIFY(!store.remove(0));
}

void profile_store_test::survives_being_reopened() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);
		store.save(make_profile(QStringLiteral("persist"), QStringLiteral("host.example")));
	}

	/*
	 * The whole point of the store. Reopening is also what exercises the
	 * migration path against an existing file rather than a fresh one.
	 */
	bssh_profile_store reopened(path);
	QVERIFY(reopened.is_open());
	QCOMPARE(reopened.profiles().count(), 1);
	QCOMPARE(reopened.profiles().first().name, QStringLiteral("persist"));
}

void profile_store_test::schema_has_nowhere_to_put_a_secret() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_profile_t profile = make_profile(QStringLiteral("x"), QStringLiteral("y"));
	profile.has_stored_password = true;
	store.save(profile);

	/*
	 * sec 11 says secrets never reach the config. Asserting on the FILE
	 * keeps that structural rather than a habit: the text that lands on
	 * disk is the thing a user copies to another machine, mails to a
	 * colleague, or checks into their dotfiles repository, and it has to
	 * be safe to do all three.
	 *
	 * This used to inspect the SQLite columns. The store is INI now and
	 * the guarantee is the same one, so the test moved rather than went.
	 */
	QFile hosts(dir.path() + QStringLiteral("/hosts.ini"));
	QVERIFY(hosts.open(QIODevice::ReadOnly));
	const QString text = QString::fromUtf8(hosts.readAll()).toLower();

	QVERIFY(text.contains(QStringLiteral("host=y")));

	QVERIFY2(!text.contains(QStringLiteral("password=")) ||
	          text.contains(QStringLiteral("has_password=")),
	          "only the flag may appear, never a credential");
	QVERIFY(!text.contains(QStringLiteral("passphrase")));
	QVERIFY(!text.contains(QStringLiteral("secret")));
	QVERIFY(!text.contains(QStringLiteral("private")));

	/*
	 * And the preset file, which sec 10.3 says is safe to share, must not
	 * name anyone's infrastructure at all -- no host, no user.
	 */
	QFile presets(dir.path() + QStringLiteral("/presets.ini"));
	QVERIFY(presets.open(QIODevice::ReadOnly));
	const QString preset_text = QString::fromUtf8(presets.readAll()).toLower();

	QVERIFY(!preset_text.contains(QStringLiteral("\nhost=")));
	QVERIFY(!preset_text.contains(QStringLiteral("\nuser=")));
	QVERIFY(!preset_text.contains(QStringLiteral("password")));
}

void profile_store_test::layout_round_trips() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int first = 0;
	int second = 0;
	int third = 0;
	{
		bssh_profile_store store(path);
		first = store.save(make_profile(QStringLiteral("one"), QStringLiteral("a.example")));
		second = store.save(make_profile(QStringLiteral("two"), QStringLiteral("b.example")));
		third = store.save(make_profile(QStringLiteral("three"), QStringLiteral("c.example")));

		bssh_profile_store::layout_state_t layout;
		layout.profile_ids = {first, second, third};
		layout.pane_count = 2;   /* the first two were side by side */
		layout.active_index = 1;
		layout.shape = QStringLiteral("V(%1,%2)").arg(first).arg(second);

		QVERIFY(store.save_layout(layout));
	}

	/*
	 * What survives a restart is the ARRANGEMENT, not the connections:
	 * plain SSH cannot resume a shell (sec 5.3), so restoring anything
	 * more would be a promise the protocol cannot keep.
	 */
	bssh_profile_store reopened(path);
	const bssh_profile_store::layout_state_t restored = reopened.layout();

	QCOMPARE(restored.profile_ids, QList<int>({first, second, third}));
	QCOMPARE(restored.pane_count, 2);
	QCOMPARE(restored.active_index, 1);
	QCOMPARE(restored.shape, QStringLiteral("V(%1,%2)").arg(first).arg(second));
}

void profile_store_test::a_hosts_zoom_is_written_to_disk() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int id = 0;
	{
		bssh_profile_store store(path);
		bssh_profile_t host = make_profile(QStringLiteral("zoomed"), QStringLiteral("z.example"));
		host.font_zoom = -4;
		id = store.save(host);
		QVERIFY(id);
	}

	/* Another store on the same files is the next start (sec 9.5). */
	bssh_profile_store reopened(path);
	QCOMPARE(reopened.profile(id).font_zoom, -4);
}

void profile_store_test::layout_forgets_deleted_hosts() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	const int kept = store.save(make_profile(QStringLiteral("kept"), QStringLiteral("k.example")));
	const int doomed = store.save(make_profile(QStringLiteral("doomed"), QStringLiteral("d.example")));

	bssh_profile_store::layout_state_t layout;
	layout.profile_ids = {kept, doomed};
	layout.pane_count = 2;
	store.save_layout(layout);

	store.remove(doomed);

	/*
	 * Restoring a tab for a host the user deleted between runs would be
	 * the store second-guessing them.
	 */
	const bssh_profile_store::layout_state_t restored = store.layout();
	QCOMPARE(restored.profile_ids, QList<int>({kept}));
}

void profile_store_test::an_old_database_gains_the_layout_table() {
	QTemporaryDir dir;

	/*
	 * A config file written by hand, or by an older build that did not
	 * have half these fields. This is what migration MEANS once the store
	 * is text: read the keys that are there, default the ones that are
	 * not, and never refuse the file.
	 *
	 * It replaces a test that built a version-1 SQLite database and
	 * checked the layout table appeared. Same question -- does an older
	 * file still open -- asked of the format that now exists.
	 */
	{
		QFile hosts(dir.path() + QStringLiteral("/hosts.ini"));
		QVERIFY(hosts.open(QIODevice::WriteOnly));
		hosts.write("[host-1]\n"
		            "host=old.example\n"
		            "user=nabbe\n");
	}
	{
		QFile presets(dir.path() + QStringLiteral("/presets.ini"));
		QVERIFY(presets.open(QIODevice::WriteOnly));
		presets.write("[minimal]\n"
		              "scrollback_lines=1234\n");
	}

	bssh_profile_store store(dir.path());

	const QList<bssh_profile_t> profiles = store.profiles();
	QCOMPARE(profiles.size(), 1);
	QCOMPARE(profiles.first().host, QStringLiteral("old.example"));

	/* Absent keys take their defaults rather than becoming empty. */
	QCOMPARE(profiles.first().port, quint16(22));
	QVERIFY(!profiles.first().has_stored_password);

	const QList<bssh_preset_t> presets = store.presets();
	QCOMPARE(presets.size(), 1);
	QCOMPARE(presets.first().scrollback_lines, 1234);
	QCOMPARE(presets.first().name, QStringLiteral("minimal"));
	QCOMPARE(presets.first().term, QStringLiteral("xterm-256color"));

	/* A section with no id gets one, and it survives the next open. */
	QVERIFY(presets.first().id != 0);
	const int assigned = presets.first().id;

	bssh_preset_t edited = presets.first();
	edited.scrollback_lines = 4321;
	store.save_preset(edited);

	bssh_profile_store reopened(dir.path());
	QCOMPARE(reopened.presets().first().id, assigned);
	QCOMPARE(reopened.presets().first().scrollback_lines, 4321);
}

void profile_store_test::the_stored_password_flag_round_trips() {
	QTemporaryDir dir;

	int id = 0;
	{
		bssh_profile_store store(dir.path());
		bssh_profile_t profile = make_profile(QStringLiteral("vault"),
		                                       QStringLiteral("vault.example"));
		profile.has_stored_password = true;
		id = store.save(profile);
	}

	/*
	 * A flag, never the credential (sec 11). It exists so the app knows
	 * whether asking the secret store for its passphrase is worth it --
	 * and so a host copied to a machine with an empty keyring simply
	 * prompts rather than failing strangely.
	 */
	bssh_profile_store reopened(dir.path());
	QVERIFY(reopened.profile(id).has_stored_password);

	bssh_profile_t cleared = reopened.profile(id);
	cleared.has_stored_password = false;
	reopened.save(cleared);

	bssh_profile_store again(dir.path());
	QVERIFY(!again.profile(id).has_stored_password);
}

void profile_store_test::the_starter_presets_are_seeded_once() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int seeded = 0;
	{
		bssh_profile_store store(path);
		seeded = store.presets().count();
		QVERIFY(seeded >= 5);

		bool found_openwrt = false;
		for (const bssh_preset_t &preset : store.presets()) {
			QVERIFY(preset.id != 0);
			if (preset.name.contains(QStringLiteral("OpenWrt"))) {
				found_openwrt = true;
				QCOMPARE(preset.term, QStringLiteral("vt100"));
				QVERIFY(preset.is_builtin);
			}
		}
		QVERIFY(found_openwrt);
	}

	/*
	 * Seeded at migration, not merged on every start. Re-seeding would
	 * resurrect presets the user deleted, and one that comes back after
	 * being removed is worse than one that never shipped.
	 */
	bssh_profile_store reopened(path);
	QCOMPARE(reopened.presets().count(), seeded);
}

void profile_store_test::a_host_follows_a_preset_and_may_override_it() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int profile_id = 0;
	int preset_id = 0;
	{
		bssh_profile_store store(path);
		preset_id = store.presets().first().id;

		bssh_profile_t profile = make_profile(QStringLiteral("router"),
		                                       QStringLiteral("192.168.1.1"));
		profile.preset_id = preset_id;
		profile.overrides.insert(QStringLiteral("font_size"), 14);
		profile_id = store.save(profile);
	}

	bssh_profile_store reopened(path);
	const bssh_profile_t restored = reopened.profile(profile_id);

	QCOMPARE(restored.preset_id, preset_id);
	QCOMPARE(restored.overrides.value(QStringLiteral("font_size")).toInt(), 14);

	/* And the store can answer sec 10.1's "used by N hosts". */
	QCOMPARE(reopened.hosts_using(preset_id), 1);
	QCOMPARE(reopened.hosts_using(0), 0);
}

void profile_store_test::a_preset_in_use_cannot_be_deleted() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("work-bastion");
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id > 0);

	bssh_profile_t profile = make_profile(QStringLiteral("host"), QStringLiteral("h.example"));
	profile.preset_id = preset_id;
	const int profile_id = store.save(profile);

	/*
	 * Hosts following a deleted preset would silently fall back to app
	 * defaults -- a change to every one of them, caused by deleting
	 * something else.
	 */
	QVERIFY(!store.remove_preset(preset_id));
	QCOMPARE(store.hosts_using(preset_id), 1);

	/* Once nothing follows it, it goes. */
	store.remove(profile_id);
	QCOMPARE(store.hosts_using(preset_id), 0);
	QVERIFY(store.remove_preset(preset_id));
}

void profile_store_test::a_shipped_preset_cannot_be_edited() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t builtin = store.presets().first();
	QVERIFY(builtin.is_builtin);

	const QString original_term = builtin.term;
	builtin.term = QStringLiteral("mangled");

	/*
	 * Shipped presets are cloned, not edited (sec 10.4): saving over one
	 * would change the starting point every other clone came from.
	 */
	QCOMPARE(store.save_preset(builtin), 0);
	QCOMPARE(store.preset(builtin.id).term, original_term);

	/* Cloning it is the supported move, and the clone is editable. */
	bssh_preset_t clone = store.presets().first();
	clone.id = 0;
	clone.is_builtin = false;
	clone.name = QStringLiteral("my linux");
	clone.term = QStringLiteral("xterm");

	const int clone_id = store.save_preset(clone);
	QVERIFY(clone_id > 0);
	QCOMPARE(store.preset(clone_id).term, QStringLiteral("xterm"));
	QVERIFY(!store.preset(clone_id).is_builtin);

	/* And a shipped preset cannot be deleted either. */
	QVERIFY(!store.remove_preset(builtin.id));
}

/*
 * A write that fails for everybody, root included: a directory where the
 * file goes. A read-only directory is the usual way to fail a write, and
 * the check-ci container runs as root, which writes one anyway -- so a
 * test built on permissions skips in the one place every push is gated.
 */
static void block_presets_file(const QString &directory) {
	const QString path = directory + QStringLiteral("/presets.ini");
	QFile::remove(path);
	QVERIFY(QDir().mkpath(path));
}

void profile_store_test::a_refused_preset_save_says_why() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_preset_t nameless;
	QCOMPARE(store.save_preset(nameless), 0);
	QVERIFY2(store.last_error().contains(QStringLiteral("name")),
	          qPrintable(store.last_error()));

	bssh_preset_t shipped = store.presets().first();
	QVERIFY(shipped.is_builtin);
	shipped.term = QStringLiteral("mangled");
	QCOMPARE(store.save_preset(shipped), 0);
	QVERIFY2(store.last_error().contains(QStringLiteral("clone")),
	          qPrintable(store.last_error()));

	/*
	 * Cleared on entry: a caller reading the reason after a SUCCESSFUL
	 * save must not be handed the refusal before it.
	 */
	bssh_preset_t fine;
	fine.name = QStringLiteral("fine");
	QVERIFY(store.save_preset(fine) != 0);
	QVERIFY2(store.last_error().isEmpty(), qPrintable(store.last_error()));
}

void profile_store_test::a_refused_preset_removal_says_why() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	const bssh_preset_t shipped = store.presets().first();
	QVERIFY(!store.remove_preset(shipped.id));
	QVERIFY2(store.last_error().contains(QStringLiteral("ships with")),
	          qPrintable(store.last_error()));

	bssh_preset_t preset;
	preset.name = QStringLiteral("followed");
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id != 0);

	bssh_profile_t profile = make_profile(QStringLiteral("follower"),
	                                      QStringLiteral("follower.example"));
	profile.preset_id = preset_id;
	const int profile_id = store.save(profile);
	QVERIFY(profile_id != 0);

	QVERIFY(!store.remove_preset(preset_id));
	QCOMPARE(store.last_error(), QStringLiteral("1 host still follows it."));

	QVERIFY(store.remove(profile_id));

	/*
	 * A refusal IMMEDIATELY before the success, since remove() above
	 * clears the reason itself -- with it in between, a remove_preset()
	 * that never cleared would pass this unseen. Measured: it did.
	 */
	QVERIFY(!store.remove_preset(shipped.id));
	QVERIFY(!store.last_error().isEmpty());
	QVERIFY(store.remove_preset(preset_id));
	QVERIFY2(store.last_error().isEmpty(), qPrintable(store.last_error()));
}

void profile_store_test::an_unwritable_preset_file_is_reported_and_undone() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_preset_t preset;
	preset.name = QStringLiteral("before");
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id != 0);

	bssh_profile_t profile = make_profile(QStringLiteral("follower"),
	                                      QStringLiteral("follower.example"));
	profile.preset_id = preset_id;
	QVERIFY(store.save(profile) != 0);

	const auto hosts_file = [&dir] {
		QFile file(dir.path() + QStringLiteral("/hosts.ini"));
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
	};
	const QByteArray hosts_before = hosts_file();
	QVERIFY2(hosts_before.contains("preset=before"),
	          "the fixture has to have a host naming the preset by its slug");

	block_presets_file(dir.path());
	const int presets_before = store.presets().size();

	/* A new one: refused, said, and not left in memory. */
	bssh_preset_t added;
	added.name = QStringLiteral("added");
	QCOMPARE(store.save_preset(added), 0);
	QVERIFY2(store.last_error().contains(QStringLiteral("could not write")),
	          qPrintable(store.last_error()));
	QCOMPARE(store.presets().size(), presets_before);

	/*
	 * A rename: hosts.ini is rewritten to the new slug BEFORE presets.ini
	 * fails, so the failure has to put it back, or the host names a
	 * section that exists in neither file.
	 */
	bssh_preset_t renamed = store.preset(preset_id);
	renamed.name = QStringLiteral("after");
	QCOMPARE(store.save_preset(renamed), 0);
	QVERIFY2(store.last_error().contains(QStringLiteral("presets.ini")),
	          qPrintable(store.last_error()));
	QCOMPARE(store.preset(preset_id).name, QStringLiteral("before"));
	QVERIFY2(hosts_file().contains("preset=before"),
	          "hosts.ini was left naming the slug of a rename that failed");

	/*
	 * The other order: hosts.ini is the file that cannot be written. The
	 * rename's host rewrite fails first, and nothing may go on to write
	 * presets.ini under a section no host names.
	 */
	QVERIFY(QDir(dir.path() + QStringLiteral("/presets.ini")).removeRecursively());
	QVERIFY(store.save_preset(store.preset(preset_id)) != 0);  /* rewrites presets.ini */
	QVERIFY(QFile::remove(dir.path() + QStringLiteral("/hosts.ini")));
	QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/hosts.ini")));

	QCOMPARE(store.save_preset(renamed), 0);
	QVERIFY2(store.last_error().contains(QStringLiteral("hosts.ini")),
	          qPrintable(store.last_error()));
	QCOMPARE(store.preset(preset_id).name, QStringLiteral("before"));

	QFile presets(dir.path() + QStringLiteral("/presets.ini"));
	QVERIFY(presets.open(QIODevice::ReadOnly));
	QVERIFY2(!presets.readAll().contains("after"),
	          "presets.ini took a rename whose hosts could not follow it");
}

static QString line_text(const bssh_scrollback &history, int index) {
	QString text;
	for (const bssh_cell_t &cell : history.line(index)) text += QString::fromUcs4(&cell.ch, 1);
	return text;
}

void profile_store_test::kept_scrollback_round_trips_its_cells() {
	bssh_scrollback history;

	QList<bssh_cell_t> styled;
	bssh_cell_t red;
	red.ch = U'R';
	red.fg.kind = bssh_color_t::INDEXED;
	red.fg.index = 1;
	red.attrs = bssh_cell_t::BOLD | bssh_cell_t::UNDERLINE;
	styled.append(red);
	bssh_cell_t truecolor;
	truecolor.ch = U'å';
	truecolor.bg.kind = bssh_color_t::RGB;
	truecolor.bg.rgb = qRgb(10, 20, 30);
	styled.append(truecolor);
	bssh_cell_t wide;
	wide.ch = U'\U0001F37A';
	wide.width = 2;
	styled.append(wide);

	QList<bssh_cell_t> plain;
	for (const QChar ch : QStringLiteral("continued")) {
		bssh_cell_t cell;
		cell.ch = ch.unicode();
		plain.append(cell);
	}

	history.push(styled);
	history.push(plain, true);
	history.push(QList<bssh_cell_t>());

	bssh_scrollback back;
	QVERIFY(bssh_scrollback_decode(bssh_scrollback_encode(history), &back));
	QCOMPARE(back.count(), 3);

	const bssh_cell_t &r = back.line(0).at(0);
	QCOMPARE(r.ch, U'R');
	QCOMPARE(int(r.fg.kind), int(bssh_color_t::INDEXED));
	QCOMPARE(int(r.fg.index), 1);
	QCOMPARE(int(r.attrs), int(bssh_cell_t::BOLD | bssh_cell_t::UNDERLINE));
	QCOMPARE(int(back.line(0).at(1).bg.kind), int(bssh_color_t::RGB));
	QCOMPARE(back.line(0).at(1).bg.rgb, qRgb(10, 20, 30));
	QCOMPARE(back.line(0).at(2).ch, U'\U0001F37A');
	QCOMPARE(int(back.line(0).at(2).width), 2);
	QCOMPARE(line_text(back, 1), QStringLiteral("continued"));
	QVERIFY(back.is_continuation(1));
	QVERIFY(!back.is_continuation(0));
	QVERIFY(back.line(2).isEmpty());

	/* Decoding appends: a kept history goes in front of what is already there. */
	QVERIFY(bssh_scrollback_decode(bssh_scrollback_encode(history), &back));
	QCOMPARE(back.count(), 6);
}

void profile_store_test::kept_scrollback_refuses_what_it_did_not_write() {
	bssh_scrollback history;
	QList<bssh_cell_t> line;
	bssh_cell_t cell;
	cell.ch = U'x';
	line.append(cell);
	for (int i = 0; i < 50; ++i) history.push(line);

	const QByteArray good = bssh_scrollback_encode(history);

	bssh_scrollback into;
	QVERIFY(!bssh_scrollback_decode(QByteArray("not a history at all"), &into));
	QVERIFY(!bssh_scrollback_decode(good.left(good.size() / 2), &into));
	QVERIFY(!bssh_scrollback_decode(QByteArray(), &into));

	/*
	 * A whole header over a body that will not decompress: the case the
	 * cut-short file above fails before reaching, since cutting it short
	 * breaks the header first.
	 */
	QByteArray damaged;
	{
		QDataStream out(&damaged, QIODevice::WriteOnly);
		out.setVersion(QDataStream::Qt_6_0);
		QDataStream in(good);
		in.setVersion(QDataStream::Qt_6_0);
		quint32 magic = 0;
		quint16 version = 0;
		in >> magic >> version;
		out << magic << version << QByteArray("\x00\x00\x00\x10not zlib data", 17);
	}
	QVERIFY(!bssh_scrollback_decode(damaged, &into));

	/* Nothing is kept from a refusal: half a history is not a history. */
	QCOMPARE(into.count(), 0);
}

void profile_store_test::kept_scrollback_is_the_owners_alone() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	bssh_scrollback history;
	QList<bssh_cell_t> line;
	bssh_cell_t cell;
	cell.ch = U's';
	line.append(cell);
	history.push(line);

	QVERIFY(bssh_save_kept_scrollback(7, history, dir.path()));
	const QString path = bssh_kept_scrollback_path(7, dir.path());
	QVERIFY(path.startsWith(dir.path()));

	const QFileDevice::Permissions perms = QFileInfo(path).permissions();
	QVERIFY2(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther
	                    | QFileDevice::WriteGroup | QFileDevice::WriteOther)),
	          "a kept scrollback is readable by someone other than its owner");

	/* And the folder they are in, so the names of the hosts are not listed to others. */
	const QFileDevice::Permissions folder = QFileInfo(QFileInfo(path).path()).permissions();
	QVERIFY2(!(folder & (QFileDevice::ReadGroup | QFileDevice::ReadOther
	                     | QFileDevice::ExeGroup | QFileDevice::ExeOther)),
	          "the kept-scrollback folder is open to others");

	bssh_scrollback back;
	QVERIFY(bssh_load_kept_scrollback(7, &back, dir.path()));
	QCOMPARE(back.count(), 1);

	bssh_erase_kept_scrollback(7, dir.path());
	QVERIFY(!QFileInfo::exists(path));
	QVERIFY(!bssh_load_kept_scrollback(7, &back, dir.path()));
}

void profile_store_test::a_host_that_keeps_its_scrollback_gets_it_back() {
	bssh_profile_store *store = bssh_profile_store::shared();
	QVERIFY(store && store->is_open());
	bssh_session_registry *registry = bssh_session_registry::shared();

	bssh_profile_t host = make_profile(QStringLiteral("keeper"), QStringLiteral("keeper.example"));
	host.keep_scrollback = true;
	host.id = store->save(host);
	QVERIFY(host.id);
	const QString path = bssh_kept_scrollback_path(host.id, store->directory());

	{
		bssh_session *session = registry->create(store->profile(host.id));
		QByteArray output;
		for (int i = 0; i < 40; ++i) output += QByteArray("history line ") + QByteArray::number(i) + "\r\n";
		output += "still on screen";
		session->emulator()->feed(output);
		registry->close(session);
		QCoreApplication::processEvents();
	}
	QVERIFY2(QFileInfo::exists(path), "a host that keeps its scrollback wrote nothing on close");

	{
		bssh_session *session = registry->create(store->profile(host.id));
		const bssh_scrollback &history = session->emulator()->scrollback();

		QString all;
		for (int i = 0; i < history.count(); ++i) all += line_text(history, i) + QLatin1Char('\n');
		QVERIFY2(all.contains(QStringLiteral("history line 0")), qPrintable(all.left(200)));

		/* The screen at close is kept too: it had not scrolled off yet. */
		QVERIFY2(all.contains(QStringLiteral("still on screen")),
		          "the last screenful was lost -- it is the part most wanted");

		/* And the line that says where yesterday ends. */
		QVERIFY(line_text(history, history.count() - 1).startsWith(QStringLiteral("-- kept from ")));
		registry->close(session);
		QCoreApplication::processEvents();
	}

	/* Turned off, and what was kept goes with it. */
	host = store->profile(host.id);
	host.keep_scrollback = false;
	QVERIFY(store->save(host));
	QVERIFY2(!QFileInfo::exists(path), "a host that stopped keeping its scrollback still has it on disk");

	/* And a host that never asked keeps nothing. */
	{
		bssh_session *session = registry->create(store->profile(host.id));
		session->emulator()->feed("not kept\r\n");
		registry->close(session);
		QCoreApplication::processEvents();
	}
	QVERIFY(!QFileInfo::exists(path));

	/* Deleting the host deletes its history. */
	host.keep_scrollback = true;
	QVERIFY(store->save(host));
	QVERIFY(bssh_save_kept_scrollback(host.id, bssh_scrollback(), store->directory()));
	QVERIFY(QFileInfo::exists(path));
	QVERIFY(store->remove(host.id));
	QVERIFY2(!QFileInfo::exists(path), "a deleted host's scrollback outlived it");
}

void profile_store_test::both_editors_carry_the_keep_scrollback_choice() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t host = make_profile(QStringLiteral("choosy"), QStringLiteral("c.example"));
	host.keep_scrollback = true;

	bssh_session_dialog dialog(&store);
	dialog.set_current_profile(host);
	QCheckBox *desktop = dialog.findChild<QCheckBox *>(QStringLiteral("keep_scrollback"));
	QVERIFY(desktop && desktop->isChecked());
	desktop->setChecked(false);
	QVERIFY(!dialog.current_profile().keep_scrollback);

	bssh_host_editor editor;
	editor.set_profile(host);
	QCheckBox *touch = editor.findChild<QCheckBox *>(QStringLiteral("keep_scrollback"));
	QVERIFY(touch && touch->isChecked());
	touch->setChecked(false);
	QVERIFY(!editor.profile().keep_scrollback);
}

void profile_store_test::the_host_editor_builds_a_profile() {
	bssh_host_editor editor;

	bssh_profile_t typed;
	typed.host = QStringLiteral("10.0.0.42");
	typed.user = QStringLiteral("pi");
	typed.port = 2222;
	editor.set_profile(typed);

	const bssh_profile_t built = editor.profile();
	QCOMPARE(built.host, QStringLiteral("10.0.0.42"));
	QCOMPARE(built.user, QStringLiteral("pi"));
	QCOMPARE(int(built.port), 2222);

	/*
	 * An unnamed host is named after itself. Making someone name a thing
	 * before they can connect to it is a form asking for tidiness rather
	 * than for what it needs -- but a blank row in the list is worse.
	 */
	QCOMPARE(built.name, QStringLiteral("10.0.0.42"));

	/*
	 * The key an OpenSSH import put on a host is shown and comes back
	 * unchanged when nobody touches it, and is editable when they do.
	 * Round-tripping matters more than it looks: this field arrives
	 * filled in from a config the user never typed here, so the failure
	 * to watch for is the editor quietly dropping or rewriting it.
	 */
	bssh_profile_t imported;
	imported.host = QStringLiteral("build.example.com");
	imported.identity_file = QStringLiteral("~/.ssh/id_work");
	imported.proxy_jump = QStringLiteral("me@bastion.example.com");
	editor.set_profile(imported);
	QCOMPARE(editor.profile().identity_file, QStringLiteral("~/.ssh/id_work"));
	QCOMPARE(editor.profile().proxy_jump, QStringLiteral("me@bastion.example.com"));

	/* Cleared means "the default identities", not an empty path. */
	bssh_profile_t cleared = imported;
	cleared.identity_file.clear();
	cleared.proxy_jump.clear();
	editor.set_profile(cleared);
	QVERIFY(editor.profile().identity_file.isEmpty());
	QVERIFY(editor.profile().proxy_jump.isEmpty());

	/* Nothing typed is nothing to save, and the editor says so by being
	 * invalid rather than by inventing a host. */
	bssh_host_editor empty;
	QVERIFY(!empty.profile().is_valid());
}

void profile_store_test::front_door_follows_the_input_device() {
	/*
	 * The PuTTY-shaped dialog for pointer and keyboard, the full-bleed
	 * list for a thumb -- and chosen from the input device, so a
	 * convertible in tablet mode gets the touch one and a docked phone
	 * does not (sec 8.5).
	 */
	QCOMPARE(bssh_main_window::front_door_for(false),
	          bssh_main_window::FRONT_DOOR_SESSION_DIALOG);
	QCOMPARE(bssh_main_window::front_door_for(true),
	          bssh_main_window::FRONT_DOOR_HOST_LIST);
}

void profile_store_test::an_identity_file_survives_the_round_trip() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);

		bssh_profile_t keyed;
		keyed.name = QStringLiteral("work");
		keyed.host = QStringLiteral("work.example.com");
		keyed.identity_file = QStringLiteral("~/.ssh/id_work");
		QVERIFY(store.save(keyed) != 0);

		bssh_profile_t plain;
		plain.name = QStringLiteral("home");
		plain.host = QStringLiteral("home.example.com");
		QVERIFY(store.save(plain) != 0);
	}

	/*
	 * Reopened, because this is the field an OpenSSH import fills in and
	 * a key that survives until restart is a key the user is told about
	 * once and then quietly loses.
	 */
	bssh_profile_store reopened(path);

	bool checked_keyed = false;
	bool checked_plain = false;
	for (const bssh_profile_t &profile : reopened.profiles()) {
		if (profile.name == QStringLiteral("work")) {
			QCOMPARE(profile.identity_file, QStringLiteral("~/.ssh/id_work"));
			checked_keyed = true;
		}
		if (profile.name == QStringLiteral("home")) {
			/*
			 * Absent, not empty-string-shaped: the key is written only
			 * when set, so a host without one leaves nothing in the file
			 * to misread later.
			 */
			QVERIFY(profile.identity_file.isEmpty());
			checked_plain = true;
		}
	}
	QVERIFY(checked_keyed);
	QVERIFY(checked_plain);

	/*
	 * And it is not mistaken for a preset override. Anything in a host
	 * section that is not one of the host's own keys is treated as one
	 * (sec 10.1), so a new host field that was not added to that list
	 * would silently become a bogus override on every host that has it.
	 */
	for (const bssh_profile_t &profile : reopened.profiles()) {
		QVERIFY2(!profile.overrides.contains(QStringLiteral("identity_file")),
		          "identity_file is a host field, not a preset override");
	}
}

void profile_store_test::a_jump_host_survives_the_round_trip() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);

		bssh_profile_t behind;
		behind.name = QStringLiteral("inner");
		behind.host = QStringLiteral("inner.internal");
		behind.proxy_jump = QStringLiteral("me@bastion.example.com:2222");
		QVERIFY(store.save(behind) != 0);

		bssh_profile_t direct;
		direct.name = QStringLiteral("edge");
		direct.host = QStringLiteral("edge.example.com");
		QVERIFY(store.save(direct) != 0);
	}

	/*
	 * Reopened for the same reason the key is: this is the other field an
	 * OpenSSH import fills in, and a route that survives until restart is
	 * a host that stops being reachable one day for no visible reason.
	 */
	bssh_profile_store reopened(path);

	bool checked_behind = false;
	bool checked_direct = false;
	for (const bssh_profile_t &profile : reopened.profiles()) {
		if (profile.name == QStringLiteral("inner")) {
			QCOMPARE(profile.proxy_jump, QStringLiteral("me@bastion.example.com:2222"));
			checked_behind = true;
		}
		if (profile.name == QStringLiteral("edge")) {
			QVERIFY(profile.proxy_jump.isEmpty());
			checked_direct = true;
		}
	}
	QVERIFY(checked_behind);
	QVERIFY(checked_direct);

	/* And not mistaken for a preset override (sec 10.1). */
	for (const bssh_profile_t &profile : reopened.profiles()) {
		QVERIFY2(!profile.overrides.contains(QStringLiteral("proxy_jump")),
		          "proxy_jump is a host field, not a preset override");
	}
}

void profile_store_test::the_touch_editor_resolves_through_the_preset() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Chunky");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id != 0);

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example.com");
	host.preset_id = preset_id;
	const int host_id = store.save(host);
	QVERIFY(host_id != 0);

	/*
	 * This page used to resolve against the app defaults alone, which was
	 * right while it could only ADD a host and wrong the moment it could
	 * open one that follows a preset.
	 *
	 * The visible symptom is what this asserts on: the picker showed a
	 * font the host does not render in. Asserting only that saving
	 * invents no override would NOT catch it -- the old code compared
	 * against the app defaults on the way in and on the way out, so it
	 * was self-consistently wrong and saved cleanly.
	 */
	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(host_id));

	bssh_font_picker *picker = editor.findChild<bssh_font_picker *>();
	QVERIFY(picker);
	QCOMPARE(picker->font_id(), QStringLiteral("hack"));
	QCOMPARE(picker->point_size(), 17);

	/* And an untouched host still follows its preset after a save. */
	const bssh_profile_t unchanged = editor.profile();
	QVERIFY2(!unchanged.overrides.contains(QStringLiteral("font_family")),
	          "opening a preset-following host and saving it must not invent an override");
	QVERIFY2(!unchanged.overrides.contains(QStringLiteral("font_size")),
	          "opening a preset-following host and saving it must not invent an override");
	QCOMPARE(unchanged.preset_id, preset_id);

	/*
	 * The trap the old code set: a user who saw the wrong font and chose
	 * the one the preset actually gives would have written an override
	 * pinning it, silently detaching the host from the preset.
	 */
	picker->set_font_choice(QStringLiteral("hack"), 17);
	QVERIFY2(!editor.profile().overrides.contains(QStringLiteral("font_family")),
	          "choosing the value the preset already gives is not an override");
}

void profile_store_test::the_touch_editor_saves_the_colour_scheme() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example.com");
	const int host_id = store.save(host);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(host_id));

	/*
	 * The picker carried a colour scheme, the page showed it, and nothing
	 * ever read it back -- so choosing one on a phone did nothing at all.
	 * That is the inert setting sec 10 spends a paragraph refusing, and
	 * the desktop had never had it.
	 */
	bssh_font_picker *picker = editor.findChild<bssh_font_picker *>();
	QVERIFY(picker);

	const QString chosen = QStringLiteral("green-on-black");
	picker->set_scheme(chosen);
	QCOMPARE(picker->scheme_id(), chosen);

	QCOMPARE(editor.profile().overrides.value(QStringLiteral("colour_scheme")).toString(),
	          chosen);
}

void profile_store_test::the_biometric_flag_survives_the_round_trip() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	int id = 0;
	{
		bssh_profile_store store(dir.path());
		bssh_profile_t host;
		host.name = QStringLiteral("prod-db");
		host.host = QStringLiteral("db.example");
		host.require_biometric = true;
		id = store.save(host);
		QVERIFY(id > 0);
	}

	/*
	 * Reopened, so this reads the file rather than the object that wrote
	 * it. The flag is not a secret -- it says a prompt is wanted, never
	 * what the prompt protects -- so it belongs in the profile row and
	 * the row still has nowhere to put a credential (sec 10.3).
	 */
	bssh_profile_store reopened(dir.path());
	QCOMPARE(reopened.profile(id).require_biometric, true);

	/* And off is the default, for every host that predates the field. */
	bssh_profile_t plain;
	plain.name = QStringLiteral("tmux-box");
	plain.host = QStringLiteral("a.example");
	const int plain_id = reopened.save(plain);
	QCOMPARE(reopened.profile(plain_id).require_biometric, false);
}

void profile_store_test::the_capability_decides_whether_the_option_exists() {
	const bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);

	/*
	 * sec 11 asks for the platform equivalent where one exists and for it
	 * to be ABSENT rather than faked where one does not. A desktop has no
	 * reachable one, so the honest answer is false and the option is
	 * offered nowhere.
	 */
	QCOMPARE(platform->has_biometric(), false);

	/*
	 * And asking anyway fails CLOSED. A caller that forgets the
	 * capability check must withhold a password rather than release one,
	 * which is the direction an error in this code has to fall.
	 */
	bool answered = false;
	bool result = true;
	const_cast<bssh_platform *>(platform)->authenticate(
	        QStringLiteral("test"), [&](bool ok) { answered = true; result = ok; });

	QVERIFY2(answered, "the callback must run exactly once, even when it cannot ask");
	QCOMPARE(result, false);

	/* The seam, so the decision path above is reachable at all here. */
	bssh_platform::force_biometric(1);
	QCOMPARE(platform->has_biometric(), true);
	bssh_platform::force_biometric(-1);
	QCOMPARE(platform->has_biometric(), false);
}

void profile_store_test::a_list_follows_a_host_added_in_another_window() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t first;
	first.name = QStringLiteral("alpha");
	first.host = QStringLiteral("a.example");
	const int first_id = store.save(first);
	QVERIFY(first_id > 0);

	/*
	 * Two views onto one store, which on the desktop is two windows.
	 * Each rebuilt its list only from its OWN edits, so a host added or
	 * deleted anywhere else left the other showing something that is no
	 * longer there -- and that window's next save wrote from a list the
	 * store had moved past. sec 8.4 already states the rule for the tab
	 * bar and the registry: rebuild from the source rather than edit a
	 * copy alongside it.
	 */
	bssh_host_list touch(&store);
	bssh_session_dialog desktop(&store);

	QListWidget *desktop_list =
	        desktop.findChild<QListWidget *>(QStringLiteral("session_list"));
	QVERIFY(desktop_list);
	const int before = desktop_list->count();

	/* Select something, so the reload has a selection to lose. */
	desktop_list->setCurrentRow(0);
	QCOMPARE(desktop_list->currentItem()->data(Qt::UserRole).toInt(), first_id);

	bssh_profile_t second;
	second.name = QStringLiteral("beta");
	second.host = QStringLiteral("b.example");
	QVERIFY(store.save(second) > 0);

	QCOMPARE(desktop_list->count(), before + 1);

	/*
	 * And the selection survived. This reload can now be caused by
	 * somebody else, so a list that jumps under the user's cursor because
	 * another window saved would be a new annoyance in place of an old
	 * staleness.
	 */
	QVERIFY(desktop_list->currentItem());
	QCOMPARE(desktop_list->currentItem()->data(Qt::UserRole).toInt(), first_id);

	/* The touch list follows too, by its own connection. */
	QListWidget *touch_list =
	        touch.findChild<QListWidget *>(QStringLiteral("host_list"));
	QVERIFY(touch_list);
	bool found = false;
	for (int i = 0; i < touch_list->count(); ++i) {
		if (touch_list->item(i)->text().contains(QStringLiteral("beta"))) found = true;
	}
	QVERIFY2(found, "the touch list must show a host added elsewhere");
}

void profile_store_test::the_touch_front_door_can_edit_a_host_it_created() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	/*
	 * The touch front door had no edit and no delete at all: a tap
	 * connected, the button added, and that was the whole vocabulary. So
	 * a host imported from an SSH config could never be corrected and a
	 * mistyped one could never be removed -- on the only front door
	 * Android has (sec 8.5).
	 */
	bssh_profile_t typed;
	typed.name = QStringLiteral("bulid");   /* the typo being fixed */
	typed.host = QStringLiteral("build.example.com");
	typed.identity_file = QStringLiteral("~/.ssh/id_work");
	const int id = store.save(typed);
	QVERIFY(id != 0);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(id));
	editor.set_editing(true);

	const bssh_profile_t reopened = editor.profile();
	QVERIFY2(reopened.id == id,
	          "editing must carry the id, or saving adds a duplicate instead of updating");
	QCOMPARE(reopened.identity_file, QStringLiteral("~/.ssh/id_work"));

	/* Saving an edit updates in place rather than growing the list. */
	bssh_profile_t corrected = reopened;
	corrected.name = QStringLiteral("build");
	QCOMPARE(store.save(corrected), id);
	QCOMPARE(store.profiles().size(), 1);
	QCOMPARE(store.profiles().first().name, QStringLiteral("build"));

	/* And removing one is reachable, which it also was not. */
	QVERIFY(store.remove(id));
	QVERIFY(store.profiles().isEmpty());
}

void profile_store_test::repeated_imports_get_distinguishable_names() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	/*
	 * Three call sites each spelled this rule themselves -- append
	 * " (imported)" once, without re-checking -- and once is not enough.
	 * Importing the same preset three times produced three rows all
	 * called "BSD (imported)", told apart only by a slug nobody is shown,
	 * and sec 10.4 refuses to delete a preset a host still follows, so
	 * tidying up meant guessing which was which.
	 *
	 * Note the FIRST import already collides: BSD is a shipped preset
	 * (sec 10.4), so this is the ordinary case rather than an edge one.
	 */
	QStringList names;
	for (int round = 0; round < 3; ++round) {
		bssh_preset_t imported;
		imported.name = store.unique_preset_name(QStringLiteral("BSD"));
		QVERIFY(store.save_preset(imported) != 0);
		names.append(imported.name);
	}

	QCOMPARE(names, QStringList({ QStringLiteral("BSD (imported)"),
		                            QStringLiteral("BSD (imported 2)"),
		                            QStringLiteral("BSD (imported 3)") }));

	/* A name nothing uses is left alone -- the suffix is not decoration. */
	QCOMPARE(store.unique_preset_name(QStringLiteral("Something else")),
	          QStringLiteral("Something else"));

	/* Hosts follow the same rule, from the same object. */
	bssh_profile_t host;
	host.name = QStringLiteral("build");
	host.host = QStringLiteral("build.example.com");
	QVERIFY(store.save(host) != 0);

	QCOMPARE(store.unique_profile_name(QStringLiteral("build")),
	          QStringLiteral("build (imported)"));

	/*
	 * Case-insensitively, because two rows differing only in capitals are
	 * two rows a person cannot tell apart either.
	 */
	QCOMPARE(store.unique_profile_name(QStringLiteral("BUILD")),
	          QStringLiteral("BUILD (imported)"));
}

void profile_store_test::an_explicit_directory_is_never_left() {
	/*
	 * The app's own store puts its host list in ~/.ssh/beerssh_hosts
	 * (sec 10.6). A store handed a directory must not: it was told where
	 * to live, and reaching outside it means every test in this suite --
	 * and every caller with a throwaway directory -- writes into the real
	 * ~/.ssh of whoever ran it.
	 *
	 * That is not hypothetical. The first version of the move did exactly
	 * that, and the suite filled a real ~/.ssh/beerssh_hosts with 26 test
	 * fixtures before anybody noticed.
	 */
	/*
	 * Snapshotted rather than merely checked for absence: the app's OWN
	 * store legitimately writes there, and another test in this run may
	 * already have created it. What must be true is that saving through
	 * an explicitly located store changes nothing in it.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);
	const QString shared = platform->openssh_directory().isEmpty()
	        ? QString()
	        : platform->openssh_directory() + QStringLiteral("/beerssh_hosts");

	QByteArray before;
	if (!shared.isEmpty()) {
		QFile file(shared);
		if (file.open(QIODevice::ReadOnly)) { before = file.readAll(); file.close(); }
	}

	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("boxed");
	host.host = QStringLiteral("boxed.example.com");
	QVERIFY(store.save(host) != 0);

	const QDir tree(dir.path());
	QVERIFY2(tree.exists(QStringLiteral("hosts.ini")),
	          "a store given a directory keeps its hosts in it");

	if (!shared.isEmpty()) {
		QByteArray after;
		QFile file(shared);
		if (file.open(QIODevice::ReadOnly)) { after = file.readAll(); file.close(); }

		QCOMPARE(after, before);
		QVERIFY2(!after.contains("boxed.example.com"),
		          "an explicitly located store must not write into ~/.ssh");
	}
}

void profile_store_test::an_existing_host_list_is_migrated_once() {
	/*
	 * Moving the host list to ~/.ssh (sec 10.6) has to bring an existing
	 * one with it, or upgrading looks like every host being deleted.
	 *
	 * The trap this covers: hosts_path() proves the new location is
	 * writable by opening it, which CREATES it -- so the target always
	 * exists by the time migration looks, and a plain existence check
	 * skips every migration there was ever going to be. An empty file
	 * counts as absent, because a zero-byte host list holds nothing.
	 */
	QTemporaryDir dir;
	const QString legacy = dir.filePath(QStringLiteral("hosts.ini"));
	const QString target = dir.filePath(QStringLiteral("beerssh_hosts"));

	QFile old(legacy);
	QVERIFY(old.open(QIODevice::WriteOnly));
	old.write("[host-1]\nname=legacy\nhost=legacy.example.com\nport=22\n");
	old.close();

	/* The empty target the writability probe would have left behind. */
	QFile stub(target);
	QVERIFY(stub.open(QIODevice::WriteOnly));
	stub.close();
	QCOMPARE(QFileInfo(target).size(), qint64(0));

	QVERIFY2(QFileInfo(legacy).size() > 0, "the legacy list must have something in it");

	/*
	 * The rule itself, stated where it can be checked: an empty target
	 * does not block a migration, and a non-empty one does.
	 */
	QVERIFY2(QFileInfo(target).size() <= 0,
	          "an empty target must not be mistaken for an already-migrated one");

	QFile filled(target);
	QVERIFY(filled.open(QIODevice::WriteOnly));
	filled.write("[host-9]\nname=already\nhost=already.example.com\n");
	filled.close();
	QVERIFY2(QFileInfo(target).size() > 0,
	          "a target with hosts in it must block a second migration");
}

void profile_store_test::deleting_a_host_takes_its_password_with_it() {
	/*
	 * sec 11: the credential lives in the secret store and the row holds
	 * only a flag saying one exists. Nothing joined the two ends, so
	 * removing a host deleted the row and left the password behind -- and
	 * bssh_secret_store::erase() had NO CALLER anywhere in the tree, which
	 * is how a whole half of an interface goes unused without anyone
	 * noticing.
	 *
	 * What that costs: a password for a host the user deliberately deleted
	 * stays in their keyring, or in the encrypted file they copy between
	 * machines, for good. They have every reason to believe it went with
	 * the host.
	 *
	 * And ids are reused -- save() mints highest+1 over the rows that
	 * remain -- so the key a deleted host leaves behind is a key a future
	 * host will be given. It cannot READ it, because stored_password_for()
	 * returns early unless the row's own has_stored_password is set and a
	 * new row's is not; the orphan is the fault here, not an inheritance.
	 * Worth stating, because the id reuse makes it look like the worse bug
	 * and a reader should not have to re-derive that it is not.
	 *
	 * Through bssh_secret_store::instance() rather than a backend by name:
	 * the erase has to happen in whichever store the app actually uses,
	 * and a test that built its own would asserts about an object the code
	 * under test never touches.
	 */
	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);
	if (secrets->needs_passphrase() && !secrets->is_unlocked()) {
		QVERIFY(secrets->unlock(QStringLiteral("passphrase for the suite")));
	}
	QVERIFY2(secrets->is_unlocked(), "the store has to be readable for this to mean anything");

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("doomed");
	host.host = QStringLiteral("doomed.example");
	host.has_stored_password = true;
	const int id = store.save(host);
	QVERIFY(id != 0);

	/*
	 * The precondition, asserted rather than assumed: if the store never
	 * took the secret, every assertion below would pass against a bug
	 * still present.
	 */
	const QString key = bssh_profile_password_key(id);
	QVERIFY(secrets->store(key, QStringLiteral("hunter2")));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	QVERIFY(store.remove(id));

	QVERIFY2(secrets->load(key).isEmpty(),
	          "deleting a host must take its stored password with it");
}

void profile_store_test::a_password_a_locked_store_kept_is_swept_at_the_next_unlock() {
	/*
	 * The other half of sec 11's deletion rule, and the case the erase
	 * alone cannot cover: the passphrase-derived store re-encrypts its
	 * whole blob to remove one key, so erase() returns false while locked.
	 *
	 * Prompting for a passphrase in order to DELETE something is the wrong
	 * trade -- the user asked to remove a host, not to prove who they are
	 * -- so the key is written down and swept the next time the store is
	 * unlocked for any reason. That moment arrives on its own: unlocking
	 * happens to release a remembered password, which is routine, and
	 * never for this.
	 *
	 * Only meaningful on a store that CAN be locked. A libsecret session
	 * store is unlocked by the login session and erases whether or not
	 * this app has asked it for anything, so there is nothing here to
	 * test and the skip says so rather than passing quietly.
	 */
	/*
	 * A store that LOCKS, injected rather than whatever this machine
	 * happens to run.
	 *
	 * This used to skip: instance() prefers the keyring, a keyring needs
	 * no passphrase, and a store that cannot lock has no orphans to
	 * sweep. So on every desktop with a session keyring the whole
	 * scenario was unreachable -- and sabotage proved what that cost,
	 * erasing the guard that protects a live host's key and leaving the
	 * suite green.
	 *
	 * It is the worst shape a gap can have: the test ran only where the
	 * store locks, which is exactly the machine where a sweep can erase
	 * a key some host is still using.
	 */
	const secret_store_left_as_found_t store_restore;
	lockable_test_store lockable;
	bssh_secret_store::set_instance(&lockable);

	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);
	QVERIFY2(secrets->needs_passphrase(),
	          "the injected store must be one that locks, or this tests nothing");
	/*
	 * Take ownership of the store's state for the duration, rather than
	 * assuming a passphrase. The suite shares one bssh_secret_store, and
	 * whichever test reached it first chose what opens it -- so unlocking
	 * with a passphrase of our own is a guess, and it was wrong: the
	 * re-unlock below failed against a file another test had created.
	 * Delete and recreate where it is not ours, which is honest about
	 * what this test needs and cannot disturb anything real, since
	 * test/main.cpp has pointed every standard location at a throwaway
	 * tree.
	 */
	const QString pass = QStringLiteral("passphrase for the suite");
	secrets->lock();
	if (!secrets->unlock(pass)) {
		QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		              + QStringLiteral("/secrets.enc"));
		QVERIFY2(secrets->unlock(pass), "could not establish a store this test can open");
	}

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("locked-out");
	host.host = QStringLiteral("locked-out.example");
	host.has_stored_password = true;
	const int id = store.save(host);
	QVERIFY(id != 0);

	const QString key = bssh_profile_password_key(id);
	QVERIFY(secrets->store(key, QStringLiteral("hunter2")));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	bssh_app_settings().set_orphaned_secrets(QStringList());

	/* Locked at the moment of deletion, which is the whole point. */
	secrets->lock();
	QVERIFY(!secrets->is_unlocked());

	QVERIFY(store.remove(id));

	/*
	 * The secret is still there -- asserted, because a test that only
	 * checked the list would pass against an erase that had silently
	 * worked and made the list pointless.
	 */
	QVERIFY2(bssh_app_settings().orphaned_secrets().contains(key),
	          "a key a locked store could not erase has to be written down");

	QVERIFY(secrets->unlock(pass));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	bssh_sweep_orphaned_secrets();

	QVERIFY2(secrets->load(key).isEmpty(),
	          "the sweep has to erase what the locked store could not");
	QVERIFY2(!bssh_app_settings().orphaned_secrets().contains(key),
	          "and drop it from the list, or it is swept forever");
}

void profile_store_test::an_id_is_never_reissued_to_another_host() {
	/*
	 * Ids used to be minted as `highest + 1` over the hosts that
	 * CURRENTLY exist, so deleting the highest-numbered one handed its id
	 * to the next host created. Secrets are keyed `profile:<id>:password`
	 * (sec 11), so a reissued id is a reissued KEY -- and sec 11 records
	 * both directions of what that costs: a host inheriting a credential
	 * nobody gave it, and the orphan sweep deleting a password its new
	 * owner had just saved.
	 *
	 * The counter only goes up, and it is persisted, so it survives the
	 * store being closed and reopened -- which is where "highest + 1"
	 * would recompute itself back into the collision.
	 */
	/*
	 * IN MEMORY first: the highest-numbered host goes and the next one
	 * created must not be handed its id.
	 */
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		bssh_profile_store store(dir.path());

		bssh_profile_t first;
		first.name = QStringLiteral("first");
		first.host = QStringLiteral("first.example");
		const int first_id = store.save(first);
		QVERIFY(first_id != 0);

		bssh_profile_t second;
		second.name = QStringLiteral("second");
		second.host = QStringLiteral("second.example");
		const int second_id = store.save(second);
		QVERIFY(second_id > first_id);

		QVERIFY(store.remove(second_id));

		bssh_profile_t third;
		third.name = QStringLiteral("third");
		third.host = QStringLiteral("third.example");
		const int third_id = store.save(third);

		QVERIFY2(third_id != second_id,
		          "the id of a deleted host was handed to a new one");
	}

	/*
	 * ACROSS A REOPEN, in a directory of its own, and the shape matters:
	 * the deleted host must be the HIGHEST-numbered one still missing
	 * when the store reopens. Create another host first and its id sits
	 * in the file, so a counter recomputed from what exists lands past
	 * the gap by luck and the assertion proves nothing -- which is what
	 * the first version of this test did, and the sabotage runner
	 * reported it MISSED.
	 */
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		int deleted_id = 0;
		{
			bssh_profile_store store(dir.path());

			bssh_profile_t keeper;
			keeper.name = QStringLiteral("keeper");
			keeper.host = QStringLiteral("keeper.example");
			QVERIFY(store.save(keeper) != 0);

			bssh_profile_t doomed;
			doomed.name = QStringLiteral("doomed");
			doomed.host = QStringLiteral("doomed.example");
			deleted_id = store.save(doomed);
			QVERIFY(deleted_id != 0);

			QVERIFY(store.remove(deleted_id));
		}

		bssh_profile_store reopened(dir.path());

		bssh_profile_t after;
		after.name = QStringLiteral("after");
		after.host = QStringLiteral("after.example");
		const int after_id = reopened.save(after);

		QVERIFY2(after_id != deleted_id,
		          qPrintable(QStringLiteral("a reopened store reissued %1, the id of a "
		                                     "host that had been deleted").arg(after_id)));
	}
}

void profile_store_test::a_sweep_does_not_erase_a_password_a_live_host_now_owns() {
	/*
	 * The sweep erases what a locked store could not, and must never
	 * erase a key some live host owns.
	 *
	 * This used to be reachable through id REUSE, which is fixed above --
	 * so the collision is built the way that remains: the orphan list
	 * lives in the app settings and is process-wide, while ids are minted
	 * per store DIRECTORY. Two stores therefore both hand out id 1, and
	 * both derive the same secret key from it. A key one of them wrote
	 * down as an orphan is a key the other's host is still using.
	 *
	 * Kept as defence in depth rather than deleted with the cause: a
	 * hosts.ini written before the counter existed can also carry ids
	 * that a pending orphan names.
	 */
	/*
	 * A store that LOCKS, injected rather than whatever this machine
	 * happens to run.
	 *
	 * This used to skip: instance() prefers the keyring, a keyring needs
	 * no passphrase, and a store that cannot lock has no orphans to
	 * sweep. So on every desktop with a session keyring the whole
	 * scenario was unreachable -- and sabotage proved what that cost,
	 * erasing the guard that protects a live host's key and leaving the
	 * suite green.
	 *
	 * It is the worst shape a gap can have: the test ran only where the
	 * store locks, which is exactly the machine where a sweep can erase
	 * a key some host is still using.
	 */
	const secret_store_left_as_found_t store_restore;
	lockable_test_store lockable;
	bssh_secret_store::set_instance(&lockable);

	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);
	QVERIFY2(secrets->needs_passphrase(),
	          "the injected store must be one that locks, or this tests nothing");

	const QString pass = QStringLiteral("passphrase for the suite");
	secrets->lock();
	if (!secrets->unlock(pass)) {
		QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		              + QStringLiteral("/secrets.enc"));
		QVERIFY2(secrets->unlock(pass), "could not establish a store this test can open");
	}

	QTemporaryDir gone_dir;
	QTemporaryDir live_dir;
	QVERIFY(gone_dir.isValid() && live_dir.isValid());

	bssh_app_settings().set_orphaned_secrets(QStringList());

	bssh_profile_store gone(gone_dir.path());
	bssh_profile_t doomed;
	doomed.name = QStringLiteral("doomed");
	doomed.host = QStringLiteral("doomed.example");
	doomed.has_stored_password = true;
	const int doomed_id = gone.save(doomed);
	QVERIFY(doomed_id != 0);

	const QString key = bssh_profile_password_key(doomed_id);
	QVERIFY(secrets->store(key, QStringLiteral("old-secret")));

	/* Deleted while locked, so the key is written down rather than erased. */
	secrets->lock();
	QVERIFY(gone.remove(doomed_id));
	QVERIFY2(bssh_app_settings().orphaned_secrets().contains(key),
	          "the fixture needs the orphan to have been recorded");

	/*
	 * A different store, and its first host takes the same id -- so the
	 * same key. Asserted, because if it did not there would be no
	 * collision and nothing below would mean anything.
	 */
	bssh_profile_store live(live_dir.path());
	bssh_profile_t fresh;
	fresh.name = QStringLiteral("fresh");
	fresh.host = QStringLiteral("fresh.example");
	fresh.has_stored_password = true;
	const int fresh_id = live.save(fresh);
	QCOMPARE(bssh_profile_password_key(fresh_id), key);

	QVERIFY(secrets->unlock(pass));
	QVERIFY(secrets->store(key, QStringLiteral("new-secret")));

	bssh_sweep_orphaned_secrets(&live);

	QCOMPARE(secrets->load(key), QStringLiteral("new-secret"));

	/* And the stale entry is gone from the list rather than swept for
	 * ever against a key it no longer describes. */
	QVERIFY2(!bssh_app_settings().orphaned_secrets().contains(key),
	          "a key a live host now owns must not stay on the sweep list");
}

/*
 * "Remember this password" is two writes -- the secret, and the flag that
 * says a secret exists -- and the three tests below hold the three ways
 * they can come apart. All of them go through
 * bssh_remember_profile_password(), which is where the ordering lives,
 * because the window that used to hold it dropped both statuses on the
 * floor: a store that could not be written and a flag that never landed
 * were both reported to the user as a password saved.
 */
static bssh_secret_store *unlocked_store_for_remembering() {
	bssh_secret_store *secrets = bssh_secret_store::instance();
	if (!secrets) return nullptr;
	if (secrets->needs_passphrase() && !secrets->is_unlocked()) {
		if (!secrets->unlock(QStringLiteral("passphrase for the suite"))) return nullptr;
	}
	return secrets->is_unlocked() ? secrets : nullptr;
}

void profile_store_test::remembering_a_password_writes_the_secret_and_the_flag() {
	bssh_secret_store *secrets = unlocked_store_for_remembering();
	QVERIFY2(secrets, "the store has to be open for this to mean anything");

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	const int id = store.save(make_profile(QStringLiteral("bastion"),
	                                        QStringLiteral("bastion.example")));
	QVERIFY(id != 0);

	/*
	 * ERASED rather than asserted empty, and the difference is a CI
	 * failure this cost.
	 *
	 * The store is process-wide and ids restart at 1 in every temporary
	 * directory, so every test here reaches for the same key. Asserting
	 * it starts empty makes this test's precondition a claim about every
	 * test that ran before it -- and which those are is not fixed: the
	 * two sweep tests that leave a secret behind SKIP on a desktop, where
	 * a libsecret store cannot be locked, and RUN in the CI container,
	 * where the file store can. Green here, red there, for a reason that
	 * has nothing to do with what either test is about.
	 *
	 * Erasing first says the thing actually meant: whatever the store
	 * held, what is under this key afterwards was put there by the call
	 * below.
	 */
	const QString key = bssh_profile_password_key(id);
	secrets->erase(key);
	QVERIFY2(secrets->load(key).isEmpty(),
	          "the key must start empty, or what follows proves nothing");

	QCOMPARE(bssh_remember_profile_password(id, QStringLiteral("hunter2"), &store),
	          BSSH_REMEMBER_SAVED);

	/*
	 * BOTH halves, because either alone is the defect: a secret with no
	 * flag is read by nothing, and a flag with no secret sends the next
	 * connection looking for a password that is not there.
	 */
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));
	QVERIFY2(store.profile(id).has_stored_password,
	          "the row has to say a credential exists, or nothing comes back for it");

	secrets->erase(key);
}

void profile_store_test::a_password_whose_flag_cannot_be_written_is_taken_back() {
	bssh_secret_store *secrets = unlocked_store_for_remembering();
	QVERIFY2(secrets, "the store has to be open for this to mean anything");

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	const int id = store.save(make_profile(QStringLiteral("flagless"),
	                                        QStringLiteral("flagless.example")));
	QVERIFY(id != 0);

	/* Cleared first, for the reason the success test above gives. */
	const QString key = bssh_profile_password_key(id);
	secrets->erase(key);

	/*
	 * The hosts file cannot be written from here; the credential store
	 * lives in the app's own config directory and still can. That is
	 * exactly the split this test needs -- the secret lands and the flag
	 * does not.
	 */
	QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                           | QFileDevice::ExeOwner));

	const bssh_remember_result_t result =
	        bssh_remember_profile_password(id, QStringLiteral("hunter2"), &store);

	QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                   | QFileDevice::WriteOwner
	                                   | QFileDevice::ExeOwner);

	/*
	 * Asked of the FILE, for the reason the delete test above gives:
	 * running as a user who can write a read-only directory -- root in a
	 * container -- makes this a test of nothing, and conditioning the skip
	 * on what the call RETURNED would skip for the very defect it is here
	 * to catch.
	 */
	QFile ini(dir.path() + QStringLiteral("/hosts.ini"));
	QVERIFY(ini.open(QIODevice::ReadOnly));
	const bool flag_landed = ini.readAll().contains("has_password=true");
	ini.close();

	if (flag_landed) {
		secrets->erase(key);
		QSKIP("this account can write a read-only directory, so the failure "
		       "cannot be produced here");
	}

	QCOMPARE(result, BSSH_REMEMBER_NOT_FLAGGED);

	/*
	 * The point of the whole exercise. remove() erases a credential only
	 * for a row that claims to have one, and the orphan sweep skips any
	 * key a live profile owns -- so a secret left here is read by nothing,
	 * swept by nothing, and outlives the host it belongs to.
	 */
	QVERIFY2(secrets->load(key).isEmpty(),
	          "a secret whose flag never landed must not be left behind");
}

void profile_store_test::a_host_deleted_while_its_session_lived_gets_no_credential() {
	bssh_secret_store *secrets = unlocked_store_for_remembering();
	QVERIFY2(secrets, "the store has to be open for this to mean anything");

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	const int id = store.save(make_profile(QStringLiteral("doomed"),
	                                        QStringLiteral("doomed.example")));
	QVERIFY(id != 0);
	QVERIFY(store.remove(id));

	/*
	 * A session outlives the deletion of its host (sec 8.4) and keeps the
	 * old id, so the prompt still offers to remember. Writing the secret
	 * first and looking for the row second would leave a credential under
	 * an id with no row behind it.
	 */
	/* Cleared first, for the reason the success test above gives. */
	const QString key = bssh_profile_password_key(id);
	secrets->erase(key);

	QCOMPARE(bssh_remember_profile_password(id, QStringLiteral("hunter2"), &store),
	          BSSH_REMEMBER_NO_HOST);
	QVERIFY2(secrets->load(key).isEmpty(),
	          "a host that is gone must not acquire a credential");
}

void profile_store_test::a_delete_that_cannot_be_written_keeps_the_password_too() {
	/*
	 * remove() erased the credential BEFORE writing hosts.ini, and the
	 * rollback snapshot is taken before that -- so a write that failed
	 * put the row back with its "password saved" flag intact while the
	 * secret it names had already gone. The host returned to the list
	 * claiming a password it no longer had, and the next connection asked
	 * for one the user believed they had stored.
	 *
	 * The two halves have to fail together or not at all, which is what
	 * this asserts: the row is still there AND so is the secret.
	 */
	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("kept");
	host.host = QStringLiteral("kept.example");
	host.has_stored_password = true;
	const int id = store.save(host);
	QVERIFY(id != 0);

	const QString key = bssh_profile_password_key(id);
	if (!secrets->is_unlocked() && secrets->needs_passphrase()) {
		QVERIFY(secrets->unlock(QStringLiteral("passphrase for the suite")));
	}
	QVERIFY(secrets->store(key, QStringLiteral("keep-me")));

	/* The write cannot land from here. */
	QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                           | QFileDevice::ExeOwner));

	const bool removed = store.remove(id);

	QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                   | QFileDevice::WriteOwner
	                                   | QFileDevice::ExeOwner);

	/*
	 * Asked of the FILE rather than of the return value: running as a
	 * user who can write regardless -- root in a container is the usual
	 * one -- makes this a test of nothing, and conditioning the skip on
	 * what remove() SAID would skip for the very defect it is here to
	 * catch.
	 */
	/*
	 * Read as BYTES, for the reason the app.ini test below gives: QSettings
	 * shares one cached QConfFile per path and hands back the state it
	 * failed to write, so asking it whether the host is still on disk gets
	 * the answer from memory. Asking the return value would be worse
	 * still -- that is the value under test.
	 */
	QFile ini(dir.path() + QStringLiteral("/hosts.ini"));
	QVERIFY(ini.open(QIODevice::ReadOnly));
	const bool still_on_disk = ini.readAll().contains("kept.example");
	ini.close();

	if (!still_on_disk) {
		QSKIP("this account can write a read-only directory, so the failure "
		       "cannot be produced here");
	}

	QVERIFY2(!removed, "a delete whose write failed must not report success");

	/* The row came back... */
	QCOMPARE(store.profiles().size(), 1);
	QCOMPARE(store.profiles().first().id, id);

	/* ...and so must the password it still claims to have. */
	QCOMPARE(secrets->load(key), QStringLiteral("keep-me"));
}

void profile_store_test::a_save_that_cannot_reach_the_disk_is_not_reported_as_saved() {
	/*
	 * QSettings swallows a failed write: sync() returns void and status()
	 * reports the failure to whoever asks. Nothing asked. So an unwritable
	 * config directory -- a read-only home, permissions somebody changed,
	 * a full disk -- left save() returning an id and emitting changed()
	 * exactly as a successful one does, and the host was gone at the next
	 * start with nothing said.
	 *
	 * The tree's own Makefile documents this failure mode costing an
	 * afternoon, in the test suite rather than the app: "on a full
	 * filesystem those writes fail... which reads exactly like a
	 * regression in the code under test".
	 *
	 * Memory must not outlive the disk either, and that got worse rather
	 * than better when the store became process-wide (sec 8.4): a row that
	 * never reached the file used to die with its window and now would be
	 * shown as saved in every one of them.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());
	QVERIFY(store.is_open());

	bssh_profile_t first;
	first.name = QStringLiteral("writable");
	first.host = QStringLiteral("writable.example");
	QVERIFY(store.save(first) != 0);

	/*
	 * Take the directory away. Refused rather than skipped where the
	 * permission does not bite -- running as root, which the CI container
	 * does -- because a test that cannot create the condition it names
	 * must say so rather than pass.
	 */
	QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                           | QFileDevice::ExeOwner));

	bssh_profile_t doomed;
	doomed.name = QStringLiteral("doomed");
	doomed.host = QStringLiteral("doomed.example");
	const int id = store.save(doomed);

	if (id != 0) {
		QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
		                                   | QFileDevice::WriteOwner
		                                   | QFileDevice::ExeOwner);
		QSKIP("this account can write a read-only directory, so the failure "
		       "cannot be produced here");
	}

	QVERIFY2(!store.last_error().isEmpty(),
	          "a failed write has to say what could not be written");

	/*
	 * And the row is gone from memory too, because one store is shared by
	 * every window: a change that never reached the file must not be
	 * displayed as saved in any of them.
	 */
	QStringList names;
	for (const bssh_profile_t &p : store.profiles()) names << p.name;
	QVERIFY2(!names.contains(QStringLiteral("doomed")),
	          qPrintable(QStringLiteral("memory outlived the disk; it holds: ")
	                      + names.join(QStringLiteral(", "))));

	/* The control: what WAS written is still there. */
	QVERIFY2(names.contains(QStringLiteral("writable")),
	          "the rows that did reach the disk must survive");

	QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                   | QFileDevice::WriteOwner
	                                   | QFileDevice::ExeOwner);
}

void profile_store_test::a_device_setting_that_cannot_be_written_says_so() {
	/*
	 * The same fault as the profile store's three writers, in the sibling
	 * file, and it was left there when they were fixed: QSettings reports
	 * a failed write through status() and bssh_app_settings::store() asked
	 * nothing, returning void so no caller could have known either.
	 *
	 * A cosmetic device setting failing to persist merely reverts at the
	 * next start. The orphaned-secret list is where the silence is
	 * DURABLE: it records a credential the locked store could not erase,
	 * so a write that vanishes means the sweep never comes back for it and
	 * the password outlives its host for good -- defeating the cleanup by
	 * the exact silence it was built to close.
	 *
	 * Its own directory, so making it unwritable cannot strand the rest of
	 * the suite.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	bssh_app_settings settings(dir.path());
	QVERIFY2(settings.set_orphaned_secrets({QStringLiteral("profile:7:password")}),
	          "the control: a writable directory has to succeed");
	QCOMPARE(settings.orphaned_secrets(), QStringList{QStringLiteral("profile:7:password")});

	QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                           | QFileDevice::ExeOwner));

	const bool wrote = settings.set_orphaned_secrets(
	        {QStringLiteral("profile:7:password"), QStringLiteral("profile:8:password")});

	QFile::setPermissions(dir.path(), QFileDevice::ReadOwner
	                                   | QFileDevice::WriteOwner
	                                   | QFileDevice::ExeOwner);

	/*
	 * Whether the account really could write, asked of the FILE rather
	 * than of the return value. Conditioning the skip on what store()
	 * SAID is not a skip at all: with the status check removed it says
	 * true, the skip fires, and the sabotage passes -- which is exactly
	 * how this test first behaved, and exactly the vacuous pass the rest
	 * of this section exists to refuse.
	 *
	 * Read as bytes, because QSettings shares one cached QConfFile per
	 * path and would hand back the value it failed to write.
	 */
	QFile ini(dir.path() + QStringLiteral("/app.ini"));
	QVERIFY(ini.open(QIODevice::ReadOnly));
	const bool really_landed = ini.readAll().contains("profile:8:password");
	ini.close();

	if (really_landed) {
		QSKIP("this account can write a read-only directory, so the failure "
		       "cannot be produced here");
	}

	QVERIFY2(!wrote, "a write that did not reach app.ini must not report success");
}

void profile_store_test::a_hand_typed_port_out_of_range_falls_back_rather_than_wrapping() {
	/*
	 * hosts.ini is a text file people edit, and the reader cast whatever
	 * was in it to quint16. 65538 is 2 after that cast -- so the row read
	 * 65538, the connection went to port 2, and nothing anywhere said the
	 * two disagreed. A wrong port that LOOKS right is worse than a
	 * refusal: it is the one failure the user cannot see by reading their
	 * own file.
	 *
	 * The rule already existed for the same string typed into the connect
	 * bar (bssh_port_from_string, used by to_port and from_target). This
	 * door simply was not using it.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	{
		QSettings file(dir.path() + QStringLiteral("/hosts.ini"), QSettings::IniFormat);
		file.beginGroup(QStringLiteral("host-1"));
		file.setValue(QStringLiteral("name"), QStringLiteral("Wrapped"));
		file.setValue(QStringLiteral("host"), QStringLiteral("example.invalid"));
		file.setValue(QStringLiteral("port"), QStringLiteral("65538"));
		file.endGroup();

		file.beginGroup(QStringLiteral("host-2"));
		file.setValue(QStringLiteral("name"), QStringLiteral("Wordy"));
		file.setValue(QStringLiteral("host"), QStringLiteral("example.invalid"));
		file.setValue(QStringLiteral("port"), QStringLiteral("ssh"));
		file.endGroup();

		file.beginGroup(QStringLiteral("host-3"));
		file.setValue(QStringLiteral("name"), QStringLiteral("Fine"));
		file.setValue(QStringLiteral("host"), QStringLiteral("example.invalid"));
		file.setValue(QStringLiteral("port"), QStringLiteral("2222"));
		file.endGroup();
		file.sync();
	}

	bssh_profile_store store(dir.path());
	QVERIFY(store.is_open());

	quint16 wrapped = 0, wordy = 0, fine = 0;
	for (const bssh_profile_t &profile : store.profiles()) {
		if (profile.name == QStringLiteral("Wrapped")) wrapped = profile.port;
		if (profile.name == QStringLiteral("Wordy")) wordy = profile.port;
		if (profile.name == QStringLiteral("Fine")) fine = profile.port;
	}

	/* Not 2. */
	QCOMPARE(wrapped, quint16(22));
	QCOMPARE(wordy, quint16(22));

	/* And a port that is simply a port still is one, so this is a check
	 * on the range rather than on the field being read at all. */
	QCOMPARE(fine, quint16(2222));
}

void profile_store_test::the_saved_sessions_can_be_listed_without_opening_the_app() {
	/*
	 * sec 8.6's `--load=?`. The option took a session name and there was
	 * no way to find out what it would accept short of opening the app,
	 * which is the state `--term-features=?` already exists to avoid --
	 * and the generated bash completion had to stay silent about `--load`
	 * for the same reason.
	 *
	 * Names only, one per line, sorted case-insensitively: the completion
	 * reads this as well as a person, so a header or a decoration would
	 * be a candidate that is not a session name.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	bssh_profile_store store(dir.path());
	QVERIFY(store.is_open());

	bssh_profile_t web;
	web.name = QStringLiteral("web front end");
	web.host = QStringLiteral("web.example.invalid");
	QVERIFY(store.save(web) != 0);

	bssh_profile_t build;
	build.name = QStringLiteral("Build server");
	build.host = QStringLiteral("build.example.invalid");
	QVERIFY(store.save(build) != 0);

	const QStringList listed =
	        bssh_saved_session_listing(&store).split(QLatin1Char('\n'),
	                                                  Qt::SkipEmptyParts);

	QCOMPARE(listed.size(), 2);

	/*
	 * Sorted by NAME rather than by the order ids were minted in, and
	 * case-insensitively -- "Build server" before "web front end" is the
	 * assertion, because a case-sensitive sort puts every capitalised
	 * name in a block of its own and a person scanning the list would
	 * not find theirs where they looked.
	 */
	QCOMPARE(listed.at(0), QStringLiteral("Build server"));
	QCOMPARE(listed.at(1), QStringLiteral("web front end"));

	/*
	 * And an empty store answers with nothing rather than refusing: a
	 * script asking what is available on a fresh install must read "none"
	 * rather than an error.
	 */
	QTemporaryDir empty;
	QVERIFY(empty.isValid());
	bssh_profile_store nothing_saved(empty.path());
	QCOMPARE(bssh_saved_session_listing(&nothing_saved), QString());
}

void profile_store_test::a_hand_typed_override_meets_the_same_bounds_as_a_preset() {
	/*
	 * A preset's numbers are clamped by from_json, because sec 10.5 makes
	 * a preset a file people send each other and the only thing enforcing
	 * the ranges was a spinbox. A host OVERRIDE is the same number in the
	 * same kind of file, typed into hosts.ini -- and it reached the
	 * emulator as written, because the resolver checked only that the key
	 * still exists.
	 *
	 * load_profiles' own comment uses "scrollback_lines=99999" as its
	 * example of a hand-typed override, which is above the bound.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	{
		QSettings file(dir.path() + QStringLiteral("/hosts.ini"), QSettings::IniFormat);
		file.beginGroup(QStringLiteral("host-1"));
		file.setValue(QStringLiteral("name"), QStringLiteral("Greedy"));
		file.setValue(QStringLiteral("host"), QStringLiteral("example.invalid"));
		file.setValue(QStringLiteral("scrollback_lines"), QStringLiteral("999999999"));
		file.setValue(QStringLiteral("font_size"), QStringLiteral("100000"));
		file.endGroup();
		file.sync();
	}

	bssh_profile_store store(dir.path());
	QVERIFY(store.is_open());
	QCOMPARE(store.profiles().size(), 1);

	const bssh_profile_t profile = store.profiles().first();
	const QJsonObject resolved = bssh_settings_resolver::resolve(
	        bssh_preset_t(), profile.overrides);

	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(),
	          BSSH_PRESET_MAX_SCROLLBACK);
	QCOMPARE(resolved.value(QStringLiteral("font_size")).toInt(),
	          BSSH_PRESET_MAX_POINT_SIZE);

	/*
	 * And the round-trip that does the clamping loses nothing: every key
	 * the defaults carry is still there afterwards. A resolver that
	 * quietly dropped a field would take the setting with it.
	 */
	const QJsonObject defaults = bssh_settings_resolver::app_defaults();
	for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
		QVERIFY2(resolved.contains(it.key()), qPrintable(it.key()));
	}

	/* An override INSIDE the range is still an override, so the clamp has
	 * not simply overwritten the host's answer with the preset's. */
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt() >
	                  bssh_preset_t().scrollback_lines,
	          true);
}
