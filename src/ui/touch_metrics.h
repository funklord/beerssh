#ifndef BSSH_TOUCH_METRICS_H
#define BSSH_TOUCH_METRICS_H

class QWidget;

/*
 * The sizes a finger needs (project.md sec 6.2, sec 8.5).
 *
 * One number, one rule -- which it had not been. sec 6.2's touch floor was
 * a static in SEVEN files under FOUR names: BSSH_TOUCH_HEIGHT_DP in the
 * font picker, the password prompt and the preset editor;
 * BSSH_TOUCH_TARGET_DP in the window, the key row and the pane container;
 * BSSH_FIELD_HEIGHT_DP in the host editor; and a bare 48 fifteen times over
 * in the settings page and the licences view. Every one of them was the
 * same 48, and the danger is not the duplication -- it is that a rule
 * written in seven places is one that can be changed in six.
 */

/*
 * The smallest thing a thumb can reliably hit, in device-independent
 * pixels. 48 is the floor Android's own guidance and Apple's agree on
 * within a pixel or two, and sec 6.2 has cited it since the key row.
 */
const int BSSH_TOUCH_TARGET_DP = 48;

/*
 * A row in a LIST is taller than the floor (sec 8.5): the host list's
 * rows carry two lines -- a name and its user@host -- and a row you read
 * before tapping is not the same thing as a button you aim at.
 */
const int BSSH_TOUCH_ROW_DP = 64;

/*
 * Gives a control the floor where touch is the input, and leaves it alone
 * where it is not.
 *
 * A function rather than the constant at fifteen call sites, because the
 * call sites were `if (touch) widget->setMinimumHeight(48);` written out
 * every time -- and the one that forgot the `if` would silently make a
 * desktop form taller, while the one that forgot the line entirely made a
 * control nobody can hit. Null is accepted: several of these widgets exist
 * only on one platform and the callers already check.
 */
void bssh_apply_touch_height(QWidget *widget, bool touch);

/*
 * Gives the floor to everything on a form that a finger aims at.
 *
 * A walk rather than a list, because the list is what keeps going wrong:
 * the preset editor named fourteen controls by hand and left out the
 * clipboard checkbox, which stayed 23dp -- half the floor -- on the one
 * layout built for a thumb. The comment above that list already records
 * Import and Export having been left out of it before. A form that grows
 * a control must not also have to remember a second place.
 *
 * What a finger aims at: a button, a text field, a box you pick from, a
 * switch, a number you step. Labels are read rather than hit. Anything
 * INSIDE one of those is that control's own business -- a spin box holds
 * a line edit, and the thing being aimed at is the outer one.
 *
 * A LIST is walked too, and it has to be: its rows are tapped and they
 * are not widgets, so nothing that looks for child widgets can see them.
 * The licences list came out with 17dp rows on a phone for exactly that
 * reason -- every check passed, and the picture showed a column nobody
 * could hit. Rows get the floor through an item delegate rather than a
 * stylesheet, so it composes with the stylesheet a list may already have
 * (the host list draws its own separators with one), and a row that
 * carries its own size hint still wins -- sec 8.5's 64dp host rows are
 * taller than the floor on purpose.
 */
void bssh_apply_touch_heights(QWidget *root, bool touch);

#endif /* BSSH_TOUCH_METRICS_H */
