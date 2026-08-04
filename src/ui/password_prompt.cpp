#include "ui/password_prompt.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

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
	QLabel *prompt = new QLabel(QStringLiteral("Password for %1").arg(target), this);
	prompt->setWordWrap(true);
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

	QDialogButtonBox *buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	outer->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	m_field->setFocus();
}

QString bssh_password_prompt::password() const {
	return m_field->text();
}

bool bssh_password_prompt::remember() const {
	return m_remember->isEnabled() && m_remember->isChecked();
}
