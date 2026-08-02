#include <QtTest>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include "preset_editor_test.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "ui/preset_editor.h"

/* The editable preset the fixtures below work on: a clone, because every
 * shipped one is read-only by design (sec 10.4). */
static int make_editable_preset(bssh_profile_store &store, const QString &name) {
	bssh_preset_t preset;
	preset.name = name;
	preset.scrollback_lines = 5000;
	return store.save_preset(preset);
}

void preset_editor_test::lists_every_preset_including_the_shipped_ones() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	const QListWidget *list = editor.findChild<QListWidget *>();
	QVERIFY(list != nullptr);

	/* The six starter presets are seeded on first open (sec 10.4). */
	QCOMPARE(list->count(), store.presets().size());
	QVERIFY(list->count() > 0);

	/*
	 * Shipped ones are marked in the list itself, so that the reason Save
	 * is unavailable is visible before the button is reached.
	 */
	bool marked = false;
	for (int i = 0; i < list->count(); ++i) {
		if (list->item(i)->text().contains(QStringLiteral("(shipped)"))) marked = true;
	}
	QVERIFY(marked);
}

void preset_editor_test::a_shipped_preset_cannot_be_saved_over() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	/* The list is sorted by name and the starter set is all built-in, so
	 * row 0 is a shipped preset. */
	QListWidget *list = editor.findChild<QListWidget *>();
	list->setCurrentRow(0);

	const bssh_preset_t shown = store.preset(editor.current_preset_id());
	QVERIFY2(shown.is_builtin, "expected the starter set to be built-in");

	QPushButton *save = editor.findChild<QPushButton *>();
	Q_UNUSED(save);

	/*
	 * The store refuses regardless -- this asserts the editor does not
	 * offer the action in the first place. A Save button that silently
	 * does nothing is worse than one that is visibly unavailable, because
	 * the user cannot tell a refusal from a bug.
	 */
	for (QPushButton *button : editor.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Save")) {
			QVERIFY2(!button->isEnabled(), "Save must be disabled for a shipped preset");
		}
		if (button->text() == QStringLiteral("Delete")) {
			QVERIFY2(!button->isEnabled(), "a shipped preset cannot be deleted either");
		}
	}
}

void preset_editor_test::cloning_a_shipped_preset_yields_an_editable_copy() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	QListWidget *list = editor.findChild<QListWidget *>();
	list->setCurrentRow(0);

	const int before = store.presets().size();
	const bssh_preset_t original = store.preset(editor.current_preset_id());

	for (QPushButton *button : editor.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Save as new")) button->click();
	}

	QCOMPARE(store.presets().size(), before + 1);

	const bssh_preset_t clone = store.preset(editor.current_preset_id());
	QVERIFY2(!clone.is_builtin, "a clone must be editable");
	QVERIFY2(clone.name != original.name,
	         "two presets with the same name make an unusable list");
	QCOMPARE(clone.scrollback_lines, original.scrollback_lines);
}

void preset_editor_test::the_host_count_is_shown_before_the_edit() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	for (int i = 0; i < 3; ++i) {
		bssh_profile_t host;
		host.host = QStringLiteral("host%1.example.com").arg(i);
		host.preset_id = preset_id;
		store.save(host);
	}

	bssh_preset_editor editor(&store);
	QListWidget *list = editor.findChild<QListWidget *>();
	for (int i = 0; i < list->count(); ++i) {
		if (list->item(i)->data(Qt::UserRole).toInt() == preset_id) list->setCurrentRow(i);
	}

	/*
	 * sec 10.1's "used by 7 hosts", and it is on screen while the fields
	 * are being typed into rather than only in the confirmation: knowing
	 * beforehand is what stops the edit being a surprise at all.
	 */
	bool found = false;
	for (const QLabel *label : editor.findChildren<QLabel *>()) {
		if (label->text().contains(QStringLiteral("3 hosts"))) found = true;
	}
	QVERIFY2(found, "the number of hosts following the preset must be visible");
}

void preset_editor_test::saving_changes_what_following_hosts_resolve_to() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	bssh_profile_t host;
	host.host = QStringLiteral("one.example.com");
	host.preset_id = preset_id;
	const int host_id = store.save(host);

	/* Edit the preset directly -- what is under test is the resolution
	 * that follows, not the button that triggers it. */
	bssh_preset_t preset = store.preset(preset_id);
	preset.scrollback_lines = 12345;
	store.save_preset(preset);

	const bssh_profile_t stored = store.profile(host_id);
	const QJsonObject resolved =
	        bssh_settings_resolver::resolve(store.preset(preset_id), stored.overrides);

	/* The entire point of the middle layer: one edit, every host. */
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(), 12345);
}

void preset_editor_test::an_overridden_field_does_not_follow_the_preset() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	bssh_profile_t host;
	host.host = QStringLiteral("special.example.com");
	host.preset_id = preset_id;

	QJsonObject overrides;
	overrides.insert(QStringLiteral("scrollback_lines"), 50);
	host.overrides = overrides;
	const int host_id = store.save(host);

	bssh_preset_t preset = store.preset(preset_id);
	preset.scrollback_lines = 12345;
	store.save_preset(preset);

	const bssh_profile_t stored = store.profile(host_id);
	const QJsonObject resolved =
	        bssh_settings_resolver::resolve(store.preset(preset_id), stored.overrides);

	/*
	 * The other half of the promise, and the half that makes the feature
	 * safe to use: a host that said something different keeps saying it.
	 * Propagation that trampled overrides would make every deliberate
	 * exception temporary.
	 */
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(), 50);
}
