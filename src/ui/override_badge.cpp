#include "ui/override_badge.h"

#include "model/preset.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"

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
	        ? QStringLiteral("the app defaults")
	        : QStringLiteral("preset \"%1\"").arg(out.preset.name);

	return out;
}

} /* namespace */

bssh_override_badge_t bssh_appearance_override_badge(const bssh_profile_store *store,
                                                      const bssh_profile_t &profile,
                                                      const QString &picked_font_id,
                                                      int picked_size,
                                                      const QString &picked_scheme_id) {
	const inherited_t inherited = inherited_appearance(store, profile);

	/*
	 * The SAME three fields bssh_appearance_overrides() writes, and the
	 * same rule about an empty scheme meaning "the picker has no opinion
	 * yet". This used to weigh the font and the size only, so changing
	 * nothing but the colour scheme wrote an override while the badge one
	 * line below went on saying "Following preset X" -- the screen and
	 * the store disagreeing, which is the single thing this helper exists
	 * to prevent (see the header).
	 *
	 * If the two ever diverge again they must diverge here, together.
	 */
	const bool scheme_differs = !picked_scheme_id.isEmpty() &&
	                             picked_scheme_id != inherited.scheme_id;

	bssh_override_badge_t badge;
	badge.overridden = picked_font_id != inherited.font_id ||
	                    picked_size != inherited.font_size ||
	                    scheme_differs;

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
                                       const QString &picked_font_id,
                                       int picked_size,
                                       const QString &picked_scheme_id) {
	const inherited_t inherited = inherited_appearance(store, profile);

	QJsonObject overrides = profile.overrides;

	if (picked_font_id != inherited.font_id) {
		overrides.insert(QStringLiteral("font_family"), picked_font_id);
	} else {
		overrides.remove(QStringLiteral("font_family"));
	}

	if (picked_size != inherited.font_size) {
		overrides.insert(QStringLiteral("font_size"), picked_size);
	} else {
		overrides.remove(QStringLiteral("font_size"));
	}

	/*
	 * An empty pick means the picker has no opinion yet rather than "the
	 * default scheme": writing one then would record an override the user
	 * never made.
	 */
	if (!picked_scheme_id.isEmpty() && picked_scheme_id != inherited.scheme_id) {
		overrides.insert(QStringLiteral("colour_scheme"), picked_scheme_id);
	} else if (picked_scheme_id == inherited.scheme_id) {
		overrides.remove(QStringLiteral("colour_scheme"));
	}

	return overrides;
}
