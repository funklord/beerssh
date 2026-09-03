#include "ui/touch_metrics.h"
#include "ui/app_settings_view.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QSlider>
#include <QComboBox>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QLabel>

#include "ui/scroll_guard.h"
#include "ui/wrapped_note.h"
#include "term/colour_scheme.h"
#include "ui/colour_editor.h"
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
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	/*
	 * The fields scroll and Back stays pinned to the bottom, the same
	 * shape the host and preset editors were given when sec 8.3 measured
	 * them against a phone. This page was missed then and has grown since
	 * -- it is the tallest touch surface in the app -- and it asks for
	 * 752px of a 640px screen.
	 *
	 * It looked like it fitted until the wrapped notes stopped
	 * under-reporting their height (bssh_wrapped_note). The page was
	 * always this tall; what changed is that it now says so.
	 */
	QWidget *content = new QWidget(this);
	QVBoxLayout *body = new QVBoxLayout(content);
	body->setContentsMargins(16, 16, 16, 16);
	body->setSpacing(12);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	/*
	 * Sideways scrolling would mean something above is too wide, which is
	 * a bug rather than something to let the user work around.
	 */
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
	outer->addWidget(scroll, 1);

	QLabel *heading = new QLabel(QStringLiteral("<b>This device</b>"), this);
	body->addWidget(heading);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_key_row = new QComboBox(this);
	m_key_row->addItem(QStringLiteral("Automatic"), int(bssh_app_settings::KEY_ROW_AUTO));
	m_key_row->addItem(QStringLiteral("Always show"), int(bssh_app_settings::KEY_ROW_ALWAYS));
	m_key_row->addItem(QStringLiteral("Never show"), int(bssh_app_settings::KEY_ROW_NEVER));

	form->addRow(QStringLiteral("On-screen keys"), m_key_row);

	/*
	 * Which keyboard, above the row's own setting in importance and
	 * directly under it on the page, because the row is what the first
	 * choice turns off.
	 */
	m_keyboard = new QComboBox(this);
	m_keyboard->setObjectName(QStringLiteral("keyboard"));
	m_keyboard->addItem(QStringLiteral("This device's keyboard"),
	                     int(bssh_app_settings::KEYBOARD_SYSTEM));
	m_keyboard->addItem(QStringLiteral("BeerSSH's own keyboard"),
	                     int(bssh_app_settings::KEYBOARD_BUILT_IN));
	form->addRow(QStringLiteral("Keyboard"), m_keyboard);

	QLabel *keyboard_note = bssh_wrapped_note(
	        QStringLiteral("This device's keyboard keeps its prediction, languages and "
	                        "layout, with the key row above it for Ctrl, Esc, Tab and the "
	                        "arrows it has none of. Choose BeerSSH's own where no keyboard "
	                        "comes up at all, or where you would rather have the terminal "
	                        "keys on the keyboard itself."), this);
	form->addRow(keyboard_note);

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
	/*
	 * addRow(widget) rather than addRow(QString(), widget): a row with no
	 * label SPANS both columns, where an empty label reserves the label
	 * column's width for nothing.
	 *
	 * That waste is what pushed this page off a phone. The paste warning's
	 * checkbox asks 241px, the label column costs 145, and 386 of a 346px
	 * screen put every note beside it off the right edge -- a form nobody
	 * could read, on the one layout built for a thumb. Every checkbox and
	 * every note on this page had been paying it.
	 */
	QLabel *note = bssh_wrapped_note(
	        QStringLiteral("Automatic shows the key row when touch is this device's main "
	                        "input. Override it if you have a keyboard attached to a "
	                        "tablet, or a touchscreen on a laptop."), this);
	form->addRow(note);

	/*
	 * Light/dark, defaulting to whatever the desktop is asking for
	 * (sec 6.2). Three states rather than a checkbox for the same reason
	 * the key row has three: "follow the system" is a real answer and the
	 * one most people want, but it is not the only one.
	 */
	m_theme = new QComboBox(this);

	/*
	 * Named, so a test can reach it. There is one of these pages per
	 * window and what they write to app.ini has to be asserted across
	 * two of them; the chord editors below are named for the same
	 * reason.
	 */
	m_theme->setObjectName(QStringLiteral("theme"));
	m_theme->addItem(QStringLiteral("Follow the system"), QStringLiteral("system"));
	m_theme->addItem(QStringLiteral("Light"), QStringLiteral("light"));
	m_theme->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
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
	/*
	 * The scrollbar (sec 8.3), Konsole's three answers plus this app's
	 * usual "ask the device". Filed under appearance beside the theme,
	 * because that is what the user came here looking for.
	 */
	m_scrollbar = new QComboBox(this);
	m_scrollbar->addItem(QStringLiteral("Automatic"),
	                      int(bssh_app_settings::SCROLLBAR_AUTO));
	m_scrollbar->addItem(QStringLiteral("On the right"),
	                      int(bssh_app_settings::SCROLLBAR_RIGHT));
	m_scrollbar->addItem(QStringLiteral("On the left"),
	                      int(bssh_app_settings::SCROLLBAR_LEFT));
	m_scrollbar->addItem(QStringLiteral("Hidden"),
	                      int(bssh_app_settings::SCROLLBAR_HIDDEN));
	form->addRow(QStringLiteral("Scrollbar"), m_scrollbar);

	QLabel *scrollbar_note = bssh_wrapped_note(
	        QStringLiteral("Automatic shows one where there is a pointer to drag it "
	                        "with. How much there is to scroll through is the host's "
	                        "own setting."),
	        this);
	form->addRow(scrollbar_note);

	/*
	 * What the status line says, or that it says nothing (sec 8.4). It
	 * used to be unconditional, and its first field was the QPA platform
	 * name -- a fact a developer wants and a user has no use for.
	 */
	m_status_line = new QComboBox(this);
	m_status_line->addItem(QStringLiteral("Session and state"),
	                        int(bssh_app_settings::STATUS_LINE_SESSION));
	m_status_line->addItem(QStringLiteral("Everything"),
	                        int(bssh_app_settings::STATUS_LINE_FULL));
	m_status_line->addItem(QStringLiteral("Hidden"),
	                        int(bssh_app_settings::STATUS_LINE_OFF));

	/*
	 * A combo box asks for the width of its widest item, and a phone form
	 * has no width to spare: "Everything, including the grid size" read
	 * well and dragged the whole page 40px past the screen, which put the
	 * notes beside it off the right edge. What the choices mean belongs in
	 * the note below, which wraps; the item labels stay short.
	 */
	QLabel *status_note = bssh_wrapped_note(
	        QStringLiteral("Everything adds the terminal's size in cells and which "
	                        "windowing system this build is using."),
	        this);
	form->addRow(QStringLiteral("Status line"), m_status_line);
	form->addRow(status_note);

	/*
	 * The window title, as a format. A list of three canned choices would
	 * have been less code and would have fitted nobody: everyone wants a
	 * different one of the same four facts, which is why Konsole, xterm
	 * and tmux all ended up with a format string.
	 */
	m_window_title = new QLineEdit(this);
	m_window_title->setPlaceholderText(QStringLiteral("%t - BeerSSH"));
	form->addRow(QStringLiteral("Window title"), m_window_title);

	QLabel *title_note = bssh_wrapped_note(
	        QStringLiteral("%t is what the session calls itself, %n its saved name, "
	                        "%u the user, %h the host, %p the port, %a this app. "
	                        "Empty means the default."),
	        this);
	form->addRow(title_note);

	m_context_menu = new QComboBox(this);
	m_context_menu->addItem(QStringLiteral("Automatic"),
	                         int(bssh_app_settings::CONTEXT_MENU_AUTO));
	m_context_menu->addItem(QStringLiteral("Pointer-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_DESKTOP));
	m_context_menu->addItem(QStringLiteral("Touch-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_TOUCH));
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
		form->addRow(m_hide_from_screenshots);
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
		form->addRow(m_focus_follows_mouse);
	}

	/*
	 * sec 6.4's multi-line paste warning, which that section asked for
	 * "by default (a setting)" and which nothing had built -- so a paste
	 * carrying five commands ran all five.
	 */
	m_warn_multiline_paste = new QCheckBox(
	        QStringLiteral("Ask before pasting more than one line"), this);
	form->addRow(m_warn_multiline_paste);

	QLabel *paste_note = bssh_wrapped_note(
	        QStringLiteral("A newline in a paste is Enter to the shell, so every line "
	                        "but the last runs before you can read it."),
	        this);
	form->addRow(paste_note);

	m_caps_as_ctrl = new QCheckBox(QStringLiteral("Caps Lock sends Ctrl"), this);
	m_caps_as_ctrl->setObjectName(QStringLiteral("caps_as_ctrl"));
	m_caps_as_ctrl->setToolTip(
	        QStringLiteral("Caps Lock arms Ctrl for the next key, in this terminal only. "
	                        "Your system still toggles its own caps lock -- no application "
	                        "can prevent that -- so a permanent swap belongs in your "
	                        "keyboard layout instead."));
	form->addRow(m_caps_as_ctrl);

	m_raise_contrast = new QCheckBox(QStringLiteral("Raise low-contrast colours"), this);
	m_raise_contrast->setToolTip(
	        QStringLiteral("Colour schemes are reproduced exactly, and some of them are hard "
	                        "to read. This lifts a colour only while drawing it, and only when "
	                        "it falls below the readable minimum."));
	form->addRow(m_raise_contrast);

	/*
	 * The night knob (sec 9.3). A slider rather than a light/dark toggle
	 * because the complaint it answers is a matter of degree: a terminal
	 * that is comfortable at a desk at noon is a lamp in a dark room, and
	 * where the line falls depends on the room and the panel.
	 *
	 * It dims the TEXT and leaves the background alone, so pure black
	 * stays pure black -- turning the light down rather than greying the
	 * screen out. The floor above still applies first; this is allowed to
	 * take it below that, because a slider a legibility default could
	 * veto would stop working exactly where somebody reached for it.
	 */
	QWidget *brightness_row = new QWidget(this);
	QHBoxLayout *brightness_layout = new QHBoxLayout(brightness_row);
	brightness_layout->setContentsMargins(0, 0, 0, 0);

	m_text_brightness = new QSlider(Qt::Horizontal, brightness_row);
	m_text_brightness->setRange(10, 100);
	m_text_brightness->setSingleStep(5);
	m_text_brightness->setPageStep(10);
	m_text_brightness->setToolTip(
	        QStringLiteral("How bright terminal text is drawn. The background is not "
	                        "touched, so black stays black and only the text comes down."));

	/*
	 * The number, beside it. A slider with no read-out is a control you
	 * cannot return to a setting you liked, and "about two thirds" is not
	 * a thing anyone can write down.
	 */
	m_text_brightness_value = new QLabel(brightness_row);
	m_text_brightness_value->setMinimumWidth(
	        m_text_brightness_value->fontMetrics().horizontalAdvance(QStringLiteral("100%")));

	brightness_layout->addWidget(m_text_brightness, 1);
	brightness_layout->addWidget(m_text_brightness_value);

	/* Sized by the walk at the end of this constructor, never by name. */
	form->addRow(QStringLiteral("Text brightness"), brightness_row);

	QLabel *brightness_note = bssh_wrapped_note(
	        QStringLiteral("For reading at night. Applies to every terminal on this "
	                        "device, not to one host."),
	        this);
	form->addRow(brightness_note);

	/*
	 * The escape hatch (sec 9.3). A button to a dialog rather than
	 * nineteen colour wells on this page: changing the sixteen is a
	 * deliberate act with consequences for every program on every host,
	 * and a control that takes a trip to get to is one somebody chose
	 * rather than one they brushed past on the way to the font.
	 *
	 * The palette itself is device-wide, like the brightness above it,
	 * while WHICH scheme a host uses stays with the host. So "custom"
	 * means "this device's custom palette", and the two questions stay
	 * in the two places that already answer them.
	 */
	m_custom_colours = new QPushButton(QStringLiteral("Custom colours..."), this);
	m_custom_colours->setToolTip(
	        QStringLiteral("Edit the palette the Custom scheme uses. Choose Custom for a "
	                        "host in its Appearance settings to see it."));
	connect(m_custom_colours, &QPushButton::clicked, this, [this] {
		bssh_app_settings stored;
		bssh_colour_editor editor(stored.custom_scheme(), this);
		if (editor.exec() != QDialog::Accepted) return;

		stored.set_custom_scheme(editor.scheme());

		/*
		 * Into the catalog immediately, or every view keeps drawing the
		 * palette it copied when it resolved the scheme -- the id has
		 * not changed, so nothing else would prompt a re-read.
		 */
		bssh_colour_scheme_catalog::instance().set_custom(editor.scheme());
		emit changed();
	});
	/*
	 * addRow(widget), not addRow(QString(), widget). An empty label
	 * reserves the label column's width for nothing -- the waste that
	 * pushed this page 40px off a phone once already -- and it also put
	 * the button in the field column while every checkbox above it
	 * starts at the margin, so the page had two left edges.
	 *
	 * In a row of its own with a stretch after it, because a spanning row
	 * hands a button the whole width: a full-width button reads as the
	 * page's primary action, and this one opens a dialog.
	 */
	QWidget *colours_row = new QWidget(this);
	QHBoxLayout *colours_layout = new QHBoxLayout(colours_row);
	colours_layout->setContentsMargins(0, 0, 0, 0);
	colours_layout->addWidget(m_custom_colours);
	colours_layout->addStretch(1);

	form->addRow(colours_row);

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
		QLabel *backend = bssh_wrapped_note(store->backend_name(), this);
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

		QLabel *backend_note = bssh_wrapped_note(detail, this);
		form->addRow(backend_note);
	}

	body->addLayout(form);

	/*
	 * The chords this app keeps for itself (sec 6.1), which asks that the
	 * set be minimal AND documented. It was neither shown nor written
	 * down anywhere a user could reach: the only way to discover that
	 * Ctrl+Shift+C never reaches your shell was to press it and notice.
	 *
	 * This used to end "Read-only for now -- sec 6.1 also asks for these
	 * to be remappable, and that is not built", and named
	 * stolen_chords() as what the rows came from. Both stopped being
	 * true when the rows below became QKeySequenceEdits built from
	 * default_chords(), and the comment describing that change was added
	 * ten lines down rather than here -- so the file said the feature was
	 * missing directly above the code implementing it.
	 *
	 * Worth the words because of which way the error points: a stale
	 * comment claiming something is UNBUILT invites the next reader to
	 * build it again, and the honest reasons in it are what make the
	 * claim credible.
	 */
	QLabel *chords_heading = new QLabel(
	        QStringLiteral("<b>Keys this app keeps</b>"), this);
	body->addWidget(chords_heading);

	QLabel *chords_note = bssh_wrapped_note(
	        QStringLiteral("These never reach the remote shell."), this);
	body->addWidget(chords_note);

	/*
	 * Editable, which sec 6.1 asked for and nothing had built: "a chord a
	 * user wants back is one they cannot yet have". A QKeySequenceEdit
	 * because it is the control the platform already has for this job --
	 * it records the chord you press rather than making you spell it,
	 * which is the difference between a setting people use and one they
	 * read about.
	 *
	 * Built from default_chords() rather than from the effective list, so
	 * every action has a row even when the user has given its chord back
	 * -- an action that vanished from the page once unbound could never
	 * be bound again.
	 */
	QFormLayout *chords = new QFormLayout;
	const bssh_app_settings bound;

	for (const bssh_input_router::chord_t &chord : bssh_input_router::default_chords()) {
		const QString name = QString::fromLatin1(chord.name);

		QKeySequenceEdit *edit = new QKeySequenceEdit(this);
		edit->setObjectName(QStringLiteral("chord_") + name);
		edit->setKeySequence(QKeySequence(bound.has_chord(name) ? bound.chord(name)
		                                                         : chord.label,
		                                   QKeySequence::PortableText));

		connect(edit, &QKeySequenceEdit::editingFinished, this, [this, name, edit] {
			bssh_app_settings stored;
			stored.set_chord(name, edit->keySequence().toString(QKeySequence::PortableText));
			emit changed();
		});

		/*
		 * The description IS the row label, which is how every shortcut
		 * editor from Konsole's onwards lays this out: the reader is
		 * looking for the action and wants to see what it is bound to,
		 * not the other way round.
		 */
		chords->addRow(QString::fromLatin1(chord.what), edit);
	}
	body->addLayout(chords);

	QLabel *chords_reset_note = bssh_wrapped_note(
	        QStringLiteral("Clear a chord to give it back to the remote shell. "
	                        "Whatever this app keeps, the shell never sees."),
	        this);
	body->addWidget(chords_reset_note);

	body->addStretch(1);

	if (touch) {
		QPushButton *back = new QPushButton(QStringLiteral("Back"), this);
		connect(back, &QPushButton::clicked, this, &bssh_app_settings_view::back_requested);

		/*
		 * Outside the scroll area on purpose: a way back that scrolls
		 * off the bottom is one the user has to go looking for, and on
		 * this page it would be past six chord rows.
		 */
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
	m_warn_multiline_paste->setChecked(settings.warn_multiline_paste());
	m_raise_contrast->setChecked(settings.raise_contrast());
	m_text_brightness->setValue(settings.text_brightness());
	m_text_brightness_value->setText(QStringLiteral("%1%").arg(settings.text_brightness()));

	const int bar_index = m_scrollbar->findData(int(settings.scrollbar()));
	m_scrollbar->setCurrentIndex(bar_index < 0 ? 0 : bar_index);

	const int status_index = m_status_line->findData(int(settings.status_line()));
	m_status_line->setCurrentIndex(status_index < 0 ? 0 : status_index);

	m_window_title->setText(settings.window_title());

	const int menu_index = m_context_menu->findData(int(settings.context_menu()));
	m_context_menu->setCurrentIndex(menu_index < 0 ? 0 : menu_index);

	const int theme_index = m_theme->findData(settings.theme());
	if (theme_index >= 0) m_theme->setCurrentIndex(theme_index);

	const int index = m_key_row->findData(int(settings.key_row_visibility()));
	m_key_row->setCurrentIndex(index < 0 ? 0 : index);

	const int keyboard_index = m_keyboard->findData(int(settings.keyboard()));
	m_keyboard->setCurrentIndex(keyboard_index < 0 ? 0 : keyboard_index);

	/*
	 * Every control commits, not just the first one. The lambda used to
	 * hang off the key row alone, so once a second setting appeared here
	 * changing IT saved nothing until the key row was also touched --
	 * which looks exactly like a setting that does not work.
	 */
	connect(m_key_row, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_key_row_visibility(bssh_app_settings::key_row_visibility_t(
			        m_key_row->currentData().toInt()));
		});
	});
	connect(m_keyboard, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_keyboard(bssh_app_settings::keyboard_t(
			        m_keyboard->currentData().toInt()));
		});
	});
	connect(m_theme, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_theme(m_theme->currentData().toString());
		});
	});
	connect(m_context_menu, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_context_menu(bssh_app_settings::context_menu_t(
			        m_context_menu->currentData().toInt()));
		});
	});
	connect(m_scrollbar, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_scrollbar(bssh_app_settings::scrollbar_t(
			        m_scrollbar->currentData().toInt()));
		});
	});
	connect(m_status_line, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_status_line(bssh_app_settings::status_line_t(
			        m_status_line->currentData().toInt()));
		});
	});

	/*
	 * On editingFinished rather than on every keystroke: a format is
	 * typed a character at a time and "%" alone is not a title anybody
	 * meant to see in their task switcher.
	 */
	connect(m_window_title, &QLineEdit::editingFinished, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_window_title(m_window_title->text()); });
	});
	if (m_hide_from_screenshots) {
		connect(m_hide_from_screenshots, &QCheckBox::toggled, this, [this] {
			commit([this](bssh_app_settings &s) {
				s.set_hide_from_screenshots(m_hide_from_screenshots->isChecked());
			});
		});
	}
	if (m_focus_follows_mouse) {
		connect(m_focus_follows_mouse, &QCheckBox::toggled, this, [this] {
			commit([this](bssh_app_settings &s) {
				s.set_focus_follows_mouse(m_focus_follows_mouse->isChecked());
			});
		});
	}
	connect(m_caps_as_ctrl, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_caps_as_ctrl(m_caps_as_ctrl->isChecked()); });
	});
	connect(m_warn_multiline_paste, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_warn_multiline_paste(m_warn_multiline_paste->isChecked());
		});
	});
	connect(m_raise_contrast, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_raise_contrast(m_raise_contrast->isChecked()); });
	});
	connect(m_text_brightness, &QSlider::valueChanged, this, [this](int value) {
		m_text_brightness_value->setText(QStringLiteral("%1%").arg(value));
		commit([value](bssh_app_settings &s) { s.set_text_brightness(value); });
	});

	/*
	 * The floor, once, for everything on the page (sec 6.2).
	 *
	 * It was eleven `if (touch) x->setMinimumHeight(...)` lines, which is
	 * the shape that keeps going wrong here -- the preset editor's
	 * fourteen-item list missed two controls in turn. It also missed what
	 * no per-widget line could reach: the rows of the popups these combo
	 * boxes drop down, which stood 25dp. What a finger taps when choosing
	 * a setting is a row in a list, not the closed box.
	 */
	bssh_apply_touch_heights(this, touch);

	/*
	 * And the wheel scrolls this page rather than changing whatever
	 * control the pointer is over. Last, because it walks the form
	 * and the form has to exist.
	 */
	bssh_dont_steal_the_wheel(this);
}

void bssh_app_settings_view::commit(const std::function<void(bssh_app_settings &)> &write) {
	bssh_app_settings stored;
	if (write) write(stored);

	/*
	 * The theme is applied from what is now STORED rather than from this
	 * page's combo. Another window may have set it since this page was
	 * built, and applying the widget would undo that on screen even now
	 * that the write no longer does.
	 */
	bssh_theme::apply(bssh_theme::from_text(stored.theme()));
	emit changed();
}
