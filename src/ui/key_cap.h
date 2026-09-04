#ifndef BSSH_KEY_CAP_H
#define BSSH_KEY_CAP_H

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
class bssh_key_cap : public QToolButton {
	/*
	 * Q_OBJECT for the metaobject rather than for signals: this class
	 * declares none. findChildren<bssh_key_cap *> is what needs it --
	 * a test asking for the keycaps, and not for every QToolButton on a
	 * keyboard that also has three side buttons.
	 */
	Q_OBJECT

public:
	explicit bssh_key_cap(QWidget *parent = nullptr);

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
	QString m_hint;
};

#endif /* BSSH_KEY_CAP_H */
