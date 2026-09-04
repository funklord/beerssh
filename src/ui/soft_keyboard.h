#ifndef BSSH_SOFT_KEYBOARD_H
#define BSSH_SOFT_KEYBOARD_H

#include <QList>
#include <QString>
#include <QWidget>

#include "ui/key_row_layout.h"
#include "ui/soft_keyboard_layout.h"

class QAbstractButton;
class QGridLayout;
class bssh_input_router;

/*
 * bssh_soft_keyboard -- the keyboard this app draws itself (project.md
 * sec 6.2).
 *
 * Sec 6.2 planned a key ROW: a strip of Ctrl, Esc, Tab and arrows above
 * whatever keyboard Android supplies, because Android's own keyboards
 * expose none of those. That answers the right question on a device where
 * a system keyboard appears. Where none does, the row is a strip of
 * modifiers with nothing to modify, and the terminal cannot be typed into
 * at all.
 *
 * So this is the other half rather than a replacement, and which one is
 * shown is a device-level setting (sec 10.6). The row still exists,
 * unchanged, for people whose system keyboard works and who want its
 * prediction, its languages and its layout.
 *
 * Every key goes through bssh_input_router, exactly as the row's do: one
 * translation path for physical keys, both on-screen keyboards and paste
 * (sec 6.3), or they drift apart.
 */
class bssh_soft_keyboard : public QWidget {
	Q_OBJECT

public:
	explicit bssh_soft_keyboard(bssh_input_router *router, QWidget *parent = nullptr);

	/* Retargeted when the shown session changes (sec 3, principle 2). */
	void set_router(bssh_input_router *router);

	/*
	 * Which group of keys is on screen. The side button walks these; the
	 * setter exists for restoring one and for tests.
	 */
	void set_group(int index);

	/*
	 * Which keyboard this is: the compact thumb one or the full-size
	 * board (sec 6.2). Rebuilds, because the two do not share a column
	 * grid -- and resets to the first group, since group indices are per
	 * style and a "terminal" index in a two-group catalog is off the end.
	 */
	void set_style(bssh_soft_keyboard_catalog::style_t style);
	int group() const { return m_group; }
	QString group_id() const;

	/* The side button, so a test can press what a thumb presses. */
	QAbstractButton *group_button() const { return m_group_button; }

private:
	void rebuild();
	QAbstractButton *make_button(const QString &label);
	void add_key(const bssh_key_t &key, int row, int column);
	void refresh_modifiers();

	/*
	 * A letter reads as its capital while Shift is on, because that is
	 * what it will send.
	 *
	 * Not decoration. A soft keyboard's Shift is a LAYOUT change rather
	 * than a modifier on the wire -- the character itself differs -- so a
	 * key still reading "q" while about to send "Q" is the one thing on
	 * screen that could have said so, saying the wrong thing.
	 */
	void refresh_letter_labels();

	bssh_input_router *m_router;

	QWidget *m_keys;
	QGridLayout *m_grid;
	QAbstractButton *m_group_button;

	int m_group;
	bssh_soft_keyboard_catalog::style_t m_style;

	QList<QAbstractButton *> m_modifier_buttons;
	QList<quint8> m_modifier_values;

	/* Buttons whose label follows Shift; see refresh_letter_labels. */
	QList<QAbstractButton *> m_letter_buttons;
	QList<char32_t> m_letter_values;
};

#endif /* BSSH_SOFT_KEYBOARD_H */
