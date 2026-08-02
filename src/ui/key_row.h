#ifndef BSSH_KEY_ROW_H
#define BSSH_KEY_ROW_H

#include <QWidget>

class QAbstractButton;
class bssh_input_router;

/*
 * bssh_key_row -- the strip of keys a soft keyboard does not have
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
class bssh_key_row : public QWidget {
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

	explicit bssh_key_row(bssh_input_router *router, QWidget *parent = nullptr);

	void set_visibility_mode(visibility_t mode);
	visibility_t visibility_mode() const { return m_mode; }

	/* Whether the row should be shown, given the mode and the platform. */
	bool should_be_visible() const;

private:
	void add_key(const QString &label, int key);
	void add_modifier(const QString &label, quint8 modifier);
	void add_text(const QString &label, char32_t ch);
	QAbstractButton *make_button(const QString &label);
	void refresh_modifier_buttons();

	bssh_input_router *m_router;
	visibility_t m_mode;
	QList<QAbstractButton *> m_modifier_buttons;
	QList<quint8> m_modifier_values;
};

#endif /* BSSH_KEY_ROW_H */
