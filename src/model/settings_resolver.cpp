#include "model/settings_resolver.h"

#include "model/app_settings.h"

QJsonObject bssh_settings_resolver::app_defaults() {
	/*
	 * A default-constructed preset IS the app defaults, rather than a
	 * second list of values kept alongside it. Two lists drift, and the
	 * drift shows up as a host behaving differently depending on whether
	 * a preset was attached -- which is precisely the bug this layering
	 * exists to prevent.
	 */
	QJsonObject defaults = bssh_preset_t().to_json();

	/*
	 * And what this device set for every host over the built-in values,
	 * each typed as its built-in value is: app.ini holds text, and a
	 * "false" laid over a bool unconverted would read as true. The name
	 * and schema are the preset's own, never a default. An empty font or
	 * scheme, or a size of 0, is the built-in one.
	 */
	const QJsonObject stored = bssh_app_settings().default_settings();
	for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
		const QString &key = it.key();
		if (key == QLatin1String("name") || key == QLatin1String("schema")) continue;
		const QJsonValue built_in = defaults.value(key);
		if (built_in.isUndefined()) continue;
		const QVariant raw = it.value().toVariant();
		QJsonValue typed;
		if (built_in.isBool()) {
			typed = raw.toString() == QLatin1String("true") || raw.toString() == QLatin1String("1")
			        || (raw.typeId() == QMetaType::Bool && raw.toBool());
		} else if (built_in.isDouble()) {
			bool ok = false;
			const int number = raw.toString().toInt(&ok);
			if (!ok) continue;
			typed = number;
		} else {
			typed = raw.toString();
		}
		if ((key == QLatin1String("font_family") || key == QLatin1String("colour_scheme"))
		    && typed.toString().isEmpty())
			continue;
		if (key == QLatin1String("font_size") && typed.toInt() <= 0) continue;
		defaults.insert(key, typed);
	}
	return defaults;
}

QJsonObject bssh_settings_resolver::resolve(const bssh_preset_t &preset,
                                             const QJsonObject &overrides) {
	return for_host(&preset, overrides);
}

QJsonObject bssh_settings_resolver::for_host(const bssh_preset_t *preset,
                                              const QJsonObject &overrides) {
	QJsonObject resolved = app_defaults();

	/* Preset over defaults. */
	const QJsonObject preset_json = preset ? preset->to_json() : QJsonObject();
	for (auto it = preset_json.constBegin(); it != preset_json.constEnd(); ++it) {
		resolved.insert(it.key(), it.value());
	}

	/*
	 * Host over preset -- but only for keys the preset actually has. An
	 * override naming a field that no longer exists is stale data from an
	 * older version, and letting it through would put a key in the
	 * resolved settings that nothing reads and no UI can revert.
	 */
	for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
		if (!resolved.contains(it.key())) continue;
		resolved.insert(it.key(), it.value());
	}

	/*
	 * Back through the preset reader, which is where the numeric bounds
	 * live -- because a host OVERRIDE reaches here without having met
	 * them. from_json() clamps scrollback, font size and keepalive for
	 * exactly the reason sec 10.5 gives: the only thing enforcing those
	 * ranges was a spinbox, and a file does not go through one. That
	 * argument covers hosts.ini precisely as well as presets.ini -- the
	 * load comment above uses "scrollback_lines=99999" as its own example
	 * of a hand-typed override, and until this round-trip that number
	 * arrived at the emulator as written.
	 *
	 * Done by round-tripping through the struct rather than by a second
	 * list of qBound calls here, for the reason typed_value() gives: two
	 * spellings of one schema drift, and the drift would appear as a
	 * bound that holds for a preset and not for a host.
	 */
	return bssh_preset_t::from_json(resolved).to_json();
}

QSet<QString> bssh_settings_resolver::overridden_fields(const bssh_preset_t &preset,
                                                         const QJsonObject &overrides) {
	QSet<QString> fields;
	const QJsonObject preset_json = preset.to_json();

	for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
		if (!preset_json.contains(it.key())) continue;

		/*
		 * An override equal to the preset's value is not an override.
		 * Badging it would tell the user this host differs when it does
		 * not, and offering "revert" would do nothing visible -- both of
		 * which teach people to ignore the badge.
		 */
		if (preset_json.value(it.key()) == it.value()) continue;

		fields.insert(it.key());
	}

	return fields;
}

QJsonObject bssh_settings_resolver::revert_field(const QJsonObject &overrides,
                                                  const QString &field) {
	QJsonObject result = overrides;
	result.remove(field);
	return result;
}
