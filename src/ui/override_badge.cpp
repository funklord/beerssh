#include "ui/override_badge.h"

#include "model/preset.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "ui/terminal_fields.h"

namespace {

/* The three appearance fields a host can override (sec 10.1). */
const char *const BSSH_APPEARANCE_KEYS[] = {
	"font_family", "font_size", "colour_scheme"
};

struct inherited_t {
	QString font_id;
	int font_size;
	QString scheme_id;
	QString source;      /* named for the badge: a preset, or the app defaults */
	bssh_preset_t preset;
};

/*
 * What this host would show with no appearance override of its own.
 *
 * The single definition of "inherits", used by both the badge and the
 * override calculation. They ask the same question and had two answers:
 * the write path compared against what the host resolves to WITH the
 * override applied, which is itself, so it could never see agreement.
 *
 * Run through the same effective_id rules the picker used to fill itself
 * in -- comparing a picker's id against a raw stored value compares an id
 * with an empty string and records an override on every save.
 */
inherited_t inherited_appearance(const bssh_profile_store *store,
                                  const bssh_profile_t &profile) {
	inherited_t out;

	out.preset = store && profile.preset_id != 0 ? store->preset(profile.preset_id)
	                                             : bssh_preset_t();

	QJsonObject without = profile.overrides;
	for (const char *key : BSSH_APPEARANCE_KEYS) without.remove(QLatin1String(key));

	const QJsonObject followed =
	        profile.preset_id == 0 ? bssh_settings_resolver::app_defaults()
	                               : bssh_settings_resolver::resolve(out.preset, without);

	out.font_id = bssh_font_catalog::instance().effective_id(
	        followed.value(QStringLiteral("font_family")).toString());
	out.font_size = followed.value(QStringLiteral("font_size")).toInt();
	out.scheme_id = bssh_colour_scheme_catalog::instance().effective_id(
	        followed.value(QStringLiteral("colour_scheme")).toString());

	out.source = profile.preset_id == 0
	        ? QStringLiteral("this device's defaults")
	        : QStringLiteral("preset \"%1\"").arg(out.preset.name);

	return out;
}

} /* namespace */

bssh_override_badge_t bssh_appearance_override_badge(const bssh_profile_store *store,
                                                      const bssh_profile_t &profile,
                                                      const bssh_appearance_pick_t &picked,
                                                      const bssh_appearance_pick_t &shown) {
	const inherited_t inherited = inherited_appearance(store, profile);

	/*
	 * Read off what bssh_appearance_overrides() would write, so the badge
	 * and the save cannot disagree: they did, twice, one field over each
	 * time (see the header).
	 */
	const QJsonObject own = bssh_appearance_overrides(store, profile, picked, shown);
	const bool scheme_differs = own.contains(QStringLiteral("colour_scheme"));

	bssh_override_badge_t badge;
	badge.overridden = own.contains(QStringLiteral("font_family")) ||
	                    own.contains(QStringLiteral("font_size")) || scheme_differs;

	if (!badge.overridden) {
		badge.text = QStringLiteral("Following %1.").arg(inherited.source);
		return badge;
	}

	/*
	 * Names what it would give as well as what it overrides: "overriding"
	 * on its own does not tell the user what pressing revert gets back.
	 */
	const QString font_name = bssh_font_catalog::instance().display_name(inherited.font_id);
	const bssh_colour_scheme_t *scheme =
	        bssh_colour_scheme_catalog::instance().by_id(inherited.scheme_id);

	badge.text = QStringLiteral("<b>Overrides %1</b>, which uses %2 at %3 pt")
	                     .arg(inherited.source,
	                           font_name,
	                           QString::number(inherited.font_size));

	/*
	 * The colours are named only when they are part of what is being
	 * overridden. A badge that recites every inherited value is one
	 * nobody finishes reading.
	 */
	if (scheme_differs) {
		badge.text += QStringLiteral(" and the %1 colours")
		                      .arg(scheme ? scheme->display_name : inherited.scheme_id);
	}

	badge.text += QLatin1Char('.');
	return badge;
}

QJsonObject bssh_appearance_overrides(const bssh_profile_store *store,
                                       const bssh_profile_t &profile,
                                       const bssh_appearance_pick_t &picked,
                                       const bssh_appearance_pick_t &shown) {
	Q_UNUSED(store);
	QJsonObject overrides = profile.overrides;

	if (picked.font_id != shown.font_id) overrides.insert(QStringLiteral("font_family"), picked.font_id);
	else if (overrides.contains(QStringLiteral("font_family")))
		overrides.insert(QStringLiteral("font_family"), picked.font_id);

	if (picked.size != shown.size) overrides.insert(QStringLiteral("font_size"), picked.size);
	else if (overrides.contains(QStringLiteral("font_size")))
		overrides.insert(QStringLiteral("font_size"), picked.size);

	/*
	 * An empty pick means the picker has no opinion yet rather than "the
	 * default scheme": writing one then would record an override the user
	 * never made.
	 */
	if (!picked.scheme_id.isEmpty() && picked.scheme_id != shown.scheme_id)
		overrides.insert(QStringLiteral("colour_scheme"), picked.scheme_id);
	else if (!picked.scheme_id.isEmpty() && overrides.contains(QStringLiteral("colour_scheme")))
		overrides.insert(QStringLiteral("colour_scheme"), picked.scheme_id);

	return overrides;
}

QJsonObject bssh_inherited_settings(const bssh_profile_store *store,
                                     const bssh_profile_t &profile) {
	QJsonObject without = profile.overrides;
	for (const QString &key : bssh_terminal_fields::keys()) without.remove(key);
	if (!store || profile.preset_id == 0) return bssh_settings_resolver::for_host(nullptr, without);
	const bssh_preset_t preset = store->preset(profile.preset_id);
	return bssh_settings_resolver::for_host(&preset, without);
}

QJsonObject bssh_terminal_overrides(const bssh_profile_t &profile, const QJsonObject &own) {
	QJsonObject out = profile.overrides;
	for (const QString &key : bssh_terminal_fields::keys()) out.remove(key);
	for (auto it = own.constBegin(); it != own.constEnd(); ++it) out.insert(it.key(), it.value());
	return out;
}
