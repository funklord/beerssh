#include "model/profile_store.h"

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
	"name", "host", "user", "port", "preset", "has_password"
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

	load();
	return true;
}

QString bssh_profile_store::hosts_path() const {
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
		profile.has_stored_password =
		        file.value(QStringLiteral("has_password"), false).toBool();
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

void bssh_profile_store::write_presets() {
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
}

void bssh_profile_store::write_profiles() {
	QSettings file(hosts_path(), QSettings::IniFormat);
	file.clear();

	for (const bssh_profile_t &profile : m_profiles) {
		file.beginGroup(QStringLiteral("host-%1").arg(profile.id));

		file.setValue(QStringLiteral("name"), profile.name);
		file.setValue(QStringLiteral("host"), profile.host);
		file.setValue(QStringLiteral("user"), profile.user);
		file.setValue(QStringLiteral("port"), profile.port);

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

int bssh_profile_store::save(const bssh_profile_t &profile) {
	if (!m_open || !profile.is_valid()) return 0;

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
	write_profiles();

	emit changed();
	return stored.id;
}

bool bssh_profile_store::remove(int id) {
	if (!m_open || id == 0) return false;

	const qsizetype before = m_profiles.size();
	m_profiles.erase(std::remove_if(m_profiles.begin(), m_profiles.end(),
	                                 [id](const bssh_profile_t &p) { return p.id == id; }),
	                  m_profiles.end());

	if (m_profiles.size() == before) return false;

	write_profiles();
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
	write_presets();

	emit changed();
	return stored.id;
}

bool bssh_profile_store::remove_preset(int id) {
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
	write_presets();

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
