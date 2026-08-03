#include "ui/override_badge.h"

#include "model/preset.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "platform/platform.h"
#include "term/font_catalog.h"

bssh_override_badge_t bssh_font_override_badge(const bssh_profile_store *store,
                                                const bssh_profile_t &profile,
                                                const QString &picked_font_id,
                                                int picked_size) {
	bssh_override_badge_t badge;
	badge.overridden = false;

	const bssh_preset_t preset = store && profile.preset_id != 0
	        ? store->preset(profile.preset_id)
	        : bssh_preset_t();

	QJsonObject without = profile.overrides;
	without.remove(QStringLiteral("font_family"));
	without.remove(QStringLiteral("font_size"));

	const QJsonObject followed = profile.preset_id == 0
	        ? bssh_settings_resolver::app_defaults()
	        : bssh_settings_resolver::resolve(preset, without);

	const bssh_platform *platform = bssh_platform::instance();
	const bool compact = platform && platform->is_touch_primary();
	const bssh_font_catalog &catalog = bssh_font_catalog::instance();

	const QString inherited = catalog.effective_id(
	        followed.value(QStringLiteral("font_family")).toString(), compact);
	const int inherited_size = followed.value(QStringLiteral("font_size")).toInt();

	badge.overridden = picked_font_id != inherited || picked_size != inherited_size;

	const QString source = profile.preset_id == 0
	        ? QStringLiteral("the app defaults")
	        : QStringLiteral("preset \"%1\"").arg(preset.name);

	if (!badge.overridden) {
		badge.text = QStringLiteral("Following %1.").arg(source);
		return badge;
	}

	/*
	 * Names what it would give as well as what it overrides: "overriding"
	 * on its own does not tell the user what pressing revert gets back.
	 */
	const bssh_font_t *font = catalog.by_id(inherited);
	badge.text = QStringLiteral("<b>Overrides %1</b>, which uses %2 at %3 pt.")
	                     .arg(source,
	                           font ? font->display_name : inherited,
	                           QString::number(inherited_size));
	return badge;
}
