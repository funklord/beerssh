#ifndef SK_KEY_CAP_H
#define SK_KEY_CAP_H

#include <QFont>
#include <QString>
#include <QToolButton>

/*
 * A keycap that can carry a second legend (project.md sec 6.2).
 *
 * A printed keycap shows what the key sends and, smaller and off to one
 * side, what it sends with AltGr. The soft keyboard needs the same thing
 * for the same reason: US International is unfamiliar to most people, and
 * a third level nobody can see is a feature nobody finds.
 *
 * Qt has no way to do this with a plain button. QToolButton renders its
 * text in one font and one colour, and neither it nor QAbstractButton
 * renders markup -- so two legends of different weight in one cap is a
 * paint, not a string. That is the whole reason this class exists.
 *
 * The rule it draws to: EXACTLY ONE legend is prominent at any moment,
 * and it is always the character the key will actually send. The hint is
 * subordinate -- smaller, dimmer, in the corner a keycap puts it -- and
 * the keyboard drops it once AltGr is armed, because at that point the
 * prominent character has become the accented one and a hint repeating
 * what the user has already done is noise on a 36dp key.
 *
 * The alternative considered was showing both legends always and moving
 * the emphasis between them. It reads better on paper and worse on a
 * phone: hunting for a character means scanning forty caps, and forty
 * caps showing two glyphs each is twice the reading at the one moment
 * the user is already looking for something.
 */
/*
 * Auto-repeat, for a key that sends something when it is held.
 *
 * A physical keyboard repeats every key but its modifiers, and a terminal
 * is where that matters most: backspacing over a mistyped path, or moving
 * the cursor along a line, is dozens of presses without it. On a phone,
 * where each press is a separate deliberate tap, that is the difference
 * between usable and not -- reported from the phone, where holding
 * Backspace deleted exactly one character.
 *
 * The rule this applies: EVERY key that sends something repeats, and
 * nothing else does. Modifiers are excluded because they are checkable
 * and a repeat would cycle armed-locked-off under a stationary thumb;
 * the page, settings, hide and script buttons are excluded because they
 * are about the keyboard rather than about the terminal.
 *
 * The alternative considered was repeating only the navigation keys, the
 * way a phone's own keyboard does -- Gboard repeats Backspace and the
 * arrows and never a letter. That is the right rule for prose, where an
 * accidental "aa" is a typo somebody has to spot. It is the wrong one
 * here: the thing being typed into is a shell, the user came from a
 * physical keyboard, and a rule that repeats some keys and not others is
 * one nobody can predict. The delay is what stops an ordinary tap
 * repeating, and it is set long enough to do that rather than tuned for
 * speed.
 *
 * A free function rather than a method, because the key row builds plain
 * QToolButtons and this board builds sk_key_caps: one rule, one place,
 * and the two boards cannot drift apart about what repeats.
 */
void sk_set_key_repeat(QAbstractButton *button);

class sk_key_cap : public QToolButton {
	/*
	 * Q_OBJECT for the metaobject rather than for signals: this class
	 * declares none. findChildren<sk_key_cap *> is what needs it --
	 * a test asking for the keycaps, and not for every QToolButton on a
	 * keyboard that also has three side buttons.
	 */
	Q_OBJECT

public:
	explicit sk_key_cap(QWidget *parent = nullptr);

	/*
	 * The subordinate legend, or empty for none.
	 *
	 * Separate from setText() rather than folded into it, so that what
	 * the button reports to a screen reader stays the character it
	 * sends. A cap reading "q a" to a braille display would be wrong
	 * twice over -- it is one key, and it sends one thing.
	 */
	void set_hint(const QString &hint);

	const QString &hint() const { return m_hint; }

	/*
	 * Whether this cap has to draw its own text rather than let the
	 * style do it.
	 *
	 * A second legend is the obvious reason. The other one is an
	 * AMPERSAND: Qt reads & in a button's text as a mnemonic marker and
	 * eats it, so the & key on the compact symbol page drew a blank cap
	 * -- it sent the right character, it was the right size, in the
	 * right cell, and it showed nothing at all. QPainter::drawText does
	 * no such interpretation, so taking the custom path fixes it.
	 *
	 * Static and public so it can be tested. The alternative fixes were
	 * worse: escaping to "&&" makes text() lie, which is what a screen
	 * reader reads, and drawing EVERY cap here would lose the style's
	 * eliding on the full board's narrow keys, where Home and PgUp
	 * already need it.
	 */
	static bool needs_own_paint(const QString &text, const QString &hint);

	/*
	 * The largest font at or below `base` that fits `text` into `width`,
	 * down to a floor.
	 *
	 * A keycap SHRINKS rather than elides. The style's answer to a label
	 * too wide for its key is an ellipsis, which on a keyboard is the
	 * worst of both: the key still takes the room and no longer says
	 * what it does. `Hom` drew as three dots when the compact grid went
	 * to eleven keys a row, and `Home`, `PgUp` and `F10` to `F12` have
	 * done the same on the full board at phone width since it was
	 * written.
	 *
	 * Static and public so the floor can be tested rather than eyeballed
	 * in a render: below it the label would be unreadable, so it elides
	 * after all -- an ellipsis is bad and four-point type is worse.
	 */
	static QFont fitted_font(const QFont &base, const QString &text, int width);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	void paint_modifier(int state);

	QString m_hint;
};

#endif /* SK_KEY_CAP_H */
