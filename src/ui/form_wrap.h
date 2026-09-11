#ifndef BSSH_FORM_WRAP_H
#define BSSH_FORM_WRAP_H

#include <QString>

class QFormLayout;
class QWidget;

/*
 * Whether a form puts its labels ABOVE its fields, and at what width
 * (project.md sec 8.2, sec 8.5, sec 8.7).
 *
 * The host editor and the preset editor both hold fields whose contents
 * are a length nobody chose -- a hostname, a key path, a ProxyJump copied
 * out of an ssh_config, a tmux line pasted from a note -- so there is no
 * width that makes them fit and the only thing to do is give them all the
 * room there is. Two columns spend part of that room on a label column,
 * and where the room is short that is the field's room being spent.
 *
 * Both editors asked is_touch_primary() until 2026-09-11, and that is the
 * fault this exists to remove: an unfolded Galaxy Z Fold is touch and is
 * 840dp wide, so it got a layout measured for 320dp across the whole
 * screen, while a desktop window dragged down to 400px kept two columns
 * and starved exactly the fields the phone was stacked to protect. Touch
 * says how big a target has to be (ui/touch_metrics.h); it does not say
 * how much room there is.
 *
 * The width it keys off is sec 8.2's, through bssh_layout_manager --
 * COMPACT stacks and anything wider does not. A second threshold invented
 * here would be a second answer to "is this narrow", and the tree already
 * has one that a desktop window dragged narrow and a folded foldable both
 * land in. Deriving one from the fields' own metrics was tried first and
 * dropped: it put the phone 31px from the wrong answer, and it moved with
 * whatever face the machine drew in -- so the CI container and the device
 * could have disagreed about the layout while both were "measured".
 */

/*
 * What bssh_watch_form_width names the forms it watches, so a test can
 * find them rather than list them.
 */
#define BSSH_WIDTH_KEYED_FORM QStringLiteral("width-keyed-form")

/*
 * The decision, as arithmetic, so a test can hand it numbers the way
 * bssh_layout_manager's own are handed to it.
 */
bool bssh_form_stacks(int width_dp);

/* Sets the form's wrap policy for the width the page currently has. */
void bssh_apply_form_wrap(QFormLayout *form, const QWidget *page);

/*
 * And keeps it there, through an event filter on the page rather than the
 * page's own resizeEvent: the two editors would otherwise carry the same
 * override, and this project has already paid for the touch floor being
 * written in seven places under four names.
 */
void bssh_watch_form_width(QFormLayout *form, QWidget *page);

#endif /* BSSH_FORM_WRAP_H */
