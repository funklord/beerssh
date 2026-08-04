#ifndef BSSH_PASSWORD_PROMPT_H
#define BSSH_PASSWORD_PROMPT_H

#include <QDialog>

class QCheckBox;
class QLineEdit;

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

	QString password() const;
	bool remember() const;

private:
	QLineEdit *m_field;
	QCheckBox *m_remember;
};

#endif /* BSSH_PASSWORD_PROMPT_H */
