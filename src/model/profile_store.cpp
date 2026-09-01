#include "model/profile_store.h"

#include "platform/platform.h"
#include "platform/secret_store.h"
#include "model/app_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

#include <algorithm>

namespace {

/*
 * Everything a host section holds that is NOT a per-field override of its
 * preset. Anything else in the section is one, which is what lets an
 * override be added by typing it into the file (sec 10.1) rather than by
 * knowing some nested syntax.
 */
const char *const BSSH_HOST_KEYS[] = {
	"name", "host", "user", "port", "preset", "has_password", "identity_file",
	"proxy_jump"
};

QString slug_for(const QString &name) {
	QString slug;
	slug.reserve(name.size());

	for (const QChar c : name) {
		if (c.isLetterOrNumber()) {
			slug.append(c.toLower());
		} else if (!slug.isEmpty() && !slug.endsWith(QLatin1Char('-'))) {
			slug.append(QLatin1Char('-'));
		}
	}

	while (slug.endsWith(QLatin1Char('-'))) slug.chop(1);

	/*
	 * A name made entirely of punctuation still has to be addressable:
	 * "preset=" pointing at nothing is worse than an ugly slug.
	 */
	return slug.isEmpty() ? QStringLiteral("preset") : slug;
}

/*
 * INI holds strings; a preset field has a type. The reference types come
 * from a default preset's own JSON, so this cannot drift from the struct:
 * a field added there is coerced correctly here without this being touched.
 */
QJsonValue typed_value(const QString &key, const QString &raw) {
	static const QJsonObject shape = bssh_preset_t().to_json();
	const QJsonValue reference = shape.value(key);

	if (reference.isBool()) {
		return raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
		       raw == QStringLiteral("1");
	}
	if (reference.isDouble()) return raw.toInt();

	return raw;
}

QString raw_value(const QJsonValue &value) {
	if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
	if (value.isDouble()) return QString::number(value.toInt());
	return value.toString();
}

} /* namespace */

bssh_profile_store::bssh_profile_store(const QString &path, QObject *parent)
    : QObject(parent),
      m_open(false) {
	QString dir = path;

	/*
	 * Whether this store is the app's own or one somebody pointed at a
	 * directory. Only the app's own may put its host list in ~/.ssh: a
	 * store constructed WITH a path was told where to live, and reaching
	 * outside it makes every test in the suite -- and every caller with a
	 * throwaway directory -- write into the real ~/.ssh instead. Which is
	 * exactly what happened the first time this was written.
	 */
	m_uses_default_location = dir.isEmpty();

	if (dir.isEmpty()) {
		/*
		 * AppConfigLocation, not AppDataLocation: this is configuration,
		 * and on a desktop that means ~/.config/beerssh rather than a
		 * data directory nobody thinks to look in. On Android both land
		 * inside the app's private storage.
		 */
		dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	} else if (QFileInfo(dir).suffix() == QStringLiteral("db")) {
		/*
		 * A caller still naming the old database file gets its directory.
		 * Writing config next to where someone pointed beats refusing to
		 * open at all.
		 */
		dir = QFileInfo(dir).absolutePath();
	}

	m_open = open(dir);
}

bssh_profile_store::~bssh_profile_store() = default;

bool bssh_profile_store::open(const QString &directory) {
	m_directory = directory;

	if (!QDir().mkpath(m_directory)) {
		m_last_error = QStringLiteral("cannot create %1").arg(m_directory);
		return false;
	}

	migrate_hosts_if_needed();
	load();
	return true;
}

/*
 * Moves an existing hosts.ini to ~/.ssh/beerssh_hosts the first time this
 * runs on a machine that has one.
 *
 * Renamed rather than copied. Two files that both claim to be the host
 * list is precisely the drift this tree keeps getting caught by: the next
 * reader cannot tell which one is authoritative, and edits land in
 * whichever the code happened to pick. If the rename fails, nothing is
 * lost -- hosts_path() will simply keep answering with the old location.
 */
void bssh_profile_store::migrate_hosts_if_needed() {
	const QString target = hosts_path();
	const QString legacy = m_directory + QStringLiteral("/hosts.ini");

	if (target == legacy) return;
	if (QFileInfo(legacy).size() <= 0) return;

	/*
	 * An EMPTY target counts as absent, not as "already migrated".
	 * hosts_path() proves the location is writable by opening it, which
	 * creates the file -- so by the time this runs the target always
	 * exists, and testing existence would skip every migration there was
	 * ever going to be. Nothing is lost by the looser test: a zero-byte
	 * host list holds nothing to overwrite.
	 */
	if (QFileInfo(target).size() > 0) return;
	QFile::remove(target);

	QFile::rename(legacy, target);
}

/*
 * The host list, in ~/.ssh where the rest of someone's SSH setup lives
 * (sec 10.6) -- and in this app's own config directory anywhere that is
 * not an ordinary desktop.
 *
 * Only the HOSTS move. They are SSH material: a name, a host, a user, a
 * port and a key path, which is what ssh_config's Host blocks hold and
 * what someone backing up ~/.ssh expects to take with them. Presets,
 * layout and device settings stay put, because a font size, a colour
 * scheme and which tabs were open are terminal furniture and have no
 * business in ~/.ssh.
 *
 * Named beerssh_hosts rather than beerssh_config, which was the obvious
 * choice and the wrong one: a file called *_config sitting beside
 * OpenSSH's `config` promises ssh_config's grammar, and this is INI
 * (sec 10.6). beerssh_hosts mirrors known_hosts, says what is in it, and
 * promises nothing it does not deliver.
 *
 * Where ~/.ssh is not a real place -- Android, or a sandbox that cannot
 * reach it -- bssh_platform says so and this stays in the config
 * directory.
 */
QString bssh_profile_store::hosts_path() const {
	if (!m_uses_default_location) return m_directory + QStringLiteral("/hosts.ini");

	const bssh_platform *platform = bssh_platform::instance();
	const QString ssh_dir = platform ? platform->openssh_directory() : QString();

	if (!ssh_dir.isEmpty() && QDir().mkpath(ssh_dir)) {
		const QString shared = ssh_dir + QStringLiteral("/beerssh_hosts");

		QFile probe(shared);
		if (probe.open(QIODevice::Append | QIODevice::WriteOnly)) {
			probe.close();
			QFile::setPermissions(shared, QFile::ReadOwner | QFile::WriteOwner);
			return shared;
		}
	}

	return m_directory + QStringLiteral("/hosts.ini");
}

QString bssh_profile_store::presets_path() const {
	return m_directory + QStringLiteral("/presets.ini");
}

QString bssh_profile_store::layout_path() const {
	return m_directory + QStringLiteral("/layout.ini");
}

void bssh_profile_store::load() {
	m_presets.clear();
	m_profiles.clear();
	m_slugs.clear();

	load_presets();
	load_profiles();
}

void bssh_profile_store::load_presets() {
	QSettings file(presets_path(), QSettings::IniFormat);
	const QStringList sections = file.childGroups();

	/*
	 * Seeded once, into a config directory that has never had a presets
	 * file. Re-seeding on every start would resurrect presets the user
	 * deleted, and one that comes back after being removed is worse than
	 * one that was never shipped (sec 10.4).
	 */
	if (sections.isEmpty() && !QFile::exists(presets_path())) {
		for (const bssh_preset_t &builtin : bssh_preset_t::builtins()) {
			bssh_preset_t seeded = builtin;
			seeded.is_builtin = true;
			m_presets.append(seeded);
		}

		sort_presets();

		int id = 0;
		for (bssh_preset_t &seeded : m_presets) {
			seeded.id = ++id;
			m_slugs.insert(seeded.id, unique_slug(slug_for(seeded.name), seeded.id));
		}

		write_presets();
		return;
	}

	/*
	 * The id is stored, not derived from position. It was derived once, and
	 * the ids then changed between one run and the next -- sections load in
	 * a different order from the one they were seeded in, so a caller
	 * holding a preset id across a reopen was pointed at a different
	 * preset. The slug is what a HUMAN references; the id is what the UI
	 * holds, and it has to mean the same thing tomorrow.
	 */
	int highest = 0;
	for (const QString &section : sections) {
		file.beginGroup(section);
		highest = qMax(highest, file.value(QStringLiteral("id"), 0).toInt());
		file.endGroup();
	}

	for (const QString &section : sections) {
		file.beginGroup(section);

		int assigned = file.value(QStringLiteral("id"), 0).toInt();
		if (assigned == 0) assigned = ++highest;   /* a hand-added section */

		const bool builtin = file.value(QStringLiteral("builtin"), false).toBool();

		/*
		 * Read on top of a default preset, so a file that omits a field
		 * -- or a hand-written one that sets only the two fields someone
		 * cared about -- gets the default rather than an empty string.
		 */
		QJsonObject json = bssh_preset_t().to_json();
		json.insert(QStringLiteral("name"), section);

		for (const QString &key : file.childKeys()) {
			if (key == QStringLiteral("builtin") || key == QStringLiteral("id")) continue;
			json.insert(key, typed_value(key, file.value(key).toString()));
		}

		bssh_preset_t preset = bssh_preset_t::from_json(json);
		preset.id = assigned;
		preset.is_builtin = builtin;

		file.endGroup();

		m_presets.append(preset);
		m_slugs.insert(preset.id, section);
	}

	sort_presets();
}

void bssh_profile_store::load_profiles() {
	QSettings file(hosts_path(), QSettings::IniFormat);

	for (const QString &section : file.childGroups()) {
		file.beginGroup(section);

		bssh_profile_t profile;
		profile.id = section.mid(section.lastIndexOf(QLatin1Char('-')) + 1).toInt();
		profile.name = file.value(QStringLiteral("name")).toString();
		profile.host = file.value(QStringLiteral("host")).toString();
		profile.user = file.value(QStringLiteral("user")).toString();
		profile.port = static_cast<quint16>(file.value(QStringLiteral("port"), 22).toUInt());
		profile.identity_file = file.value(QStringLiteral("identity_file")).toString();
		profile.proxy_jump = file.value(QStringLiteral("proxy_jump")).toString();
		profile.has_stored_password =
		        file.value(QStringLiteral("has_password"), false).toBool();
		profile.require_biometric =
		        file.value(QStringLiteral("require_biometric"), false).toBool();
		profile.preset_id = id_for_slug(file.value(QStringLiteral("preset")).toString());

		/*
		 * Every key that is not one of the host's own is a per-field
		 * override of the preset (sec 10.1), typed against the preset
		 * schema -- so "scrollback_lines=99999" typed in by hand arrives
		 * as a number rather than as the string "99999".
		 */
		QJsonObject overrides;
		for (const QString &key : file.childKeys()) {
			bool is_host_key = false;
			for (const char *known : BSSH_HOST_KEYS) {
				if (key == QLatin1String(known)) is_host_key = true;
			}
			if (is_host_key) continue;

			overrides.insert(key, typed_value(key, file.value(key).toString()));
		}
		profile.overrides = overrides;

		file.endGroup();

		if (profile.is_valid()) m_profiles.append(profile);
	}

	sort_profiles();
}

void bssh_profile_store::sort_profiles() {
	std::sort(m_profiles.begin(), m_profiles.end(),
	          [](const bssh_profile_t &a, const bssh_profile_t &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
}

void bssh_profile_store::sort_presets() {
	std::sort(m_presets.begin(), m_presets.end(),
	          [](const bssh_preset_t &a, const bssh_preset_t &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
}

namespace {

/*
 * "name", then "name (imported)", then "name (imported 2)" -- as many as
 * it takes. The suffix says how the entry got here, which is the useful
 * thing to know when two rows would otherwise look identical.
 */
QString unique_among(const QStringList &taken, const QString &wanted) {
	if (!taken.contains(wanted, Qt::CaseInsensitive)) return wanted;

	const QString base = wanted + QStringLiteral(" (imported");
	QString candidate = base + QLatin1Char(')');

	int suffix = 1;
	while (taken.contains(candidate, Qt::CaseInsensitive)) {
		candidate = base + QLatin1Char(' ') + QString::number(++suffix) + QLatin1Char(')');
	}
	return candidate;
}

} /* namespace */

QString bssh_profile_store::unique_preset_name(const QString &wanted) const {
	QStringList taken;
	for (const bssh_preset_t &preset : m_presets) taken.append(preset.name);
	return unique_among(taken, wanted);
}

QString bssh_profile_store::unique_profile_name(const QString &wanted) const {
	QStringList taken;
	for (const bssh_profile_t &profile : m_profiles) taken.append(profile.name);
	return unique_among(taken, wanted);
}

QString bssh_profile_store::unique_slug(const QString &base, int except_id) const {
	QString candidate = base;
	int suffix = 1;

	for (;;) {
		bool taken = false;
		for (auto it = m_slugs.constBegin(); it != m_slugs.constEnd(); ++it) {
			if (it.key() != except_id && it.value() == candidate) taken = true;
		}
		if (!taken) return candidate;

		candidate = base + QLatin1Char('-') + QString::number(++suffix);
	}
}

int bssh_profile_store::id_for_slug(const QString &slug) const {
	if (slug.isEmpty()) return 0;

	for (auto it = m_slugs.constBegin(); it != m_slugs.constEnd(); ++it) {
		if (it.value() == slug) return it.key();
	}

	/*
	 * A host naming a preset that is not there follows nothing rather than
	 * following something arbitrary. sec 10.1 resolves that to the app
	 * defaults, which is the honest answer to "the preset this host wanted
	 * has been deleted".
	 */
	return 0;
}

bool bssh_profile_store::write_presets() {
	QSettings file(presets_path(), QSettings::IniFormat);
	file.clear();

	for (const bssh_preset_t &preset : m_presets) {
		file.beginGroup(m_slugs.value(preset.id, slug_for(preset.name)));

		const QJsonObject json = preset.to_json();
		for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
			/*
			 * The schema version belongs to the export format (sec 10.5),
			 * where a file arrives from somewhere else and its age
			 * matters. Here the file IS the store and its shape is this
			 * build's.
			 */
			if (it.key() == QStringLiteral("schema")) continue;
			file.setValue(it.key(), raw_value(it.value()));
		}

		/*
		 * Written so it survives a reopen. Ugly next to the readable
		 * keys, and the alternative was ids that silently change meaning
		 * when a preset is added above them in the sort order.
		 */
		file.setValue(QStringLiteral("id"), preset.id);

		if (preset.is_builtin) file.setValue(QStringLiteral("builtin"), true);

		file.endGroup();
	}

	file.sync();

	/*
	 * The half nobody asked for. QSettings swallows a failed write and
	 * says nothing, so an unwritable config directory -- a read-only
	 * home, a full disk, permissions somebody changed -- left the row in
	 * memory, the id returned and changed() emitted, and the host gone at
	 * the next start. With one store shared by every window (sec 8.4), an
	 * unwritten change is shown as saved in all of them.
	 */
	if (file.status() != QSettings::NoError) {
		m_last_error = QStringLiteral("could not write %1").arg(file.fileName());
		return false;
	}
	return true;
}

bool bssh_profile_store::write_profiles() {
	QSettings file(hosts_path(), QSettings::IniFormat);
	file.clear();

	for (const bssh_profile_t &profile : m_profiles) {
		file.beginGroup(QStringLiteral("host-%1").arg(profile.id));

		file.setValue(QStringLiteral("name"), profile.name);
		file.setValue(QStringLiteral("host"), profile.host);
		file.setValue(QStringLiteral("user"), profile.user);
		file.setValue(QStringLiteral("port"), profile.port);
		if (!profile.identity_file.isEmpty()) {
			file.setValue(QStringLiteral("identity_file"), profile.identity_file);
		}
		if (!profile.proxy_jump.isEmpty()) {
			file.setValue(QStringLiteral("proxy_jump"), profile.proxy_jump);
		}

		/*
		 * Written only when true, and it is a FLAG, not a secret: it says
		 * a credential exists in the platform store, never what it is
		 * (sec 11). That is what keeps this file safe to copy to another
		 * machine -- where the flag will be true and the keyring empty,
		 * and the app will simply ask.
		 */
		if (profile.has_stored_password) {
			file.setValue(QStringLiteral("has_password"), true);
		}
		if (profile.require_biometric) {
			file.setValue(QStringLiteral("require_biometric"), true);
		}
		if (profile.preset_id != 0) {
			file.setValue(QStringLiteral("preset"), m_slugs.value(profile.preset_id));
		}

		for (auto it = profile.overrides.constBegin();
		     it != profile.overrides.constEnd(); ++it) {
			file.setValue(it.key(), raw_value(it.value()));
		}

		file.endGroup();
	}

	file.sync();

	/*
	 * The half nobody asked for. QSettings swallows a failed write and
	 * says nothing, so an unwritable config directory -- a read-only
	 * home, a full disk, permissions somebody changed -- left the row in
	 * memory, the id returned and changed() emitted, and the host gone at
	 * the next start. With one store shared by every window (sec 8.4), an
	 * unwritten change is shown as saved in all of them.
	 */
	if (file.status() != QSettings::NoError) {
		m_last_error = QStringLiteral("could not write %1").arg(file.fileName());
		return false;
	}
	return true;
}

QList<bssh_profile_t> bssh_profile_store::profiles() const {
	return m_profiles;
}

bssh_profile_t bssh_profile_store::profile(int id) const {
	for (const bssh_profile_t &profile : m_profiles) {
		if (profile.id == id) return profile;
	}
	return bssh_profile_t();
}

bssh_profile_store *bssh_profile_store::shared() {
	static bssh_profile_store *store = new bssh_profile_store;
	return store;
}

int bssh_profile_store::save(const bssh_profile_t &profile) {
	if (!m_open || !profile.is_valid()) return 0;

	const QList<bssh_profile_t> before_write = m_profiles;

	bssh_profile_t stored = profile;
	if (stored.name.isEmpty()) stored.name = stored.host;

	if (stored.id == 0) {
		int highest = 0;
		for (const bssh_profile_t &existing : m_profiles) {
			highest = qMax(highest, existing.id);
		}
		stored.id = highest + 1;
		m_profiles.append(stored);
	} else {
		bool replaced = false;
		for (bssh_profile_t &existing : m_profiles) {
			if (existing.id != stored.id) continue;
			existing = stored;
			replaced = true;
		}
		if (!replaced) m_profiles.append(stored);
	}

	sort_profiles();
	if (!write_profiles()) {
		/*
	 * Restored rather than reloaded, and the difference is not
	 * stylistic: QSettings shares one cached QConfFile per path, so a
	 * write that failed still leaves its values in that cache and a
	 * fresh QSettings on the same file reads them back. load() therefore
	 * returns the row that never reached the disk. Measured, after a
	 * test that expected reloading to undo it and got "doomed" back.
	 *
	 * Memory must not outlive the disk, because one store is shared by
	 * every window (sec 8.4): a change that never reached the file would
	 * otherwise be shown as saved in all of them and be gone at the next
	 * start.
	 */
		/*
	 * Memory must not outlive the disk. The row is dropped again by
	 * reloading, because one store is shared by every window (sec 8.4)
	 * and a change that never reached the file would otherwise be shown
	 * as saved in all of them -- and be gone at the next start.
	 */
		m_profiles = before_write;
		emit changed();
		return 0;
	}

	emit changed();
	return stored.id;
}

bool bssh_profile_store::remove(int id) {
	if (!m_open || id == 0) return false;

	const QList<bssh_profile_t> before_write = m_profiles;

	/* Read while the row still exists; see the erase below. */
	const bool had_password = profile(id).has_stored_password;

	const qsizetype before = m_profiles.size();
	m_profiles.erase(std::remove_if(m_profiles.begin(), m_profiles.end(),
	                                 [id](const bssh_profile_t &p) { return p.id == id; }),
	                  m_profiles.end());

	if (m_profiles.size() == before) return false;


	/*
	 * And the credential with it (sec 11). The row held only a FLAG
	 * saying one existed; the secret lives in bssh_secret_store under a
	 * key derived from this id, and nothing joined the two ends -- so a
	 * password for a host the user deliberately deleted stayed in their
	 * keyring, or in the encrypted file sec 11 calls safe to copy between
	 * machines, for good. bssh_secret_store::erase() had NO CALLER
	 * anywhere in the tree, which is how half an interface goes unused
	 * without anybody noticing.
	 *
	 * The flag is read before the row goes, because it is the cheap way
	 * to know a secret exists at all: asking the store would mean
	 * unlocking it on every deletion, including the many with nothing to
	 * erase.
	 *
	 * The erase is not required to succeed and the deletion does not
	 * depend on it. A libsecret store erases whether or not this app has
	 * unlocked anything; the passphrase-derived file cannot, since it
	 * re-encrypts the whole blob and returns false while locked. What to
	 * do about that -- prompt on delete, or record the orphan and sweep
	 * it at the next unlock -- is a question sec 11 does not answer, and
	 * this does not invent one: a locked store is left exactly as it was
	 * before this change, no worse and no better.
	 */
	if (had_password) {
		const QString key = bssh_profile_password_key(id);
		bssh_secret_store *secrets = bssh_secret_store::instance();
		if ((!secrets || !secrets->erase(key)) && !bssh_note_orphaned_secret(key)) {
			/*
			 * Neither erased nor written down, so nothing will come
			 * back for it. Recorded rather than swallowed: the row is
			 * gone as asked, and this says the credential outlived it.
			 */
			m_last_error = QStringLiteral(
			        "removed the host, but could not remove or schedule its "
			        "saved password");
		}
	}

	if (!write_profiles()) {
		/* See save(): restored, not reloaded. */
		m_profiles = before_write;
		emit changed();
		return false;
	}

	emit changed();
	return true;
}

QList<bssh_preset_t> bssh_profile_store::presets() const {
	return m_presets;
}

bssh_preset_t bssh_profile_store::preset(int id) const {
	for (const bssh_preset_t &preset : m_presets) {
		if (preset.id == id) return preset;
	}
	return bssh_preset_t();
}

int bssh_profile_store::save_preset(const bssh_preset_t &preset) {
	const QList<bssh_preset_t> presets_before = m_presets;
	const QHash<int, QString> slugs_before = m_slugs;
	if (!m_open || preset.name.isEmpty()) return 0;

	/* Shipped presets are cloned, not edited (sec 10.4). */
	if (preset.id != 0 && this->preset(preset.id).is_builtin) return 0;

	bssh_preset_t stored = preset;

	if (stored.id == 0) {
		int highest = 0;
		for (const bssh_preset_t &existing : m_presets) {
			highest = qMax(highest, existing.id);
		}
		stored.id = highest + 1;
		stored.is_builtin = false;

		m_slugs.insert(stored.id, unique_slug(slug_for(stored.name), stored.id));
		m_presets.append(stored);
	} else {
		/*
		 * A rename moves the section, so every host pointing at the old
		 * slug is rewritten with it. In a file keyed by name, renaming is
		 * the operation that breaks references, and leaving seven hosts
		 * pointing at a section that no longer exists would drop all
		 * seven back to the app defaults without saying so.
		 */
		const QString wanted = unique_slug(slug_for(stored.name), stored.id);
		const bool renamed = m_slugs.value(stored.id) != wanted;
		if (renamed) m_slugs.insert(stored.id, wanted);

		for (bssh_preset_t &existing : m_presets) {
			if (existing.id == stored.id) existing = stored;
		}

		if (renamed) write_profiles();
	}

	sort_presets();
	if (!write_presets()) {
		/* See save(): restored, not reloaded. */
		m_presets = presets_before;
		m_slugs = slugs_before;
		emit changed();
		return 0;
	}

	emit changed();
	return stored.id;
}

bool bssh_profile_store::remove_preset(int id) {
	const QList<bssh_preset_t> presets_before = m_presets;
	const QHash<int, QString> slugs_before = m_slugs;
	if (!m_open || id == 0) return false;
	if (preset(id).is_builtin) return false;

	/*
	 * Refused while hosts still follow it. They would silently fall back
	 * to app defaults, which is a change to every one of them made by
	 * deleting something else.
	 */
	if (hosts_using(id) > 0) return false;

	const qsizetype before = m_presets.size();
	m_presets.erase(std::remove_if(m_presets.begin(), m_presets.end(),
	                                [id](const bssh_preset_t &p) { return p.id == id; }),
	                 m_presets.end());

	if (m_presets.size() == before) return false;

	m_slugs.remove(id);
	if (!write_presets()) {
		/* See save(): restored, not reloaded. */
		m_presets = presets_before;
		m_slugs = slugs_before;
		emit changed();
		return false;
	}

	emit changed();
	return true;
}

int bssh_profile_store::hosts_using(int preset_id) const {
	if (preset_id == 0) return 0;

	int count = 0;
	for (const bssh_profile_t &profile : m_profiles) {
		if (profile.preset_id == preset_id) ++count;
	}
	return count;
}

bool bssh_profile_store::save_layout(const layout_state_t &layout) {
	if (!m_open) return false;

	QSettings file(layout_path(), QSettings::IniFormat);
	file.clear();

	QStringList ids;
	for (int id : layout.profile_ids) ids.append(QString::number(id));

	file.beginGroup(QStringLiteral("layout"));
	file.setValue(QStringLiteral("hosts"), ids.join(QLatin1Char(',')));
	file.setValue(QStringLiteral("panes"), layout.pane_count);
	file.setValue(QStringLiteral("active"), layout.active_index);
	file.endGroup();
	file.sync();

	/* Same silence as the two writers above; see write_profiles. */
	if (file.status() != QSettings::NoError) {
		m_last_error = QStringLiteral("could not write %1").arg(file.fileName());
		return false;
	}

	return true;
}

bssh_profile_store::layout_state_t bssh_profile_store::layout() const {
	layout_state_t state;
	if (!m_open) return state;

	QSettings file(layout_path(), QSettings::IniFormat);
	file.beginGroup(QStringLiteral("layout"));

	const QStringList ids = file.value(QStringLiteral("hosts")).toString()
	                                .split(QLatin1Char(','), Qt::SkipEmptyParts);
	const int panes = file.value(QStringLiteral("panes"), 1).toInt();
	const int active = file.value(QStringLiteral("active"), 0).toInt();
	file.endGroup();

	/*
	 * Ids that no longer exist are dropped, so a tab is never restored for
	 * a host the user deleted -- that would be the store second-guessing
	 * them.
	 */
	for (const QString &raw : ids) {
		const int id = raw.toInt();
		if (profile(id).is_valid()) state.profile_ids.append(id);
	}

	state.pane_count = qBound(1, panes, qMax(1, int(state.profile_ids.size())));
	state.active_index = qBound(0, active, qMax(0, int(state.profile_ids.size()) - 1));

	return state;
}

bool bssh_note_orphaned_secret(const QString &key) {
	if (key.isEmpty()) return false;

	bssh_app_settings settings;
	QStringList pending = settings.orphaned_secrets();
	if (pending.contains(key)) return true;

	pending << key;
	return settings.set_orphaned_secrets(pending);
}

void bssh_sweep_orphaned_secrets() {
	bssh_app_settings settings;
	const QStringList pending = settings.orphaned_secrets();
	if (pending.isEmpty()) return;

	bssh_secret_store *secrets = bssh_secret_store::instance();
	if (!secrets) return;

	/*
	 * Kept when the erase fails, dropped when it succeeds. A store that
	 * is still locked leaves the list exactly as it was, so the sweep is
	 * safe to call from anywhere and costs nothing when it cannot act.
	 */
	QStringList still_pending;
	for (const QString &key : pending) {
		if (!secrets->erase(key)) still_pending << key;
	}

	if (still_pending != pending) settings.set_orphaned_secrets(still_pending);
}
