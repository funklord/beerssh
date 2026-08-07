#include "ui/touch_metrics.h"
#include "ui/password_prompt.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>

#include "ui/wrapped_note.h"
#include <QLineEdit>
#include <QVBoxLayout>

#include "platform/platform.h"

bssh_password_prompt::bssh_password_prompt(const QString &target, bool allow_remember,
                                            QWidget *parent)
    : QDialog(parent),
      m_field(new QLineEdit(this)),
      m_remember(new QCheckBox(QStringLiteral("Remember this password"), this)) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *outer = new QVBoxLayout(this);

	/*
	 * The destination on its own line, and allowed to wrap.
	 *
	 * It used to be the label half of a form row, which made the dialog's
	 * width a function of how long the user's host name is: "Password for
	 * nabbe@10.0.2.2:2299:" plus a field is wider than a phone, and the
	 * parts that ran off the edge were the checkbox and the buttons.
	 */
	QLabel *prompt = bssh_wrapped_note(QStringLiteral("Password for %1").arg(target), this);
	outer->addWidget(prompt);

	m_field->setEchoMode(QLineEdit::Password);

	/*
	 * Named for a screen reader, since the visible label above is a
	 * separate widget and nothing otherwise connects the two (sec 6.3).
	 */
	m_field->setAccessibleName(QStringLiteral("Password"));
	m_field->setAccessibleDescription(
	        QStringLiteral("Password for %1").arg(target));
	outer->addWidget(m_field);

	/*
	 * Unchecked by default: storing a credential is a decision, not a
	 * default.
	 */
	m_remember->setEnabled(allow_remember);
	outer->addWidget(m_remember);

	/*
	 * All the slack in ONE place, between the question and the buttons.
	 *
	 * There was none, and a vertical layout with nowhere to put spare
	 * height gives it to whichever widget will take it -- here the
	 * wrapped label, whose policy allows growth. On a phone-sized dialog
	 * that put roughly 200px of nothing above the label and 250px between
	 * the label and the field it belongs to, so a question and its answer
	 * box read as two unrelated things at opposite ends of the screen.
	 *
	 * Invisible on the desktop, where the dialog is only as tall as its
	 * contents and there is no slack to misplace. Found by rendering it
	 * at 360x640, which is the size it actually appears at on the
	 * platform that has to type into it.
	 */
	outer->addStretch(1);

	QDialogButtonBox *buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	outer->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	/*
	 * Sec 6.2's 48dp floor, which this dialog never had.
	 *
	 * Sec 11 reshaped it once already, for WIDTH: the destination sat in
	 * a form layout's label column and demanded 508px on a 360px screen,
	 * pushing OK off the edge. That was fixed and the heights were not
	 * looked at, so the field, the checkbox and both buttons stayed
	 * pointer-sized on the one dialog a user cannot avoid -- you cannot
	 * reach a password-authenticated host without it. A 20px checkbox is
	 * a target you miss, and the thing you miss it into is a password
	 * field.
	 *
	 * Gated on the input device rather than the OS, the same question the
	 * key row and the front door ask (sec 6.2, sec 8.5): a tablet with a
	 * keyboard and a laptop with a touchscreen are the two cases an OS
	 * check gets wrong in opposite directions.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	if (platform && platform->is_touch_primary()) {
		m_field->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		m_remember->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

		for (QAbstractButton *button : buttons->buttons()) {
			button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		}
	}

	m_field->setFocus();
}

QString bssh_password_prompt::password() const {
	return m_field->text();
}

bool bssh_password_prompt::remember() const {
	return m_remember->isEnabled() && m_remember->isChecked();
}
