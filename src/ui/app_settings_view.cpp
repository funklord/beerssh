#include "ui/app_settings_view.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include "input/input_router.h"
#include <QPushButton>
#include <QVBoxLayout>

#include "model/app_settings.h"
#include "platform/platform.h"
#include "platform/secret_store.h"
#include "ui/theme.h"

bssh_app_settings_view::bssh_app_settings_view(QWidget *parent)
    : QWidget(parent) {
	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	QVBoxLayout *outer = new QVBoxLayout(this);

	QLabel *heading = new QLabel(QStringLiteral("<b>This device</b>"), this);
	outer->addWidget(heading);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_key_row = new QComboBox(this);
	m_key_row->addItem(QStringLiteral("Automatic"), int(bssh_app_settings::KEY_ROW_AUTO));
	m_key_row->addItem(QStringLiteral("Always show"), int(bssh_app_settings::KEY_ROW_ALWAYS));
	m_key_row->addItem(QStringLiteral("Never show"), int(bssh_app_settings::KEY_ROW_NEVER));
	if (touch) m_key_row->setMinimumHeight(48);

	form->addRow(QStringLiteral("On-screen keys"), m_key_row);

	/*
	 * Directly under the control it explains, not at the foot of the
	 * page. It says what "Automatic" decides, and it sat below the whole
	 * form back when there was one setting to explain -- with three, and
	 * with Appearance offering a "Follow the system" of its own, a note
	 * about "Automatic" under a contrast checkbox reads as if it
	 * described the page.
	 *
	 * The honest answer is not "on Android": sec 6.2 gates the row on
	 * touch being the PRIMARY input, since an OS check gets a convertible
	 * in tablet mode and a tablet with a keyboard docked wrong in
	 * opposite directions.
	 */
	QLabel *note = new QLabel(
	        QStringLiteral("Automatic shows the key row when touch is this device's main "
	                        "input. Override it if you have a keyboard attached to a "
	                        "tablet, or a touchscreen on a laptop."), this);
	note->setWordWrap(true);
	form->addRow(QString(), note);

	/*
	 * Light/dark, defaulting to whatever the desktop is asking for
	 * (sec 6.2). Three states rather than a checkbox for the same reason
	 * the key row has three: "follow the system" is a real answer and the
	 * one most people want, but it is not the only one.
	 */
	m_theme = new QComboBox(this);
	m_theme->addItem(QStringLiteral("Follow the system"), QStringLiteral("system"));
	m_theme->addItem(QStringLiteral("Light"), QStringLiteral("light"));
	m_theme->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
	if (touch) m_theme->setMinimumHeight(48);
	form->addRow(QStringLiteral("Appearance"), m_theme);

	/*
	 * sec 9.3's contrast floor. The schemes are faithful reproductions
	 * and several of them are genuinely hard to read as published --
	 * xterm's blue measures 1.81:1 -- so this is on by default and lifts
	 * the offending colour when DRAWING it, leaving the scheme itself
	 * saying what its author published.
	 */
	/*
	 * How the terminal's right-click (or long-press) menu is laid out.
	 * Automatic means a pointer gets pointer-sized rows and a thumb gets
	 * thumb-sized ones; the override is here because the same device can
	 * be both, and because a desktop user who wants the big menu is
	 * entitled to it.
	 */
	m_context_menu = new QComboBox(this);
	m_context_menu->addItem(QStringLiteral("Automatic"),
	                         int(bssh_app_settings::CONTEXT_MENU_AUTO));
	m_context_menu->addItem(QStringLiteral("Pointer-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_DESKTOP));
	m_context_menu->addItem(QStringLiteral("Touch-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_TOUCH));
	if (touch) m_context_menu->setMinimumHeight(48);
	form->addRow(QStringLiteral("Terminal menu"), m_context_menu);

	/*
	 * Offered only where the platform can actually do it (sec 11: "No
	 * portable desktop equivalent exists -- don't promise one in the
	 * UI"). A checkbox that claims to hide a terminal from screenshots
	 * and cannot is worse than no checkbox: it is a security control
	 * reporting success while doing nothing.
	 */
	m_hide_from_screenshots = nullptr;
	if (bssh_platform::instance() && bssh_platform::instance()->can_hide_from_screenshots()) {
		m_hide_from_screenshots =
		        new QCheckBox(QStringLiteral("Hide the terminal from screenshots"), this);
		m_hide_from_screenshots->setToolTip(
		        QStringLiteral("Keeps this app out of screenshots, screen recordings and "
		                        "the recent-apps thumbnail. That thumbnail is a picture of "
		                        "whatever was last on screen, which here is your shell. It "
		                        "stops you screenshotting the terminal too."));
		if (touch) m_hide_from_screenshots->setMinimumHeight(48);
		form->addRow(QString(), m_hide_from_screenshots);
	}

	/*
	 * Offered only where there is a pointer to hover (sec 8.3). On touch
	 * it would be a setting with nothing to act on, which is the same
	 * dishonesty as offering to hide a window from screenshots where
	 * nothing can.
	 */
	m_focus_follows_mouse = nullptr;
	if (!touch) {
		m_focus_follows_mouse =
		        new QCheckBox(QStringLiteral("Focus the pane under the pointer"), this);
		m_focus_follows_mouse->setToolTip(
		        QStringLiteral("With more than one pane, moving the pointer over one "
		                        "gives it the keyboard without clicking. Off by default: "
		                        "it surprises anyone expecting click-to-focus, and the "
		                        "cost of that surprise is typing into the wrong shell."));
		form->addRow(QString(), m_focus_follows_mouse);
	}

	m_caps_as_ctrl = new QCheckBox(QStringLiteral("Caps Lock sends Ctrl"), this);
	m_caps_as_ctrl->setToolTip(
	        QStringLiteral("Caps Lock arms Ctrl for the next key, in this terminal only. "
	                        "Your system still toggles its own caps lock -- no application "
	                        "can prevent that -- so a permanent swap belongs in your "
	                        "keyboard layout instead."));
	if (touch) m_caps_as_ctrl->setMinimumHeight(48);
	form->addRow(QString(), m_caps_as_ctrl);

	m_raise_contrast = new QCheckBox(QStringLiteral("Raise low-contrast colours"), this);
	m_raise_contrast->setToolTip(
	        QStringLiteral("Colour schemes are reproduced exactly, and some of them are hard "
	                        "to read. This lifts a colour only while drawing it, and only when "
	                        "it falls below the readable minimum."));
	if (touch) m_raise_contrast->setMinimumHeight(48);
	form->addRow(QString(), m_raise_contrast);

	/*
	 * Where stored passwords actually live (sec 11), stated rather than
	 * implied.
	 *
	 * sec 11 asks for this in as many words: a hardware-backed keystore
	 * and a file encrypted with a passphrase the user typed are not
	 * equivalent security, and averaging them into a reassuring
	 * "encrypted" is how somebody ends up trusting the weaker one. The
	 * store has said which it is since it was written -- backend_name()
	 * and is_hardware_backed() exist for this -- and nothing showed it.
	 * The only place it appeared was the unlock prompt, which the keyring
	 * backend never raises, so the users who most needed to know were the
	 * ones who could not find out.
	 *
	 * Read once, at build: the backend is chosen at startup and does not
	 * change under a running app.
	 */
	const bssh_secret_store *store = bssh_secret_store::instance();
	if (store) {
		QLabel *backend = new QLabel(store->backend_name(), this);
		backend->setWordWrap(true);
		backend->setTextInteractionFlags(Qt::TextSelectableByMouse);

		/*
		 * A wrapped label in a form's field column is clipped rather
		 * than grown -- the layout asks it for a height before it knows
		 * the width it will get. On a phone that cut this one off at
		 * "passphrase-encrypted file (no", and losing the end of that
		 * particular label changes what it says.
		 */
		backend->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
		backend->setMinimumHeight(backend->sizeHint().height());
		form->addRow(QStringLiteral("Stored passwords"), backend);

		/*
		 * Derived from what the store reports, never from which class it
		 * is. needs_passphrase() is the honest dividing line: a store the
		 * user has to unlock by typing something is only as strong as
		 * that something, whoever wrote it.
		 */
		QString detail;
		if (store->needs_passphrase()) {
			detail = QStringLiteral(
			        "Only as safe as the passphrase you choose and as this machine: "
			        "anyone who can read the file and guess the passphrase can read "
			        "what is in it.");
		} else if (store->is_hardware_backed()) {
			detail = QStringLiteral(
			        "Held in hardware that will not hand the key back, and unlocked "
			        "the way this device is.");
		} else {
			/*
			 * True of both stores that reach this branch: a desktop
			 * keyring unlocked by the login session, and an Android
			 * Keystore whose key the system releases to this app. Saying
			 * "your session" here, as this did, is a desktop idea that
			 * does not survive the trip to a phone.
			 */
			detail = QStringLiteral(
			        "Held by the system and released to this app with nothing for you "
			        "to type. Not hardware-backed.");
		}

		QLabel *backend_note = new QLabel(detail, this);
		backend_note->setWordWrap(true);
		form->addRow(QString(), backend_note);
	}

	outer->addLayout(form);

	/*
	 * The chords this app keeps for itself (sec 6.1), which asks that the
	 * set be minimal AND documented. It was neither shown nor written
	 * down anywhere a user could reach: the only way to discover that
	 * Ctrl+Shift+C never reaches your shell was to press it and notice.
	 *
	 * Built from bssh_input_router::stolen_chords(), which is the same
	 * list handle_key() matches against, so this cannot drift into
	 * describing a set the router no longer takes. Read-only for now --
	 * sec 6.1 also asks for these to be remappable, and that is not
	 * built.
	 */
	QLabel *chords_heading = new QLabel(
	        QStringLiteral("<b>Keys this app keeps</b>"), this);
	outer->addWidget(chords_heading);

	QLabel *chords_note = new QLabel(
	        QStringLiteral("These never reach the remote shell."), this);
	chords_note->setWordWrap(true);
	outer->addWidget(chords_note);

	QFormLayout *chords = new QFormLayout;
	for (const bssh_input_router::chord_t &chord : bssh_input_router::stolen_chords()) {
		QLabel *what = new QLabel(QString::fromLatin1(chord.what), this);
		what->setWordWrap(true);
		what->setObjectName(QStringLiteral("stolen_chord"));
		chords->addRow(QString::fromLatin1(chord.label), what);
	}
	outer->addLayout(chords);

	outer->addStretch(1);

	if (touch) {
		QPushButton *back = new QPushButton(QStringLiteral("Back"), this);
		back->setMinimumHeight(48);
		connect(back, &QPushButton::clicked, this, &bssh_app_settings_view::back_requested);
		outer->addWidget(back);
	}

	const bssh_app_settings settings;
	if (m_hide_from_screenshots) {
		m_hide_from_screenshots->setChecked(settings.hide_from_screenshots());
	}
	if (m_focus_follows_mouse) {
		m_focus_follows_mouse->setChecked(settings.focus_follows_mouse());
	}
	m_caps_as_ctrl->setChecked(settings.caps_as_ctrl());
	m_raise_contrast->setChecked(settings.raise_contrast());

	const int menu_index = m_context_menu->findData(int(settings.context_menu()));
	m_context_menu->setCurrentIndex(menu_index < 0 ? 0 : menu_index);

	const int theme_index = m_theme->findData(settings.theme());
	if (theme_index >= 0) m_theme->setCurrentIndex(theme_index);

	const int index = m_key_row->findData(int(settings.key_row_visibility()));
	m_key_row->setCurrentIndex(index < 0 ? 0 : index);

	/*
	 * Every control commits, not just the first one. The lambda used to
	 * hang off the key row alone, so once a second setting appeared here
	 * changing IT saved nothing until the key row was also touched --
	 * which looks exactly like a setting that does not work.
	 */
	connect(m_key_row, &QComboBox::currentIndexChanged, this, [this] { commit(); });
	connect(m_theme, &QComboBox::currentIndexChanged, this, [this] { commit(); });
	connect(m_context_menu, &QComboBox::currentIndexChanged, this, [this] { commit(); });
	if (m_hide_from_screenshots) {
		connect(m_hide_from_screenshots, &QCheckBox::toggled, this, [this] { commit(); });
	}
	if (m_focus_follows_mouse) {
		connect(m_focus_follows_mouse, &QCheckBox::toggled, this, [this] { commit(); });
	}
	connect(m_caps_as_ctrl, &QCheckBox::toggled, this, [this] { commit(); });
	connect(m_raise_contrast, &QCheckBox::toggled, this, [this] { commit(); });
}

void bssh_app_settings_view::commit() {
	bssh_app_settings stored;
	stored.set_key_row_visibility(
	        bssh_app_settings::key_row_visibility_t(m_key_row->currentData().toInt()));
	stored.set_theme(m_theme->currentData().toString());
	stored.set_context_menu(
	        bssh_app_settings::context_menu_t(m_context_menu->currentData().toInt()));
	stored.set_raise_contrast(m_raise_contrast->isChecked());
	stored.set_caps_as_ctrl(m_caps_as_ctrl->isChecked());
	if (m_focus_follows_mouse) {
		stored.set_focus_follows_mouse(m_focus_follows_mouse->isChecked());
	}
	if (m_hide_from_screenshots) {
		stored.set_hide_from_screenshots(m_hide_from_screenshots->isChecked());
	}

	/* Applied immediately: a setting that waits for a restart reads as a
	 * setting that did not work. */
	bssh_theme::apply(bssh_theme::from_text(m_theme->currentData().toString()));
	emit changed();
}
