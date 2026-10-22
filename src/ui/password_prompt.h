#ifndef BSSH_PASSWORD_PROMPT_H
#define BSSH_PASSWORD_PROMPT_H

#include <QDialog>

#include "softkeys/focus_target.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QShowEvent;
class sk_keyboard;
class sk_keyboard_switch;

/*
 * The password prompt (project.md sec 11).
 *
 * Its own class rather than a dialog assembled inline, for one reason:
 * it was assembled inline, and it was too wide for a phone. The whole
 * string "Password for nabbe@10.0.2.2:2299:" sat in a form layout's LABEL
 * column, so the dialog's width grew with the length of the destination --
 * and on a 360dp screen the checkbox and the buttons ran off the right-hand
 * edge. A password prompt whose OK button is off-screen is not a cosmetic
 * problem, and nothing could have caught it: there was nothing to
 * construct.
 *
 * So the destination goes on its own wrapped line and the field below it,
 * and a test can now ask this to fit.
 */
class bssh_password_prompt : public QDialog {
	Q_OBJECT

public:
	/*
	 * allow_remember is false for a host that is not saved: the secret is
	 * keyed by profile id, so there is nowhere to hang it, and offering a
	 * checkbox that cannot work is worse than not offering one.
	 */
	bssh_password_prompt(const QString &target, bool allow_remember,
	                      QWidget *parent = nullptr);
	~bssh_password_prompt() override;

	QString password() const;
	bool remember() const;

	/* A line above the field saying why it is asked again; empty hides it. */
	void set_note(const QString &note);

	/*
	 * Softkeys instead of the system keyboard (sec 6.2), on a touch
	 * screen. A dialog is a window of its own, so the main window's
	 * forms keyboard cannot reach it: it carries its own, and the same
	 * switch over the system keyboard. The window passes its choice in
	 * and reads it back, so a switch made here holds for the forms too.
	 */
	void use_softkeys(bool use);
	bool uses_softkeys() const { return m_softkeys; }

protected:
	void showEvent(QShowEvent *event) override;
	bool event(QEvent *event) override;
	void done(int result) override;

private:
	void update_keyboard();
	void fit_to_screen();

	QLineEdit *m_field;
	QCheckBox *m_remember;
	QLabel *m_note;
	sk_keyboard *m_keyboard = nullptr;
	sk_keyboard_switch *m_switch = nullptr;
	sk_focus_target m_target;
	bool m_softkeys = false;
};

#endif /* BSSH_PASSWORD_PROMPT_H */
