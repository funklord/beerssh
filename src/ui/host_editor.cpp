#include "ui/host_editor.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "model/settings_resolver.h"
#include "platform/platform.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"

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

	/*
	 * The same picker the desktop dialog embeds, sized for touch (sec
	 * 9.4: one list on both platforms). It is on the host page rather
	 * than behind a settings screen because on a phone this IS the
	 * settings screen -- there is no second window to put it in.
	 */
	m_font_picker = new bssh_font_picker(this);
	m_font_picker->set_touch_sized(true);
	outer->addWidget(m_font_picker);

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

	/*
	 * Hosts added here follow no preset yet, so what they resolve to is
	 * the app defaults with their own overrides on top (sec 10.1).
	 */
	QJsonObject resolved = bssh_settings_resolver::app_defaults();
	for (auto it = profile.overrides.constBegin(); it != profile.overrides.constEnd(); ++it) {
		resolved.insert(it.key(), it.value());
	}

	m_font_picker->set_font_choice(
	        resolved.value(QStringLiteral("font_family")).toString(),
	        resolved.value(QStringLiteral("font_size")).toInt());

	m_host->setFocus();
}

bssh_profile_t bssh_host_editor::profile() const {
	bssh_profile_t edited = m_profile;

	edited.host = m_host->text().trimmed();
	edited.user = m_user->text().trimmed();
	edited.port = static_cast<quint16>(m_port->value());
	edited.name = m_name->text().trimmed();

	/*
	 * The font, as an override, and only where it differs from the
	 * defaults -- an override equal to what it overrides is a badge that
	 * tells the user something untrue (sec 10.1).
	 */
	const QJsonObject defaults = bssh_settings_resolver::app_defaults();
	QJsonObject overrides = edited.overrides;

	const bssh_platform *platform = bssh_platform::instance();
	const QString current = bssh_font_catalog::instance().effective_id(
	        defaults.value(QStringLiteral("font_family")).toString(),
	        platform && platform->is_touch_primary());

	if (m_font_picker->font_id() != current) {
		overrides.insert(QStringLiteral("font_family"), m_font_picker->font_id());
	}
	if (m_font_picker->point_size() !=
	    defaults.value(QStringLiteral("font_size")).toInt()) {
		overrides.insert(QStringLiteral("font_size"), m_font_picker->point_size());
	}
	edited.overrides = overrides;

	/* An unnamed host is named after itself, so the list never shows a
	 * blank row. */
	if (edited.name.isEmpty()) edited.name = edited.host;

	return edited;
}
