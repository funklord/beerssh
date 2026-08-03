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
#include "ui/override_badge.h"

#include "model/profile_store.h"

#include "term/font_catalog.h"

/*
 * Touch target sizes. The same 48dp floor the key row uses (sec 6.2),
 * applied to text fields and buttons: a field a thumb cannot reliably
 * land in is a field that gets tapped twice.
 */
static const int BSSH_FIELD_HEIGHT_DP = 48;
static const int BSSH_BUTTON_HEIGHT_DP = 56;

bssh_host_editor::bssh_host_editor(bssh_profile_store *store, QWidget *parent)
    : QWidget(parent),
      m_store(store),
      m_title(nullptr),
      m_font_scope(nullptr),
      m_font_revert(nullptr),
      m_save(nullptr) {
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(16, 16, 16, 16);
	outer->setSpacing(12);

	m_title = new QLabel(QStringLiteral("Add host"), this);
	QFont title_font = bssh_scaled_font(m_title->font(), 1.4);
	title_font.setBold(true);
	m_title->setFont(title_font);
	outer->addWidget(m_title);

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
	 * The key this host should be offered, which is what an imported
	 * OpenSSH IdentityFile lands in (sec 10.5). Blank means the default
	 * identities, which is what almost everyone wants -- so the
	 * placeholder says that rather than leaving an empty box looking like
	 * something unfinished.
	 *
	 * A path, typed, and no Browse button -- unlike the desktop dialog,
	 * where the key really is a file on the machine the dialog is running
	 * on. There is no way to GET a key onto a phone through this app yet:
	 * nothing imports one, and sec 11's key story stops at the default
	 * identities and the agent. So on touch this field is mostly for
	 * reading and correcting what an OpenSSH import (sec 10.5) put there,
	 * and a file picker would promise a workflow that does not exist.
	 */
	m_identity = new QLineEdit(this);
	m_identity->setPlaceholderText(QStringLiteral("default keys"));
	m_identity->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);

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
	form->addRow(QStringLiteral("Key file"), m_identity);
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

	/*
	 * sec 10.1's badge and revert, the same pair the desktop Window page
	 * carries. Without it "this host looks different from the others" and
	 * "this host overrides its preset" are the same observation, and the
	 * only way back to following the preset is deleting the host and
	 * adding it again.
	 */
	m_font_scope = new QLabel(this);
	m_font_scope->setWordWrap(true);
	outer->addWidget(m_font_scope);

	m_font_revert = new QPushButton(QStringLiteral("Use the preset's font"), this);
	m_font_revert->setMinimumHeight(BSSH_FIELD_HEIGHT_DP);
	outer->addWidget(m_font_revert);
	connect(m_font_revert, &QPushButton::clicked,
	         this, &bssh_host_editor::revert_font_to_preset);

	outer->addStretch(1);

	m_save = new QPushButton(QStringLiteral("Add host"), this);
	QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"), this);
	m_save->setMinimumHeight(BSSH_BUTTON_HEIGHT_DP);
	cancel->setMinimumHeight(BSSH_BUTTON_HEIGHT_DP);
	m_save->setDefault(true);

	/*
	 * Full-width and stacked rather than a right-aligned row: a phone is
	 * held in one hand, and the reachable part of the screen is the
	 * bottom, not its right-hand corner.
	 */
	outer->addWidget(m_save);
	outer->addWidget(cancel);

	connect(m_save, &QPushButton::clicked, this, [this] {
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
	m_identity->setText(profile.identity_file);

	const QJsonObject resolved = resolved_settings();

	m_font_picker->set_font_choice(
	        resolved.value(QStringLiteral("font_family")).toString(),
	        resolved.value(QStringLiteral("font_size")).toInt());
	m_font_picker->set_scheme(resolved.value(QStringLiteral("colour_scheme")).toString());

	update_override_badge();
	m_host->setFocus();
}

/*
 * All three layers (sec 10.1), which is what this page used to skip.
 *
 * It resolved against the app defaults alone -- correct back when this
 * page could only ADD a host, and wrong the moment it could open one that
 * follows a preset: the picker then showed a font the host does not
 * render in, and the obvious way to "fix" that is to choose the font you
 * can see, which writes an override pinning it and quietly stops the host
 * following its preset at all.
 */
QJsonObject bssh_host_editor::resolved_settings() const {
	if (!m_store || m_profile.preset_id == 0) {
		QJsonObject resolved = bssh_settings_resolver::app_defaults();
		for (auto it = m_profile.overrides.constBegin();
		     it != m_profile.overrides.constEnd(); ++it) {
			resolved.insert(it.key(), it.value());
		}
		return resolved;
	}

	return bssh_settings_resolver::resolve(m_store->preset(m_profile.preset_id),
	                                        m_profile.overrides);
}

void bssh_host_editor::set_editing(bool editing) {
	if (m_title) m_title->setText(editing ? QStringLiteral("Edit host")
	                                      : QStringLiteral("Add host"));
	if (m_save) m_save->setText(editing ? QStringLiteral("Save")
	                                    : QStringLiteral("Add host"));
}

void bssh_host_editor::update_override_badge() {
	if (!m_font_scope || !m_font_revert) return;

	/*
	 * Only meaningful once a host exists and follows something. While
	 * ADDING one there is nothing to be following or overriding yet, so
	 * saying either would be noise and a revert button would have nothing
	 * to revert to.
	 */
	if (!m_store || m_profile.id == 0) {
		m_font_scope->setVisible(false);
		m_font_revert->setVisible(false);
		return;
	}

	const bssh_override_badge_t badge = bssh_font_override_badge(
	        m_store, m_profile, m_font_picker->font_id(), m_font_picker->point_size());

	m_font_scope->setText(badge.text);
	m_font_scope->setVisible(true);
	m_font_revert->setVisible(badge.overridden);
}

void bssh_host_editor::revert_font_to_preset() {
	m_profile.overrides =
	        bssh_settings_resolver::revert_field(m_profile.overrides,
	                                              QStringLiteral("font_family"));
	m_profile.overrides =
	        bssh_settings_resolver::revert_field(m_profile.overrides,
	                                              QStringLiteral("font_size"));

	const QJsonObject resolved = resolved_settings();
	m_font_picker->set_font_choice(
	        resolved.value(QStringLiteral("font_family")).toString(),
	        resolved.value(QStringLiteral("font_size")).toInt());

	update_override_badge();
}

bssh_profile_t bssh_host_editor::profile() const {
	bssh_profile_t edited = m_profile;

	edited.host = m_host->text().trimmed();
	edited.user = m_user->text().trimmed();
	edited.port = static_cast<quint16>(m_port->value());
	edited.name = m_name->text().trimmed();
	edited.identity_file = m_identity->text().trimmed();

	/*
	 * Sparse appearance overrides, computed by the same function the
	 * desktop dialog uses so the two cannot drift (sec 10.1).
	 */
	edited.overrides = bssh_appearance_overrides(
	        m_store, edited, m_font_picker->font_id(),
	        m_font_picker->point_size(), m_font_picker->scheme_id());

	/* An unnamed host is named after itself, so the list never shows a
	 * blank row. */
	if (edited.name.isEmpty()) edited.name = edited.host;

	return edited;
}
