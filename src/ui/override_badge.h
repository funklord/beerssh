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

/* What a host's font picker shows: a font, a size and a colour scheme. */
struct bssh_appearance_pick_t {
	QString font_id;
	int size = 0;
	QString scheme_id;
};

/*
 * Whether the host has an appearance of its own -- a field it carries, or
 * one moved on the picker from what it was `shown` -- and what it would
 * follow without: what it resolves to with NO font override of its own,
 * since what it resolves to now includes the override.
 */
bssh_override_badge_t bssh_appearance_override_badge(const bssh_profile_store *store,
                                                      const bssh_profile_t &profile,
                                                      const bssh_appearance_pick_t &picked,
                                                      const bssh_appearance_pick_t &shown);

/*
 * The sparse appearance overrides this host should carry: the profile's,
 * with font_family, font_size and colour_scheme set where the picker moved
 * from what it was `shown`, and the rest as they were. A field moved is a
 * choice whatever it now equals -- the value the template gives included
 * -- and only the revert puts it back to following (the holder's rule,
 * 2026-11-06, settled for beerssh 2026-11-09). Until then a pick equal to
 * what the host inherits was dropped, and the host stopped holding it the
 * moment the template moved.
 *
 * Paired with the badge above, because the two disagreeing is not a
 * cosmetic bug: the screen saying "Following" while the store holds an
 * override is what both are built to rule out.
 */
QJsonObject bssh_appearance_overrides(const bssh_profile_store *store,
                                       const bssh_profile_t &profile,
                                       const bssh_appearance_pick_t &picked,
                                       const bssh_appearance_pick_t &shown);

/*
 * The same for the terminal and connection fields (bssh_terminal_fields),
 * shared by the desktop dialog and the touch host editor for the same
 * reason: what a host would have without settings of its own, and its
 * overrides with the fields chosen on the form, `own`, as its own -- a pick
 * equal to what it would have without it included, since choosing it is
 * still a choice (the holder's rule, 2026-11-06).
 */
QJsonObject bssh_inherited_settings(const bssh_profile_store *store,
                                     const bssh_profile_t &profile);
QJsonObject bssh_terminal_overrides(const bssh_profile_t &profile, const QJsonObject &own);

#endif /* BSSH_OVERRIDE_BADGE_H */
