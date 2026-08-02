#include "ui/host_editor.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "term/font_catalog.h"

/*
 * Touch target sizes. The same 48dp floor the key row uses (sec 6.2),
 * applied to text fields and buttons: a field a thumb cannot reliably
 * land in is a field that gets tapped twice.
 */
static const int BSSH_FIELD_HEIGHT_DP = 48;
static const int BSSH_BUTTON_HEIGHT_DP = 56;

bssh_host_editor::bssh_host_editor(QWidget *parent)
    : QWidget(parent) {
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(16, 16, 16, 16);
	outer->setSpacing(12);

	QLabel *title = new QLabel(QStringLiteral("Add host"), this);
	QFont title_font = bssh_scaled_font(title->font(), 1.4);
	title_font.setBold(true);
	title->setFont(title_font);
	outer->addWidget(title);

	QFormLayout *form = new QFormLayout();
	form->setSpacing(12);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_host = new QLineEdit(this);
	m_host->setPlaceholderText(QStringLiteral("example.com or 10.0.0.42"));
	m_host->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);

	m_user = new QLineEdit(this);
	m_user->setPlaceholderText(QStringLiteral("root"));
	m_user->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);

	m_port = new QSpinBox(this);
	m_port->setRange(1, 65535);
	m_port->setValue(22);
	m_port->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);

	m_name = new QLineEdit(this);
	m_name->setPlaceholderText(QStringLiteral("optional"));
	m_name->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);

	/*
	 * Host first, because it is the only field that must be filled in.
	 * The name is last and optional: making someone name a thing before
	 * they can connect to it is a form asking for tidiness rather than
	 * for what it needs.
	 */
	form->addRow(QStringLiteral("Host"), m_host);
	form->addRow(QStringLiteral("User"), m_user);
	form->addRow(QStringLiteral("Port"), m_port);
	form->addRow(QStringLiteral("Name"), m_name);
	outer->addLayout(form);

	outer->addStretch(1);

	QPushButton *save = new QPushButton(QStringLiteral("Save"), this);
	QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"), this);
	save->setMinimumHeight(BSSH_BUTTON_HEIGHT_DP);
	cancel->setMinimumHeight(BSSH_BUTTON_HEIGHT_DP);
	save->setDefault(true);

	/*
	 * Full-width and stacked rather than a right-aligned row: a phone is
	 * held in one hand, and the reachable part of the screen is the
	 * bottom, not its right-hand corner.
	 */
	outer->addWidget(save);
	outer->addWidget(cancel);

	connect(save, &QPushButton::clicked, this, [this] {
		const bssh_profile_t edited = profile();
		if (!edited.is_valid()) {
			/* Nothing to save, and saying so beats saving an empty row. */
			m_host->setFocus();
			return;
		}
		emit save_requested(edited);
	});
	connect(cancel, &QPushButton::clicked, this, &bssh_host_editor::cancelled);
}

void bssh_host_editor::set_profile(const bssh_profile_t &profile) {
	m_profile = profile;

	m_host->setText(profile.host);
	m_user->setText(profile.user);
	m_port->setValue(profile.port);
	m_name->setText(profile.name);

	m_host->setFocus();
}

bssh_profile_t bssh_host_editor::profile() const {
	bssh_profile_t edited = m_profile;

	edited.host = m_host->text().trimmed();
	edited.user = m_user->text().trimmed();
	edited.port = static_cast<quint16>(m_port->value());
	edited.name = m_name->text().trimmed();

	/* An unnamed host is named after itself, so the list never shows a
	 * blank row. */
	if (edited.name.isEmpty()) edited.name = edited.host;

	return edited;
}
