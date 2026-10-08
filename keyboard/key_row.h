#ifndef SK_KEY_ROW_H
#define SK_KEY_ROW_H

#include <QWidget>

#include <functional>

class QAbstractButton;
class QScrollArea;
class sk_target;
class sk_touch_chords;

/*
 * sk_key_row -- the strip of keys a soft keyboard does not have
 * (project.md sec 6.2).
 *
 * Android soft keyboards expose no Ctrl, Esc, Tab, arrows or function
 * keys, and a terminal without those is a terminal you cannot use. This
 * is the same answer ConnectBot and Termux reached, and users coming from
 * either will expect it.
 *
 * Every button goes through bssh_input_router, never straight to the
 * session: one translation path for physical keys, the key row and paste
 * (sec 6.3), or the three drift apart.
 *
 * Visibility is a capability question, not an OS question -- see
 * visibility_t. The row is BUILT on every platform, deliberately, both
 * because a convertible laptop in tablet mode wants it and because a
 * widget compiled only on Android is a widget only tested on Android.
 */
class sk_key_row : public QWidget {
	Q_OBJECT

public:
	/*
	 * AUTO follows the platform's primary input device; the other two
	 * are the user overriding it. A plain on/off would get both the
	 * convertible-in-tablet-mode case and the phone-with-a-Bluetooth-
	 * keyboard case wrong, in opposite directions (sec 6.2).
	 */
	enum visibility_t {
		AUTO,
		ALWAYS,
		NEVER
	};

	explicit sk_key_row(sk_target *target, QWidget *parent = nullptr);

	/* Retargeted when the shown session changes (sec 3, principle 2). */
	void set_target(sk_target *target);

	void set_visibility_mode(visibility_t mode);

	/*
	 * Which keys the row holds (sec 6.2, sec 10.2). Empty selects the
	 * default layout; the value comes from the host's preset, so a
	 * network switch and a build server get different rows.
	 */
	void set_layout(const QString &id);
	QString layout_id() const { return m_layout_id; }
	visibility_t visibility_mode() const { return m_mode; }

	/* Whether the row should be shown, given the mode and the platform. */
	bool should_be_visible() const;

	/*
	 * How AUTO asks whether this is a touch device. The application knows
	 * better than any guess here -- BeerSSH has its own platform answer,
	 * forced in its tests -- so it may say; without one, any touchscreen Qt
	 * reports counts.
	 */
	void set_touch_primary(std::function<bool()> query) { m_touch_primary = std::move(query); }

private:
	void add_key(const QString &label, int key);
	void add_modifier(const QString &label, quint8 modifier);
	void add_text(const QString &label, char32_t ch);
	void add_chord(const QString &label, char32_t ch, quint8 modifier);

	/* `gap`: holds a key's width and draws nothing (sec 6.2). */
	void add_spacer();

	/* Fn: swaps the row for F1..F12 and back (sec 6.2). */
	void add_function_toggle(const QString &label);
	QAbstractButton *make_button(const QString &label);
	void refresh_modifier_buttons();

	sk_target *m_target;
	std::function<bool()> m_touch_primary;
	sk_touch_chords *m_touch = nullptr;
	visibility_t m_mode;
	/*
	 * The keys scroll horizontally: sec 6.2's own key list at sec 6.2's own
	 * 48dp floor is nearly twice a phone's width. See the constructor.
	 */
	QScrollArea *m_scroll;
	QWidget *m_keys;

	QString m_layout_id;

	/* Whether the row is currently showing F1..F12 rather than its keys. */
	bool m_function_mode;

	/* Every button in the row, so a layout change can clear it. */
	QList<QAbstractButton *> m_buttons;
	QList<QAbstractButton *> m_modifier_buttons;
	QList<quint8> m_modifier_values;
};

#endif /* SK_KEY_ROW_H */
