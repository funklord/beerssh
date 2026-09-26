#include "ui/touch_metrics.h"
#include "ui/host_editor.h"

#include <QCheckBox>
#include <QComboBox>

#include <QFormLayout>
#include <QLabel>

#include "ui/scroll_guard.h"
#include "ui/wrapped_note.h"
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QScroller>
#include <QVBoxLayout>

#include "model/settings_resolver.h"
#include "platform/platform.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"
#include "ui/form_wrap.h"
#include "ui/override_badge.h"

#include "model/profile_store.h"

#include "term/font_catalog.h"

static const int BSSH_BUTTON_HEIGHT_DP = 56;

bssh_host_editor::bssh_host_editor(bssh_profile_store *store, QWidget *parent)
    : QWidget(parent),
      m_store(store),
      m_title(nullptr),
      m_font_scope(nullptr),
      m_font_revert(nullptr),
      m_save(nullptr),
      m_name(nullptr),
      m_identity(nullptr),
      m_jump(nullptr),
      m_biometric(nullptr),
      m_host(nullptr),
      m_user(nullptr),
      m_port(nullptr),
      m_font_picker(nullptr) {
	/*
	 * The fields scroll; the actions do not.
	 *
	 * This form needs 821px of height, and a phone has 640. Without
	 * anywhere to scroll, Qt squeezes the rows past their minimums until
	 * they overlap -- which is what a device showed: "Name" drawn on top
	 * of "TERM", a spin box across a checkbox. The buttons stay pinned to
	 * the bottom rather than becoming something to scroll down to, for
	 * the same reason they are stacked full-width: a phone is held in one
	 * hand and the bottom is what the thumb reaches.
	 */
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	QWidget *content = new QWidget(this);
	QVBoxLayout *body = new QVBoxLayout(content);
	body->setContentsMargins(16, 16, 16, 16);
	body->setSpacing(12);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	/* Sideways scrolling would mean something above is too wide, which is
	 * a bug rather than something to let the user work around. */
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
	outer->addWidget(scroll, 1);

	m_title = new QLabel(QStringLiteral("Add host"), this);
	QFont title_font = bssh_scaled_font(m_title->font(), 1.4);
	title_font.setBold(true);
	m_title->setFont(title_font);
	body->addWidget(m_title);

	QFormLayout *form = new QFormLayout();
	form->setSpacing(12);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_host = new QLineEdit(this);
	m_host->setPlaceholderText(QStringLiteral("example.com or 10.0.0.42"));
	m_host->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

	m_user = new QLineEdit(this);
	m_user->setPlaceholderText(QStringLiteral("root"));
	m_user->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

	m_port = new QSpinBox(this);
	m_port->setRange(1, 65535);
	m_port->setValue(22);
	m_port->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

	m_name = new QLineEdit(this);
	m_name->setPlaceholderText(QStringLiteral("optional"));
	m_name->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

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
	m_identity->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

	/*
	 * The bastion, if this host sits behind one (sec 10.5). Blank is a
	 * direct connection, which is what nearly every host is, so the
	 * placeholder says so rather than leaving a box that looks like a
	 * missing answer.
	 *
	 * Spelled the way OpenSSH spells it, because that is what the person
	 * filling this in has already typed after -J or pasted out of a
	 * config file, and because an import (sec 10.5) puts that exact
	 * string here.
	 */
	m_jump = new QLineEdit(this);
	m_jump->setPlaceholderText(QStringLiteral("direct"));
	m_jump->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

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

	/*
	 * The folder it is listed under (sec 10.5), after the name because it
	 * is the same kind of thing: how the host is found, not how it
	 * connects. Editable, offering the folders that exist.
	 */
	m_folder = new QComboBox(this);
	m_folder->setEditable(true);
	m_folder->setObjectName(QStringLiteral("host_folder"));
	m_folder->lineEdit()->setPlaceholderText(QStringLiteral("none"));
	m_folder->setAccessibleName(QStringLiteral("Folder"));
	m_folder->lineEdit()->setAccessibleName(QStringLiteral("Folder"));
	m_folder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	form->addRow(QStringLiteral("Folder"), m_folder);
	form->addRow(QStringLiteral("Key file"), m_identity);
	form->addRow(QStringLiteral("Via"), m_jump);
	body->addLayout(form);

	/*
	 * Where the room is short the label goes ABOVE its field rather than
	 * beside it, and ui/form_wrap.h holds why and at what width.
	 *
	 * The measurement this page supplied: two columns at 320dp took 88 of
	 * the 306 the scroll area leaves for the label column, and the sample
	 * host's "nabbe@bastion.internal.example.com:2222" arrived in 218px
	 * of the 245 its own text needs.
	 *
	 * WrapAllRows rather than the settings page's WrapLongRows, and the
	 * difference is the same distinction: WrapLongRows wraps a row whose
	 * MINIMUM will not fit, and a field willing to scroll its own text
	 * has a small minimum, so it would never fire here. That page's
	 * fields hold chords from a fixed table and can state what they
	 * need; these cannot.
	 *
	 * Watched rather than set once, because the width changes under the
	 * page: a foldable unfolds, a desktop window is dragged.
	 */
	bssh_watch_form_width(form, this);

	/*
	 * sec 11's per-host biometric release, and it is on the page only
	 * where the device can actually raise a prompt.
	 *
	 * ABSENT rather than disabled, which is the same rule that keeps the
	 * hide-from-screenshots checkbox off the desktop settings page: a
	 * control promising a fingerprint dialog to a device with no reader
	 * -- or with a reader and nothing enrolled -- is a promise the app
	 * cannot keep, and the user who ticks it has made their own stored
	 * password unreachable.
	 *
	 * Per host because the question is about trust in one machine, which
	 * is the same reasoning sec 7.1 gives for OSC 52's write half being
	 * a per-host setting. Off by default for the reason sec 11 gives:
	 * the fallback is the typed password, so this adds a lock rather
	 * than removing one, and nobody should acquire it without asking.
	 */
	m_biometric = nullptr;
	if (bssh_platform::instance() && bssh_platform::instance()->has_biometric()) {
		m_biometric = new QCheckBox(
		        QStringLiteral("Ask for a fingerprint before using the saved password"),
		        this);
		m_biometric->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		body->addWidget(m_biometric);
	}

	/* Per host and off unless asked for; see bssh_kept_scrollback. */
	m_keep_scrollback = new QCheckBox(QStringLiteral("Keep this host's scrollback after closing"),
	                                  this);
	m_keep_scrollback->setObjectName(QStringLiteral("keep_scrollback"));
	m_keep_scrollback->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	body->addWidget(m_keep_scrollback);
	body->addWidget(bssh_wrapped_note(
	        QStringLiteral("Saved unencrypted in your settings folder, readable only by "
	                        "you -- including anything that was on screen, a password "
	                        "typed at the wrong prompt too."),
	        this));

	/*
	 * The same picker the desktop dialog embeds, sized for touch (sec
	 * 9.4: one list on both platforms). It is on the host page rather
	 * than behind a settings screen because on a phone this IS the
	 * settings screen -- there is no second window to put it in.
	 */
	m_font_picker = new bssh_font_picker(this);
	m_font_picker->set_touch_sized(true);
	body->addWidget(m_font_picker);

	/*
	 * Line the picker's label column up with this form's (sec 8.5).
	 *
	 * They already agreed, and only by accident: this form's widest label
	 * is "Key file" and the picker's is "Colours", which happen to be
	 * within a few pixels of each other. Rename either -- or translate
	 * the app -- and the page grows a second left edge, which is exactly
	 * the defect the preset editor had and which took measuring a
	 * screenshot to see. An accident that looks like a decision is worth
	 * turning into one.
	 */
	bssh_font_picker::align_labels(form, m_font_picker);

	/*
	 * sec 10.1's badge and revert, the same pair the desktop Window page
	 * carries. Without it "this host looks different from the others" and
	 * "this host overrides its preset" are the same observation, and the
	 * only way back to following the preset is deleting the host and
	 * adding it again.
	 */
	m_font_scope = new QLabel(this);

	/*
	 * By NAME, so a test can ask this page what its badge says rather
	 * than guessing which QLabel it is. findChild order is not a
	 * contract; an object name is.
	 */
	m_font_scope->setObjectName(QStringLiteral("host_font_scope"));
	bssh_wrap_note(m_font_scope);
	body->addWidget(m_font_scope);

	m_font_revert = new QPushButton(QStringLiteral("Use the preset's font"), this);
	m_font_revert->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	body->addWidget(m_font_revert);
	/*
	 * The badge follows the picker. It did not: nothing connected these
	 * on the touch page, so choosing a font left the badge saying
	 * "Following preset X" while profile() went on to write a font
	 * override -- the screen and the store disagreeing, which is the one
	 * thing this badge exists to prevent. The revert button was hidden
	 * too, so the override just made was unreachable until the page was
	 * left and reopened. The desktop dialog has always had this.
	 */
	connect(m_font_picker, &bssh_font_picker::font_chosen,
	         this, [this] { update_override_badge(); });
	connect(m_font_picker, &bssh_font_picker::scheme_chosen,
	         this, [this] { update_override_badge(); });

	connect(m_font_revert, &QPushButton::clicked,
	         this, &bssh_host_editor::revert_font_to_preset);

	body->addStretch(1);

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
	QWidget *actions = new QWidget(this);
	QVBoxLayout *action_row = new QVBoxLayout(actions);
	action_row->setContentsMargins(16, 8, 16, 16);
	action_row->setSpacing(8);
	action_row->addWidget(m_save);
	action_row->addWidget(cancel);
	outer->addWidget(actions, 0);

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

	/*
	 * The floor, for everything on the page at once (sec 6.2). This form
	 * is touch-only -- the desktop reaches the same profile through the
	 * PuTTY-shaped dialog (sec 8.5) -- so there is no condition to ask.
	 */
	bssh_apply_touch_heights(this, true);

	/*
	 * And the wheel scrolls this page rather than changing whatever
	 * control the pointer is over. Last, because it walks the form
	 * and the form has to exist.
	 */
	bssh_dont_steal_the_wheel(this);

}

void bssh_host_editor::set_profile(const bssh_profile_t &profile) {
	m_profile = profile;

	m_host->setText(profile.host);
	m_user->setText(profile.user);
	m_port->setValue(profile.port);
	m_name->setText(profile.name);

	m_folder->clear();
	m_folder->addItem(QString());
	if (m_store) m_folder->addItems(m_store->folders());
	m_folder->setCurrentText(profile.folder);
	m_identity->setText(profile.identity_file);
	m_jump->setText(profile.proxy_jump);
	if (m_biometric) m_biometric->setChecked(profile.require_biometric);
	m_keep_scrollback->setChecked(profile.keep_scrollback);

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

	const bssh_override_badge_t badge = bssh_appearance_override_badge(
	        m_store, m_profile, m_font_picker->font_id(), m_font_picker->point_size(),
	        m_font_picker->scheme_id());

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
	edited.folder = m_folder->currentText().trimmed();
	edited.identity_file = m_identity->text().trimmed();
	edited.proxy_jump = m_jump->text().trimmed();

	/*
	 * Left as it was found where the control is absent, never forced to
	 * false. A profile synced from a phone that HAS a reader keeps its
	 * setting when it is edited on a machine that does not -- otherwise
	 * opening the host on a desktop would quietly disarm it, and the
	 * user would learn that by finding the lock gone.
	 */
	if (m_biometric) edited.require_biometric = m_biometric->isChecked();
	edited.keep_scrollback = m_keep_scrollback->isChecked();

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
