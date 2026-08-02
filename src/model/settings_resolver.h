#ifndef BSSH_SETTINGS_RESOLVER_H
#define BSSH_SETTINGS_RESOLVER_H

#include <QJsonObject>
#include <QSet>
#include <QString>

#include "model/preset.h"

/*
 * The three-layer resolution from project.md sec 10.1:
 *
 *     app defaults  <-  preset ("work-bastion")  <-  host overrides
 *        (lowest)                                        (highest)
 *
 * Kept as a pure function over JSON objects rather than as a method on
 * anything, for two reasons. It is the piece a user will argue with --
 * "why is this host using that font" -- so it has to be inspectable and
 * testable on its own. And the UI needs to know not just the winning
 * value but WHICH layer won, because sec 10.1 requires an override badge
 * with a one-tap revert; a resolver that only returns the answer cannot
 * tell you where it came from.
 */
class bssh_settings_resolver {
public:
	/*
	 * The winning value for every field, given a preset and the host's
	 * own overrides. Overrides are a sparse map: a field absent from it
	 * takes the preset's value, which is what makes editing a preset
	 * propagate to every host that has not overridden that one field.
	 */
	static QJsonObject resolve(const bssh_preset_t &preset, const QJsonObject &overrides);

	/*
	 * Which fields the host overrode -- exactly the set that gets a badge
	 * and a revert control. Fields whose override equals the preset's
	 * value are NOT included: an "override" that changes nothing is a
	 * badge that tells the user something untrue.
	 */
	static QSet<QString> overridden_fields(const bssh_preset_t &preset,
	                                        const QJsonObject &overrides);

	/*
	 * Drops an override, so the field follows the preset again. Returns
	 * the new override map; sec 10.1's "revert to preset value".
	 */
	static QJsonObject revert_field(const QJsonObject &overrides, const QString &field);

	/* App defaults, the bottom layer: a preset with nothing set. */
	static QJsonObject app_defaults();
};

#endif /* BSSH_SETTINGS_RESOLVER_H */
