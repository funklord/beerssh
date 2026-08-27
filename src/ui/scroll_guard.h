#ifndef BSSH_SCROLL_GUARD_H
#define BSSH_SCROLL_GUARD_H

class QWidget;

/*
 * Stops the controls on a scrolling form from eating the wheel.
 *
 * A QComboBox, a spin box and a slider all handle a wheel notch by
 * CHANGING THEIR VALUE, and they do it whether or not they have focus.
 * Put one on a page inside a QScrollArea and the wheel means two
 * different things depending on which pixel the pointer happens to be
 * over -- and the page does not move, so the natural reaction is to keep
 * wheeling, which walks the control further.
 *
 * Reported from use, and the report is what this exists for: "suddenly
 * the text became dark when looking through config menus". Scrolling the
 * device settings with the pointer over the Appearance row moved it from
 * "Follow the system" to "Light" and then to "Dark" -- one notch each,
 * applied immediately and written to app.ini, so it survived a restart.
 * The text-brightness slider one row down is the same accident with a
 * different victim. Neither is recoverable by scrolling back, because
 * scrolling back is what did it.
 *
 * The wheel is therefore given to the enclosing scroll area, always,
 * for every such control the page contains. Not "unless it has focus":
 * that variant leaves exactly the same accident available one click
 * later, and a user who has just picked a value from a combo is the
 * user most likely to wheel next.
 *
 * Nothing is taken away. These controls are still set by clicking, by
 * dragging, and from the keyboard once focused -- which is every route
 * a keyboard-only or a touch user had before.
 *
 * Call it AFTER the page is populated, on the widget that owns the
 * form. Controls with no QAbstractScrollArea above them are left alone:
 * a combo on a dialog that does not scroll has no page to scroll and
 * the wheel is unambiguous there.
 */
void bssh_dont_steal_the_wheel(QWidget *root);

#endif /* BSSH_SCROLL_GUARD_H */
