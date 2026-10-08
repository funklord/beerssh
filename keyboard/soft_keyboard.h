#ifndef BSSH_SOFT_KEYBOARD_H
#define BSSH_SOFT_KEYBOARD_H

#include <QList>
#include <QString>
#include <QWidget>

#include "keyboard/key_row_layout.h"

class bssh_key_cap;
#include "keyboard/soft_keyboard_layout.h"

class QAbstractButton;
class QGridLayout;
class bssh_input_router;
class bssh_touch_chords;
class QHBoxLayout;
class bssh_resize_grip;

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

	/*
	 * Draw the keys again, because the CATALOG changed underneath.
	 *
	 * set_style() and set_group() cover a change of mind about which
	 * keys to show; this covers the keys themselves being different --
	 * a new keyboard layout. The widget holds nothing but what it read
	 * from the catalog, so rebuilding is the whole of the update.
	 */
	void rebuild_keys() { rebuild(); }

	bssh_soft_keyboard_catalog::style_t style() const { return m_style; }

	/*
	 * The split keyboard's halves (sec 6.2), 0 for the left and 1 for the
	 * right. They live in this widget until somebody places them: the
	 * main window puts them either side of the terminal, where they take
	 * width instead of height, and this widget is then empty while split.
	 * Shown only while split and while this widget is not hidden, so the
	 * window's one show/hide of the keyboard still governs both.
	 */
	QWidget *half(int side) const { return m_halves[side ? 1 : 0]; }

	/* How wide each half's keys are, in dp; the window decides. */
	void set_half_width(int width_dp);

	/*
	 * How tall the keys are when the keyboard sits under the terminal, in
	 * dp; 0 for their natural height. Below sec 6.2's 48dp floor only
	 * because somebody dragged it there, and never below a 28dp key.
	 */
	void set_bottom_height(int height_dp);

	/* Whether the resize bars show (sec 6.2); a setting decides. */
	void set_grips_shown(bool shown);

	/* The smallest a key gets by dragging, in dp. */
	static const int SMALLEST_KEY_DP = 28;

signals:
	/*
	 * A resize bar was let go: `size_dp` is a split half's width for
	 * STYLE_SPLIT, and the keys' height for the others. The window keeps
	 * it; this widget keeps nothing between runs.
	 */
	void size_chosen(bssh_soft_keyboard_catalog::style_t style, int size_dp);

	/*
	 * The configuration key: show the settings the user would otherwise
	 * reach through the front door and a menu, which from a live session
	 * on a phone is several screens away.
	 */
	void settings_requested();

	/*
	 * The hide key. The keyboard does not hide ITSELF: the window owns
	 * whether it is shown, has to remember that the user asked rather
	 * than the layout deciding, and is the only thing that can bring it
	 * back when the terminal is tapped. A widget that hid itself would
	 * leave nobody holding that state.
	 */
	void hide_requested();

protected:
	void changeEvent(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void rebuild();
	void refresh_side_icons();
	void refresh_script_button();
	bssh_key_cap *make_button(const QString &label, QWidget *parent);
	bool altgr_armed() const;
	void add_key(const bssh_key_t &key, QGridLayout *grid, int row, int column);
	void lay_out_split(const bssh_soft_key_group_t &group);
	void update_halves();
	void apply_bottom_height();
	int row_count() const;
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
	bssh_touch_chords *m_touch = nullptr;

	QWidget *m_keys;
	QGridLayout *m_grid;

	QHBoxLayout *m_outer = nullptr;
	bssh_resize_grip *m_top_grip = nullptr;
	bssh_resize_grip *m_half_grips[2] = { nullptr, nullptr };
	bool m_grips_shown = true;
	int m_half_width = 0;
	int m_bottom_height = 0;
	int m_drag_from = 0;
	QWidget *m_side = nullptr;
	QWidget *m_halves[2] = { nullptr, nullptr };
	QWidget *m_half_keys[2] = { nullptr, nullptr };
	QGridLayout *m_half_grids[2] = { nullptr, nullptr };
	QAbstractButton *m_group_button;

	/*
	 * In the SIDE COLUMN rather than on a page, and that is the whole
	 * decision (sec 6.2).
	 *
	 * Hide has to be reachable from wherever you are: a key that lives on
	 * the letters page cannot be reached from the numpad, so a user who
	 * cycled over to type a digit would have to cycle back to put the
	 * keyboard away. The side column is the only part of this widget that
	 * does not change when the page button is pressed.
	 *
	 * It also leaves the row arithmetic alone. Both keyboards have rows
	 * that must sum exactly to their own width, and the full-size one is
	 * written in quarter keys where a wrong span puts every key after it
	 * in the wrong place -- so adding two keys to a row is the expensive
	 * way to do this and buys nothing.
	 */
	QAbstractButton *m_settings_button;
	QAbstractButton *m_hide_button;
	QAbstractButton *m_script_button;

	int m_group;
	bssh_soft_keyboard_catalog::style_t m_style;

	QList<QAbstractButton *> m_modifier_buttons;
	QList<quint8> m_modifier_values;

	/* Buttons whose label follows Shift; see refresh_letter_labels. */
	/*
	 * The keys that CHANGE under Shift, and what each becomes. Parallel
	 * lists rather than a struct because they are filled and read in one
	 * place each; the second legend is zero for a letter, which takes its
	 * capital instead (bssh_shifted_char).
	 */
	QList<bssh_key_cap *> m_letter_buttons;
	QList<bssh_key_t> m_letter_keys;
};

#endif /* BSSH_SOFT_KEYBOARD_H */
