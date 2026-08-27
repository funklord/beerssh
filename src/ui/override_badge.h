#ifndef BSSH_OVERRIDE_BADGE_H
#define BSSH_OVERRIDE_BADGE_H

#include <QJsonObject>
#include <QString>

#include "model/profile.h"

class bssh_profile_store;

/*
 * sec 10.1's "is this host following its preset, or overriding it?", in
 * one place for both front doors.
 *
 * The three layers only mean anything if a person can see which one a
 * value came from: without this, "this host looks different from the
 * others" and "this host overrides its preset" are the same observation,
 * and the way back to following the preset is deleting the host and
 * adding it again.
 *
 * Here rather than in either editor because there are two of them
 * (sec 8.5) and the answer must not depend on which one you opened. The
 * PuTTY-shaped dialog had this and the touch page did not, which is
 * exactly the shape of drift a shared answer prevents.
 */
struct bssh_override_badge_t {
	/* Whether the picked font differs from what the host would inherit. */
	bool overridden;

	/* One sentence, ready to show. Rich text -- the desktop bolds it. */
	QString text;
};

/*
 * Compared against what the host would resolve to with NO font override
 * of its own, not against what it resolves to now -- the latter already
 * includes the override and would always compare equal.
 */
bssh_override_badge_t bssh_appearance_override_badge(const bssh_profile_store *store,
                                                      const bssh_profile_t &profile,
                                                      const QString &picked_font_id,
                                                      int picked_size,
                                                      const QString &picked_scheme_id);

/*
 * The sparse appearance overrides this host should carry, given what the
 * picker is showing. Returns the profile's overrides with font_family,
 * font_size and colour_scheme inserted where they differ from what the
 * host INHERITS, and removed where they now agree with it.
 *
 * Paired with the badge above and sharing its idea of "inherits", because
 * the two disagreeing is not a cosmetic bug: the desktop dialog compared
 * the picker against what the host resolves to WITH its current override
 * still applied, so choosing the value the preset already gives recorded
 * an override pinning it -- while the badge, comparing against the value
 * without it, said "Following". The screen said one thing and the store
 * held another.
 *
 * Removing when they agree is the other half. Insert-only leaves a host
 * that has been talked back into agreeing with its preset still carrying
 * an override, which stops following the next time the preset changes.
 */
QJsonObject bssh_appearance_overrides(const bssh_profile_store *store,
                                       const bssh_profile_t &profile,
                                       const QString &picked_font_id,
                                       int picked_size,
                                       const QString &picked_scheme_id);

#endif /* BSSH_OVERRIDE_BADGE_H */
