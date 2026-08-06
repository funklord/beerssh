#ifndef BSSH_WRAPPED_NOTE_H
#define BSSH_WRAPPED_NOTE_H

#include <QString>

class QLabel;
class QWidget;

/*
 * An explanatory note that survives being put in a form (project.md
 * sec 8.3).
 *
 * A QLabel with setWordWrap(true) knows its own height only once it knows
 * its width -- that is what heightForWidth() is for -- but its size policy
 * does not advertise the dependency, so a layout asking "how tall are
 * you?" is answered with ONE LINE. QFormLayout believes it, hands over one
 * line's worth of room, and the rest of the sentence is drawn on top of
 * the next row.
 *
 * It is not hypothetical and it is not subtle once rendered: on the device
 * settings page the key-row note stopped at "Override it if you have a
 * keyboard" with "Appearance" printed through its remaining two lines, and
 * the stored-passwords note lost its last line the same way. Both had
 * setWordWrap(true) and both looked correct in the source.
 *
 * **Nothing caught it, and the reason is worth keeping.** The touch-surface
 * check asks whether a page's MINIMUM size fits a phone, and a label that
 * under-reports its own minimum is invisible to that question -- the page
 * passed while the text on it was unreadable. It was found by rendering
 * the screen and looking at it, which is what `make shots` is for.
 *
 * So the fix belongs in one function rather than at fourteen call sites:
 * a note built anywhere gets the policy, and a note added later cannot
 * quietly miss it.
 */
QLabel *bssh_wrapped_note(const QString &text, QWidget *parent);

/*
 * The same treatment for a label somebody else built -- the ones held as
 * members, whose text arrives after construction. Same rule, same reason;
 * this is what bssh_wrapped_note() is written in terms of, so the two can
 * never drift into disagreeing about what a note is.
 */
void bssh_wrap_note(QLabel *note);

#endif /* BSSH_WRAPPED_NOTE_H */
