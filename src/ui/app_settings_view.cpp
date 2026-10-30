#include "ui/touch_metrics.h"
#include "ui/app_settings_view.h"

#include "softkeys/keyboard_layout.h"

#include <QCheckBox>
#include <QShowEvent>
#include <QHBoxLayout>
#include <QSlider>
#include <QSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QStyle>
#include <QLineEdit>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QLabel>

#include "ui/pane_snap.h"
#include "ui/scroll_guard.h"
#include "ui/wrapped_note.h"
#include "term/colour_scheme.h"
#include "ui/colour_editor.h"
#include "ui/key_row_editor.h"
#include "input/input_router.h"
#include <QPushButton>
#include <QVBoxLayout>

#include "model/app_settings.h"
#include "platform/platform.h"
#include "platform/secret_store.h"
#include "ui/theme.h"

/*
 * A combo box wide enough for the longest thing it can show.
 *
 * QComboBox's own minimum lets it shrink below its current item, so on a
 * 320dp screen the keyboard row drew "BeerSSH's own, compac" -- a control
 * reporting a setting by a name that is not the setting's name. The width
 * assertion cannot see it: a widget happy to clip has a small minimum and
 * therefore FITS.
 *
 * Stating the real requirement is also what lets WrapLongRows do its job:
 * a field that admits it needs the room stops sharing a row with its
 * label, rather than silently truncating beside it.
 */
static void bssh_fit_items(QComboBox *box) {
	int widest = 0;
	for (int i = 0; i < box->count(); ++i) {
		widest = qMax(widest, box->fontMetrics().horizontalAdvance(box->itemText(i)));
	}

	/* The frame and the drop-down arrow, which the text does not include. */
	const int chrome = box->style()->pixelMetric(QStyle::PM_ScrollBarExtent)
	                    + box->fontMetrics().horizontalAdvance(QLatin1Char('W')) * 2;
	box->setMinimumWidth(widest + chrome);
}

/*
 * Every pointer member starts null, and on this class it is load-bearing
 * rather than tidy: resizeEvent() calls refresh_keyboard_fallback(),
 * which guards on `!m_keyboard || !m_keyboard_fallback` -- and a resize
 * can arrive while this constructor is still building them, because
 * adding a widget to a layout can lay the parent out again. With none of
 * them initialised that guard read uninitialised memory and would pass a
 * garbage pointer straight into a dereference.
 *
 * Found by the same lens that caught it in bssh_font_picker: a null
 * guard is only as good as the value it reads.
 */
QString bssh_app_settings_view::section_name(section_t section) {
	switch (section) {
	case SECTION_KEYBOARD:   return QStringLiteral("Keyboard");
	case SECTION_APPEARANCE: return QStringLiteral("Appearance");
	case SECTION_TERMINAL:   return QStringLiteral("Terminal");
	case SECTION_PASSWORDS:  return QStringLiteral("Stored passwords");
	case SECTION_SHORTCUTS:  return QStringLiteral("Shortcuts");
	case EVERYTHING:
	case EVERYTHING_BUT_APPEARANCE:
		break;
	}
	return QStringLiteral("This device");
}

void bssh_app_settings_view::set_heading(const QString &heading) {
	m_heading->setText(QStringLiteral("<b>%1</b>").arg(heading.toHtmlEscaped()));
}

bool bssh_app_settings_view::on_this_page(section_t section, QWidget *widget) {
	const bool wanted = m_section == EVERYTHING || m_section == section
	                    || (m_section == EVERYTHING_BUT_APPEARANCE && section != SECTION_APPEARANCE);
	if (!wanted && widget) widget->hide();
	return wanted;
}

bssh_app_settings_view::bssh_app_settings_view(QWidget *parent)
    : bssh_app_settings_view(EVERYTHING, parent) {}

bssh_app_settings_view::bssh_app_settings_view(section_t section, QWidget *parent)
    : QWidget(parent),
      m_section(section),
      m_key_row(nullptr),
      m_keyboard(nullptr),
      m_keyboard_fallback(nullptr),
      m_keyboard_layout(nullptr),
      m_theme(nullptr),
      m_scrollbar(nullptr),
      m_arrangement(nullptr),
      m_snap_radius(nullptr),
      m_snap_radius_value(nullptr),
      m_status_line(nullptr),
      m_window_title(nullptr),
      m_context_menu(nullptr),
      m_caps_as_ctrl(nullptr),
      m_keyboard_grips(nullptr),
      m_keyboard_reset(nullptr),
      m_warn_multiline_paste(nullptr),
      m_keep_sessions_on_close(nullptr),
      m_offer_multiplexers(nullptr),
      m_honour_font_scale(nullptr),
      m_font_ligatures(nullptr),
      m_honour_remote_cursor(nullptr),
      m_focus_follows_mouse(nullptr),
      m_hide_from_screenshots(nullptr),
      m_raise_contrast(nullptr),
      m_text_brightness(nullptr),
      m_custom_colours(nullptr),
      m_custom_key_row(nullptr),
      m_text_brightness_value(nullptr) {
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

	/*
	 * The heading names the page rather than always saying "This device".
	 * On the desktop that is still what it says, because there the whole
	 * view IS the "This device" category of sec 8.5's tree; a touch
	 * section page says which section it is, since the user arrived by
	 * tapping that row and Back is the way out.
	 */
	m_heading = new QLabel(QStringLiteral("<b>%1</b>")
	                               .arg(section_name(m_section).toHtmlEscaped()),
	                        this);
	body->addWidget(m_heading);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	/*
	 * The same policy as the chord form below, for the same reason and so
	 * that the two halves of one page cannot answer a narrow screen
	 * differently. This form's labels are shorter, so in practice it
	 * wraps later -- which is what WrapLongRows means.
	 */
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);

	m_key_row = new QComboBox(this);
	m_key_row->addItem(QStringLiteral("Automatic"), int(bssh_app_settings::KEY_ROW_AUTO));
	m_key_row->addItem(QStringLiteral("Always show"), int(bssh_app_settings::KEY_ROW_ALWAYS));
	m_key_row->addItem(QStringLiteral("Never show"), int(bssh_app_settings::KEY_ROW_NEVER));

	bssh_fit_items(m_key_row);
	if (on_this_page(SECTION_KEYBOARD, m_key_row)) form->addRow(QStringLiteral("On-screen keys"), m_key_row);

	/*
	 * Composing your own row (sec 6.2, sec 6.3). Device-wide, like the
	 * custom palette above and for the same reason: WHICH row a host uses
	 * is the host's question and lives with the host, while what "Custom"
	 * contains is this device's.
	 *
	 * Behind a dialog rather than on the page, which is the colour
	 * editor's argument repeated: a row is a dozen decisions, and a
	 * control that takes a deliberate trip is a choice somebody made
	 * instead of one they made by accident on the way past.
	 */
	m_custom_key_row = new QPushButton(QStringLiteral("Custom key row..."), this);
	m_custom_key_row->setToolTip(
	        QStringLiteral("Compose the row the Custom layout uses. Choose Custom for a "
	                        "host in its Appearance settings to see it."));
	connect(m_custom_key_row, &QPushButton::clicked, this, [this] {
		bssh_app_settings stored;

		/*
		 * Seeded from what is stored, or from the catalogue's default
		 * where nothing is -- an empty field would make the first use of
		 * this dialog start by failing its own validity check, which
		 * reads as the feature being broken.
		 */
		QString seed = stored.custom_key_row();
		if (seed.isEmpty()) {
			const sk_key_row_catalog catalog;
			if (const sk_key_row_layout_t *fallback =
			            catalog.by_id(catalog.default_id())) {
				seed = sk_key_row_spec(fallback->keys);
			}
		}

		bssh_key_row_editor editor(seed, this);
		if (editor.exec() != QDialog::Accepted) return;

		stored.set_custom_key_row(editor.spec());

		/*
		 * Into the catalogue at once, the same as the palette: the id
		 * has not changed, so nothing else would prompt a re-read and
		 * every open row would keep the keys it copied.
		 */
		sk_key_row_catalog::instance().set_custom(
		        sk_parse_key_row(editor.spec()));
		emit changed();
	});

	QWidget *key_row_row = new QWidget(this);
	QHBoxLayout *key_row_layout = new QHBoxLayout(key_row_row);
	key_row_layout->setContentsMargins(0, 0, 0, 0);
	key_row_layout->addWidget(m_custom_key_row);
	key_row_layout->addStretch(1);

	if (on_this_page(SECTION_KEYBOARD, key_row_row)) form->addRow(key_row_row);

	/*
	 * Which keyboard, above the row's own setting in importance and
	 * directly under it on the page, because the row is what the first
	 * choice turns off.
	 */
	m_keyboard = new QComboBox(this);
	m_keyboard->setObjectName(QStringLiteral("keyboard"));
	m_keyboard->addItem(QStringLiteral("This device's keyboard"),
	                     int(bssh_app_settings::KEYBOARD_SYSTEM));
	/*
	 * Short deliberately. A combo asks for room for its LONGEST item, and
	 * the page it sits on has to fit 360dp -- "BeerSSH's own keyboard
	 * (full size)" wanted 373px of the 346 available and pushed the whole
	 * settings form into sideways scrolling. Caught by the touch-fits
	 * test rather than by looking, which is the point of that test.
	 */
	m_keyboard->addItem(QStringLiteral("BeerSSH's own, compact"),
	                     int(bssh_app_settings::KEYBOARD_BUILT_IN));
	m_keyboard->addItem(QStringLiteral("BeerSSH's own, full size"),
	                     int(bssh_app_settings::KEYBOARD_BUILT_IN_FULL));
	m_keyboard->addItem(QStringLiteral("BeerSSH's own, split"),
	                     int(bssh_app_settings::KEYBOARD_BUILT_IN_SPLIT));
	bssh_fit_items(m_keyboard);
	if (on_this_page(SECTION_KEYBOARD, m_keyboard)) form->addRow(QStringLiteral("Keyboard"), m_keyboard);

	/*
	 * No "Unset, this device uses ..." line under the combo any more: the
	 * combo already shows what this device uses, so the line said it a
	 * second time (the holder, 2026-10-22).
	 */

	/*
	 * Which layout the built-in keyboard draws (sec 6.2).
	 *
	 * Beside the keyboard setting rather than on a preset, because a
	 * layout is where a PERSON's hands expect the letters to be. The
	 * colour scheme and the key row above are preset-owned since they
	 * suit a host; a different alphabet per server would be a strange
	 * thing to want.
	 *
	 * Listed from the catalog rather than spelled out here, so adding a
	 * layout is one table and no edit to this page.
	 */
	m_keyboard_layout = new QComboBox(this);
	m_keyboard_layout->setObjectName(QStringLiteral("keyboard-layout"));
	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
		m_keyboard_layout->addItem(layout.display_name, layout.id);
	}
	bssh_fit_items(m_keyboard_layout);
	if (on_this_page(SECTION_KEYBOARD, m_keyboard_layout)) form->addRow(QStringLiteral("Layout"), m_keyboard_layout);

	QLabel *keyboard_note = bssh_wrapped_note(
	        QStringLiteral("This device's keyboard keeps its prediction, languages and "
	                        "layout, with the key row above it for Ctrl, Esc, Tab and the "
	                        "arrows it has none of. Choose BeerSSH's own where no keyboard "
	                        "comes up at all, or where you would rather have the terminal "
	                        "keys on the keyboard itself."), this);
	if (on_this_page(SECTION_KEYBOARD, keyboard_note)) form->addRow(keyboard_note);

	/*
	 * What the choice will ACTUALLY give on this screen.
	 *
	 * The full-size keyboard needs about 600dp and falls back to the
	 * compact one below that (sec 6.2). Silently: the holder chose it on
	 * a folded phone, nothing changed, and reported it as "the choice
	 * doesn't work on a small screen" -- which from where they were
	 * standing is exactly what it was. A setting that is quietly
	 * overruled is worse than one that is refused, because there is
	 * nothing to argue with.
	 *
	 * Shown only when the two differ, so the row is silent in the case
	 * where it would be saying nothing.
	 */
	m_keyboard_fallback = bssh_wrapped_note(QString(), this);
	if (on_this_page(SECTION_KEYBOARD, m_keyboard_fallback)) form->addRow(m_keyboard_fallback);
	m_keyboard_fallback->hide();

	connect(m_keyboard, QOverload<int>::of(&QComboBox::currentIndexChanged),
	         this, &bssh_app_settings_view::refresh_keyboard_fallback);

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
	if (on_this_page(SECTION_KEYBOARD, note)) form->addRow(note);

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
	bssh_fit_items(m_theme);
	if (on_this_page(SECTION_APPEARANCE, m_theme)) form->addRow(QStringLiteral("Appearance"), m_theme);

	/*
	 * Where brightness and contrast go, built further down: straight
	 * under the theme, the first things on the page rather than below
	 * the fold. Looked for and not found was the report (2026-10-10).
	 */
	int lighting_row = form->rowCount();

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
	bssh_fit_items(m_scrollbar);
	if (on_this_page(SECTION_APPEARANCE, m_scrollbar)) form->addRow(QStringLiteral("Scrollbar"), m_scrollbar);

	QLabel *scrollbar_note = bssh_wrapped_note(
	        QStringLiteral("Automatic shows one where there is a pointer to drag it "
	                        "with. How much there is to scroll through is the host's "
	                        "own setting."),
	        this);
	if (on_this_page(SECTION_APPEARANCE, scrollbar_note)) form->addRow(scrollbar_note);

	/*
	 * How terminals occupy the window (sec 8.7's ARRANGEMENT). The idiom
	 * picks the default and then lets go, which that section asked for in
	 * as many words and nothing implemented: the window chose once from
	 * is_touch_primary() and offered no way back, so a desktop could not
	 * be shown the column at all except from a test fixture.
	 */
	m_arrangement = new QComboBox(this);
	m_arrangement->setObjectName(QStringLiteral("arrangement"));
	m_arrangement->addItem(QStringLiteral("Automatic"),
	                        int(bssh_app_settings::ARRANGEMENT_AUTO));
	m_arrangement->addItem(QStringLiteral("Side by side"),
	                        int(bssh_app_settings::ARRANGEMENT_TILED));
	m_arrangement->addItem(QStringLiteral("Stacked"),
	                        int(bssh_app_settings::ARRANGEMENT_COLUMN));
	bssh_fit_items(m_arrangement);
	if (on_this_page(SECTION_APPEARANCE, m_arrangement))
		form->addRow(QStringLiteral("Terminals"), m_arrangement);

	QLabel *arrangement_note = bssh_wrapped_note(
	        QStringLiteral("Automatic follows the device: side by side on a desktop "
	                        "or an unfolded screen, and stacked on a phone -- one "
	                        "on screen held sideways, two held upright. Stacked "
	                        "holds every session and scrolls."),
	        this);
	if (on_this_page(SECTION_APPEARANCE, arrangement_note)) form->addRow(arrangement_note);

	/*
	 * Which side the tab bar is on when terminals are side by side
	 * (sec 8.7). Stacked has no tab bar -- every session is a pane there --
	 * so this names the arrangement it applies to rather than offering a
	 * choice that would do nothing.
	 */
	m_tab_position = new QComboBox(this);
	m_tab_position->setObjectName(QStringLiteral("tab_position"));
	m_tab_position->addItem(QStringLiteral("Top"), int(bssh_app_settings::TABS_TOP));
	m_tab_position->addItem(QStringLiteral("Bottom"), int(bssh_app_settings::TABS_BOTTOM));
	m_tab_position->addItem(QStringLiteral("Left"), int(bssh_app_settings::TABS_LEFT));
	m_tab_position->addItem(QStringLiteral("Right"), int(bssh_app_settings::TABS_RIGHT));
	/* None: a pane's tabs are then in its corner (sec 8.7a). */
	m_tab_position->addItem(QStringLiteral("Hidden"), int(bssh_app_settings::TABS_HIDDEN));
	bssh_fit_items(m_tab_position);
	if (on_this_page(SECTION_APPEARANCE, m_tab_position))
		form->addRow(QStringLiteral("Tabs, side by side"), m_tab_position);

	/*
	 * The desktop's menu bar (sec 8.7a). Hidden, its menus are behind the
	 * focused pane's corner, so nothing it reached becomes unreachable. A
	 * phone has no menu bar to hide.
	 */
	m_menu_bar_shown = new QCheckBox(QStringLiteral("Show the menu bar"), this);
	m_menu_bar_shown->setObjectName(QStringLiteral("menu_bar_shown"));
	m_menu_bar_shown->setToolTip(
	        QStringLiteral("Hidden, its menus are in the corner of the terminal you are using."));
	if (!touch && on_this_page(SECTION_APPEARANCE, m_menu_bar_shown))
		form->addRow(m_menu_bar_shown);
	else
		m_menu_bar_shown->hide();

	/*
	 * How far the stacked divider is pulled onto the halfway line (sec
	 * 8.7 asks for the radius to be configurable). A slider rather than a
	 * spin box for the reason the brightness one is: a thumb cannot hit a
	 * spin box's arrows, and this is a touch surface first.
	 *
	 * Zero is the end somebody actually comes here for -- it turns the
	 * magnet off -- so the range starts there rather than at sec 8.7's
	 * suggested five.
	 */
	QWidget *snap_row = new QWidget(this);
	QHBoxLayout *snap_layout = new QHBoxLayout(snap_row);
	snap_layout->setContentsMargins(0, 0, 0, 0);

	m_snap_radius = new QSlider(Qt::Horizontal, snap_row);
	m_snap_radius->setObjectName(QStringLiteral("snap_radius"));
	/* Named for a reader: a slider in a box layout gets no buddy. */
	m_snap_radius->setAccessibleName(QStringLiteral("Divider snap"));
	m_snap_radius->setRange(0, BSSH_MAX_COLUMN_SNAP_RADIUS);
	m_snap_radius->setSingleStep(1);
	m_snap_radius->setPageStep(4);
	m_snap_radius->setToolTip(
	        QStringLiteral("How close to half the divider between stacked terminals "
	                        "has to be before it settles there. Zero turns it off."));

	m_snap_radius_value = new QLabel(snap_row);
	m_snap_radius_value->setMinimumWidth(
	        m_snap_radius_value->fontMetrics().horizontalAdvance(QStringLiteral("00 px")));

	snap_layout->addWidget(m_snap_radius, 1);
	snap_layout->addWidget(m_snap_radius_value);

	if (on_this_page(SECTION_APPEARANCE, snap_row))
		form->addRow(QStringLiteral("Divider snap"), snap_row);

	QLabel *snap_note = bssh_wrapped_note(
	        QStringLiteral("Only stacked terminals have a divider to drag. Zero means "
	                        "it settles wherever you leave it."),
	        this);
	if (on_this_page(SECTION_APPEARANCE, snap_note)) form->addRow(snap_note);

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
	bssh_fit_items(m_status_line);
	if (on_this_page(SECTION_APPEARANCE, m_status_line)) form->addRow(QStringLiteral("Status line"), m_status_line);
	if (on_this_page(SECTION_APPEARANCE, status_note)) form->addRow(status_note);

	/*
	 * The window title, as a format. A list of three canned choices would
	 * have been less code and would have fitted nobody: everyone wants a
	 * different one of the same four facts, which is why Konsole, xterm
	 * and tmux all ended up with a format string.
	 */
	m_window_title = new QLineEdit(this);
	m_window_title->setPlaceholderText(QStringLiteral("%t - BeerSSH"));
	if (on_this_page(SECTION_APPEARANCE, m_window_title)) form->addRow(QStringLiteral("Window title"), m_window_title);

	QLabel *title_note = bssh_wrapped_note(
	        QStringLiteral("%t is what the session calls itself, %n its saved name, "
	                        "%u the user, %h the host, %p the port, %a this app. "
	                        "Empty means the default."),
	        this);
	if (on_this_page(SECTION_APPEARANCE, title_note)) form->addRow(title_note);

	m_honour_font_scale = new QCheckBox(
	        QStringLiteral("Follow the system font size"), this);
	m_honour_font_scale->setObjectName(QStringLiteral("honour_font_scale"));
	m_honour_font_scale->setToolTip(
	        QStringLiteral("Scale menus, forms and lists with the device's "
	                        "text-size setting. The terminal itself keeps its "
	                        "own font, so its column count does not change."));
	if (on_this_page(SECTION_APPEARANCE, m_honour_font_scale))
		form->addRow(m_honour_font_scale);

	m_font_ligatures = new QCheckBox(
	        QStringLiteral("Font ligatures in the terminal"), this);
	m_font_ligatures->setObjectName(QStringLiteral("font_ligatures"));
	m_font_ligatures->setToolTip(
	        QStringLiteral("Draw ligatures like a combined arrow where the font "
	                        "has them. Off by default: the grid still counts the "
	                        "separate characters, so a selection can land mid-glyph."));
	if (on_this_page(SECTION_APPEARANCE, m_font_ligatures))
		form->addRow(m_font_ligatures);

	m_honour_remote_cursor = new QCheckBox(
	        QStringLiteral("Let programs change the cursor shape"), this);
	m_honour_remote_cursor->setObjectName(QStringLiteral("honour_remote_cursor"));
	m_honour_remote_cursor->setToolTip(
	        QStringLiteral("Follow a program's DECSCUSR request -- how vim shows a "
	                        "bar in insert mode and a block in normal. Off keeps the "
	                        "preset's cursor whatever the host asks for."));
	if (on_this_page(SECTION_APPEARANCE, m_honour_remote_cursor))
		form->addRow(m_honour_remote_cursor);

	m_context_menu = new QComboBox(this);
	m_context_menu->addItem(QStringLiteral("Automatic"),
	                         int(bssh_app_settings::CONTEXT_MENU_AUTO));
	m_context_menu->addItem(QStringLiteral("Pointer-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_DESKTOP));
	m_context_menu->addItem(QStringLiteral("Touch-sized"),
	                         int(bssh_app_settings::CONTEXT_MENU_TOUCH));
	bssh_fit_items(m_context_menu);
	if (on_this_page(SECTION_TERMINAL, m_context_menu)) form->addRow(QStringLiteral("Terminal menu"), m_context_menu);

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
		if (on_this_page(SECTION_TERMINAL, m_hide_from_screenshots)) form->addRow(m_hide_from_screenshots);
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
		if (on_this_page(SECTION_TERMINAL, m_focus_follows_mouse)) form->addRow(m_focus_follows_mouse);
	}

	/*
	 * sec 6.4's multi-line paste warning, which that section asked for
	 * "by default (a setting)" and which nothing had built -- so a paste
	 * carrying five commands ran all five.
	 */
	m_warn_multiline_paste = new QCheckBox(
	        QStringLiteral("Ask before pasting more than one line"), this);
	if (on_this_page(SECTION_TERMINAL, m_warn_multiline_paste)) form->addRow(m_warn_multiline_paste);

	QLabel *paste_note = bssh_wrapped_note(
	        QStringLiteral("A newline in a paste is Enter to the shell, so every line "
	                        "but the last runs before you can read it."),
	        this);
	if (on_this_page(SECTION_TERMINAL, paste_note)) form->addRow(paste_note);

	/* sec 5.5: what closing a window does to the sessions it shows. */
	m_keep_sessions_on_close = new QCheckBox(
	        QStringLiteral("Ask to keep sessions running on close"), this);
	if (on_this_page(SECTION_TERMINAL, m_keep_sessions_on_close))
		form->addRow(m_keep_sessions_on_close);

	QLabel *close_note = bssh_wrapped_note(
	        QStringLiteral("Off, closing a window closes its sessions. On, it asks, and "
	                        "can hide the window with its sessions running, to bring back "
	                        "from the tray."),
	        this);
	if (on_this_page(SECTION_TERMINAL, close_note)) form->addRow(close_note);

	/* sec 10.2a: tmux and screen sessions offered when a shell connects. */
	m_offer_multiplexers = new QCheckBox(QStringLiteral("Offer tmux and screen sessions"), this);
	if (on_this_page(SECTION_TERMINAL, m_offer_multiplexers)) form->addRow(m_offer_multiplexers);

	QLabel *multiplexer_note = bssh_wrapped_note(
	        QStringLiteral("When a shell connects, the tmux and screen sessions already "
	                        "running there are listed, and a menu asks which to open. "
	                        "Nothing is shown when there are none."),
	        this);
	if (on_this_page(SECTION_TERMINAL, multiplexer_note)) form->addRow(multiplexer_note);

	/*
	 * How much picture history each terminal keeps IN MEMORY (sec 7.1); past
	 * it pictures are spilled to disk. A number the user sets, per
	 * terminal, as the holder asked on 2026-09-26.
	 */
	m_image_history = new QSpinBox(this);
	m_image_history->setObjectName(QStringLiteral("image_history"));
	m_image_history->setRange(0, BSSH_MAX_IMAGE_HISTORY_MB);
	m_image_history->setSingleStep(8);
	m_image_history->setSuffix(QStringLiteral(" MB"));
	if (on_this_page(SECTION_TERMINAL, m_image_history))
		form->addRow(QStringLiteral("Picture history"), m_image_history);

	QLabel *image_note = bssh_wrapped_note(
	        QStringLiteral("Pictures a program draws stay in the terminal's scrollback. "
	                        "Past this much memory per terminal the oldest are kept on "
	                        "disk, encrypted, and read back when you scroll to them."),
	        this);
	if (on_this_page(SECTION_TERMINAL, image_note)) form->addRow(image_note);

	m_caps_as_ctrl = new QCheckBox(QStringLiteral("Caps Lock sends Ctrl"), this);
	m_caps_as_ctrl->setObjectName(QStringLiteral("caps_as_ctrl"));
	m_caps_as_ctrl->setToolTip(
	        QStringLiteral("Caps Lock arms Ctrl for the next key, in this terminal only. "
	                        "Your system still toggles its own caps lock -- no application "
	                        "can prevent that -- so a permanent swap belongs in your "
	                        "keyboard layout instead."));
	if (on_this_page(SECTION_KEYBOARD, m_caps_as_ctrl)) form->addRow(m_caps_as_ctrl);

	/*
	 * The built-in keyboard's resize bars (sec 6.2), and the way back
	 * from a size dragged too far. A bar is a thing that can be grabbed
	 * by accident at an edge, so it can be put away; the size it chose
	 * is kept whether or not the bars show.
	 */
	m_keyboard_grips = new QCheckBox(QStringLiteral("Drag bars to resize the keyboard"), this);
	m_keyboard_grips->setObjectName(QStringLiteral("keyboard_grips"));
	m_keyboard_grips->setToolTip(
	        QStringLiteral("A thin bar on the edge of BeerSSH's own keyboard, facing the "
	                        "terminal. Drag it to make the keys bigger or smaller; the size "
	                        "is kept for each keyboard."));
	if (on_this_page(SECTION_KEYBOARD, m_keyboard_grips)) form->addRow(m_keyboard_grips);

	m_keyboard_reset = new QPushButton(QStringLiteral("Reset keyboard size"), this);
	m_keyboard_reset->setObjectName(QStringLiteral("keyboard_reset"));
	connect(m_keyboard_reset, &QPushButton::clicked, this, [this] {
		commit([](bssh_app_settings &s) { s.reset_keyboard_size(); });
	});
	QWidget *reset_row = new QWidget(this);
	QHBoxLayout *reset_layout = new QHBoxLayout(reset_row);
	reset_layout->setContentsMargins(0, 0, 0, 0);
	reset_layout->addWidget(m_keyboard_reset);
	reset_layout->addStretch(1);
	if (on_this_page(SECTION_KEYBOARD, reset_row)) form->addRow(reset_row);

	m_raise_contrast = new QCheckBox(QStringLiteral("Raise low-contrast colours"), this);
	m_raise_contrast->setToolTip(
	        QStringLiteral("Colour schemes are reproduced exactly, and some of them are hard "
	                        "to read. This lifts a colour only while drawing it, and only when "
	                        "it falls below the readable minimum."));
	if (on_this_page(SECTION_APPEARANCE, m_raise_contrast)) form->insertRow(lighting_row++, m_raise_contrast);

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
	/*
	 * Named for a reader, because nothing else names it. sec 14's M11
	 * found four controls anonymous on the desktop Session page for
	 * exactly this reason -- a label and a field side by side in a box
	 * layout, where QFormLayout::addRow would have made the label a buddy
	 * and done it for free. The tooltip beside it is not an answer: a
	 * reader announces the NAME, and a slider that says nothing is one a
	 * blind user cannot tell from the next slider along.
	 */
	m_text_brightness->setAccessibleName(QStringLiteral("Text brightness"));
	m_text_brightness->setRange(BSSH_TEXT_BRIGHTNESS_MIN, 100);
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
	if (on_this_page(SECTION_APPEARANCE, brightness_row)) {
		form->insertRow(lighting_row++, QStringLiteral("Text brightness"), brightness_row);
	}

	/* Until the app closes: sec 9.3, the holder's rule of 2026-10-09. */
	QLabel *brightness_note = bssh_wrapped_note(
	        QStringLiteral("For reading at night. Applies to every terminal on this "
	                        "device until BeerSSH closes; it starts at full brightness "
	                        "and contrast every time."),
	        this);
	if (on_this_page(SECTION_APPEARANCE, brightness_note)) form->insertRow(lighting_row++, brightness_note);

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

	if (on_this_page(SECTION_APPEARANCE, colours_row)) form->addRow(colours_row);

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
	 */
	/*
	 * Built empty and filled on first show (fill_store_notes): which store
	 * this is is the one question here that can take seconds to answer.
	 */
	m_store_name = bssh_wrapped_note(QString(), this);
	m_store_name->setTextInteractionFlags(Qt::TextSelectableByMouse);

	/*
	 * A wrapped label in a form's field column is clipped rather
	 * than grown -- the layout asks it for a height before it knows
	 * the width it will get. On a phone that cut this one off at
	 * "passphrase-encrypted file (no", and losing the end of that
	 * particular label changes what it says.
	 */
	m_store_name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
	/*
	 * No row label on its own page: there the heading already says
	 * "Stored passwords", and this is the only row, so the words
	 * appeared twice one line apart. The label earns its place on the
	 * desktop, where the row sits among everything else in the "This
	 * device" category and has to say which setting it is.
	 */
	if (on_this_page(SECTION_PASSWORDS, m_store_name)) {
		if (m_section == SECTION_PASSWORDS) form->addRow(m_store_name);
		else form->addRow(QStringLiteral("Stored passwords"), m_store_name);
	}

	m_store_detail = bssh_wrapped_note(QString(), this);
	if (on_this_page(SECTION_PASSWORDS, m_store_detail)) form->addRow(m_store_detail);

	/*
	 * Not added at all when this page has no rows in it -- a
	 * shortcuts-only page would otherwise carry an empty form and the
	 * spacing that goes with it.
	 */
	if (form->rowCount() > 0) body->addLayout(form);
	else delete form;

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
	if (whole() || m_section == SECTION_SHORTCUTS) {
		/*
		 * On a section page the page's own heading already says
		 * "Shortcuts", so this one would be a second title for the same
		 * block -- rendered, the page read "Shortcuts" and then "Keys
		 * this app keeps" directly under it.
		 *
		 * Kept on the desktop, where the view is the whole "This device"
		 * category and this is what separates the chords from everything
		 * above them.
		 */
		if (whole()) {
			QLabel *chords_heading = new QLabel(
			        QStringLiteral("<b>Keys this app keeps</b>"), this);
			body->addWidget(chords_heading);
		}

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

		/*
		 * The label goes ABOVE its field when the row will not fit beside it,
		 * which on this form is most of them on a narrow phone.
		 *
		 * Every shortcut's description is its row label -- "Open another
		 * session in a window" is 195px of the 306 a 320dp screen leaves
		 * inside the scroll area -- so beside a QKeySequenceEdit the row
		 * demanded 313px and the whole page scrolled sideways. Measured on a
		 * Galaxy Z Fold 3 cover screen, which is 320dp folded.
		 *
		 * WrapLongRows rather than WrapAllRows: it wraps only the rows that
		 * do not fit, so a desktop keeps the two-column form it has always
		 * had and the phone gets a readable one. Shortening the descriptions
		 * would have been the other way to reach the number, and it is the
		 * wrong way -- the description is the only thing on the row that says
		 * what the chord DOES.
		 */
		chords->setRowWrapPolicy(QFormLayout::WrapLongRows);

		const bssh_app_settings bound;

		for (const bssh_input_router::chord_t &chord : bssh_input_router::default_chords()) {
			const QString name = QString::fromLatin1(chord.name);

			QKeySequenceEdit *edit = new QKeySequenceEdit(this);
			edit->setObjectName(QStringLiteral("chord_") + name);

			/*
			 * Wide enough for the chord it holds, and that is what makes
			 * WrapLongRows above work at all.
			 *
			 * A QKeySequenceEdit's minimum lets it shrink below its own
			 * text, so on a 320dp screen the form kept both columns, gave
			 * the field whatever was left, and drew "trl+Shift+C" and
			 * "ft+PgDown". The width assertion passed throughout -- it asks
			 * for a minimum that FITS, and a field happy to clip has a small
			 * one. `make shots` at 320 is what found it, which is the whole
			 * argument for rendering the pictures at the same floor the
			 * assertions use.
			 *
			 * So the field states what it actually needs, the row no longer
			 * fits beside its label, and WrapLongRows puts the label above
			 * it -- which is the outcome the policy was chosen for.
			 */
			const QString shown = QKeySequence(chord.label, QKeySequence::PortableText)
			                              .toString(QKeySequence::NativeText);
			edit->setMinimumWidth(edit->fontMetrics().horizontalAdvance(shown)
			                       + edit->fontMetrics().horizontalAdvance(QLatin1Char('W')) * 2);
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

		/*
		 * General key remapping (sec 6.1): send a different key for one you
		 * press. A fixed set of slots rather than an add/remove list --
		 * enough for the handful a person keeps, and no dynamic table to
		 * make fit a phone. Each row is two QKeySequenceEdits, and
		 * WrapLongRows stacks the second under the first where they do not
		 * fit beside each other, the same way the chords above wrap.
		 */
		if (whole()) {
			QLabel *remap_heading = new QLabel(
			        QStringLiteral("<b>Remap keys</b>"), this);
			body->addWidget(remap_heading);
		}

		QLabel *remap_note = bssh_wrapped_note(
		        QStringLiteral("Send a different key for one you press -- a "
		                        "special key such as Escape, an arrow, or a "
		                        "function or navigation key. Leave a row blank to "
		                        "use nothing."),
		        this);
		body->addWidget(remap_note);

		QFormLayout *remaps = new QFormLayout;
		remaps->setRowWrapPolicy(QFormLayout::WrapLongRows);

		const bssh_app_settings remap_store;
		const QList<QPair<QString, QString>> rules = remap_store.key_remaps();

		m_remap_from.clear();
		m_remap_to.clear();
		for (int i = 0; i < bssh_app_settings::BSSH_KEY_REMAP_SLOTS; ++i) {
			QKeySequenceEdit *from = new QKeySequenceEdit(this);
			from->setObjectName(QStringLiteral("remap_from_%1").arg(i));
			from->setKeySequence(
			        QKeySequence(rules.value(i).first, QKeySequence::PortableText));

			QKeySequenceEdit *to = new QKeySequenceEdit(this);
			to->setObjectName(QStringLiteral("remap_to_%1").arg(i));
			to->setKeySequence(
			        QKeySequence(rules.value(i).second, QKeySequence::PortableText));

			/*
			 * States its own width so WrapLongRows can see the row does not
			 * fit, exactly as the chord edits do -- a QKeySequenceEdit's own
			 * minimum lets it clip its text otherwise.
			 */
			const int w = from->fontMetrics().horizontalAdvance(
			                      QStringLiteral("Ctrl+Shift+Left"))
			              + from->fontMetrics().horizontalAdvance(QLatin1Char('W')) * 2;
			from->setMinimumWidth(w);
			to->setMinimumWidth(w);

			connect(from, &QKeySequenceEdit::editingFinished, this,
			         &bssh_app_settings_view::save_key_remaps);
			connect(to, &QKeySequenceEdit::editingFinished, this,
			         &bssh_app_settings_view::save_key_remaps);

			m_remap_from.append(from);
			m_remap_to.append(to);
			remaps->addRow(from, to);
		}
		body->addLayout(remaps);
	}

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
	m_keyboard_grips->setChecked(settings.keyboard_grips());
	m_warn_multiline_paste->setChecked(settings.warn_multiline_paste());
	m_keep_sessions_on_close->setChecked(settings.keep_sessions_on_close());
	m_offer_multiplexers->setChecked(settings.offer_multiplexer_sessions());
	m_honour_font_scale->setChecked(settings.honour_system_font_scale());
	m_font_ligatures->setChecked(settings.font_ligatures());
	m_menu_bar_shown->setChecked(settings.menu_bar_shown());
	m_honour_remote_cursor->setChecked(settings.honour_remote_cursor_shape());
	m_raise_contrast->setChecked(settings.raise_contrast());
	m_text_brightness->setValue(settings.text_brightness());
	m_text_brightness_value->setText(QStringLiteral("%1%").arg(settings.text_brightness()));

	const int bar_index = m_scrollbar->findData(int(settings.scrollbar()));
	m_scrollbar->setCurrentIndex(bar_index < 0 ? 0 : bar_index);

	const int arrangement_index = m_arrangement->findData(int(settings.arrangement()));
	m_arrangement->setCurrentIndex(arrangement_index < 0 ? 0 : arrangement_index);
	m_image_history->setValue(settings.image_history_mb());
	const int tabs_index = m_tab_position->findData(int(settings.tab_position()));
	m_tab_position->setCurrentIndex(tabs_index < 0 ? 0 : tabs_index);
	m_snap_radius->setValue(settings.column_snap_radius());
	m_snap_radius_value->setText(QStringLiteral("%1 px").arg(settings.column_snap_radius()));

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

	const int layout_index = m_keyboard_layout->findData(settings.keyboard_layout());
	m_keyboard_layout->setCurrentIndex(layout_index < 0 ? 0 : layout_index);

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
	connect(m_keyboard_layout, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_keyboard_layout(m_keyboard_layout->currentData().toString());
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
	connect(m_arrangement, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_arrangement(bssh_app_settings::arrangement_t(
			        m_arrangement->currentData().toInt()));
		});
	});
	connect(m_image_history, &QSpinBox::valueChanged, this, [this](int value) {
		commit([value](bssh_app_settings &s) { s.set_image_history_mb(value); });
	});
	connect(m_tab_position, &QComboBox::currentIndexChanged, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_tab_position(bssh_app_settings::tab_position_t(
			        m_tab_position->currentData().toInt()));
		});
	});
	connect(m_snap_radius, &QSlider::valueChanged, this, [this](int value) {
		m_snap_radius_value->setText(QStringLiteral("%1 px").arg(value));
		commit([value](bssh_app_settings &s) { s.set_column_snap_radius(value); });
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
	connect(m_keyboard_grips, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_keyboard_grips(m_keyboard_grips->isChecked()); });
	});
	connect(m_caps_as_ctrl, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_caps_as_ctrl(m_caps_as_ctrl->isChecked()); });
	});
	connect(m_warn_multiline_paste, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_warn_multiline_paste(m_warn_multiline_paste->isChecked());
		});
	});
	connect(m_keep_sessions_on_close, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_keep_sessions_on_close(m_keep_sessions_on_close->isChecked());
		});
	});
	connect(m_offer_multiplexers, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_offer_multiplexer_sessions(m_offer_multiplexers->isChecked());
		});
	});
	connect(m_honour_font_scale, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_honour_system_font_scale(m_honour_font_scale->isChecked());
		});
	});
	connect(m_menu_bar_shown, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) { s.set_menu_bar_shown(m_menu_bar_shown->isChecked()); });
	});
	connect(m_font_ligatures, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_font_ligatures(m_font_ligatures->isChecked());
		});
	});
	connect(m_honour_remote_cursor, &QCheckBox::toggled, this, [this] {
		commit([this](bssh_app_settings &s) {
			s.set_honour_remote_cursor_shape(m_honour_remote_cursor->isChecked());
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

void bssh_app_settings_view::save_key_remaps() {
	QList<QPair<QString, QString>> remaps;
	for (int i = 0; i < bssh_app_settings::BSSH_KEY_REMAP_SLOTS; ++i) {
		const QString from = m_remap_from.value(i)
		        ? m_remap_from.at(i)->keySequence().toString(QKeySequence::PortableText)
		        : QString();
		const QString to = m_remap_to.value(i)
		        ? m_remap_to.at(i)->keySequence().toString(QKeySequence::PortableText)
		        : QString();
		remaps.append(qMakePair(from, to));
	}
	commit([&remaps](bssh_app_settings &s) { s.set_key_remaps(remaps); });
}

/*
 * The fallback note, kept in step with the choice and with the width.
 *
 * Asks sk_keyboard_style_for() rather than repeating its threshold: a
 * second copy of that number would be a second thing to be wrong, and the
 * one that decides what is DRAWN is the one that must be reported.
 */
void bssh_app_settings_view::refresh_keyboard_fallback() {
	if (!m_keyboard || !m_keyboard_fallback) return;

	/*
	 * NOT ON A PAGE THIS NOTE IS NOT ON, which is not the same as "not
	 * built" (sec 8.5).
	 *
	 * on_this_page() hides the widgets a section page leaves out, and
	 * hiding is all it can do -- they are still children of this view,
	 * just not in its form. This slot ends by SHOWING the label, so on
	 * any other section it put the note back, unlaid-out, at the
	 * top-left corner: rendered on the device, "This screen is too
	 * narrow for the full-size keyboard" was drawn across the Shortcuts
	 * heading.
	 *
	 * It needs the device's own combination to appear at all -- the
	 * built-in full keyboard chosen AND a screen too narrow to draw it
	 * -- which is why every assertion stayed green and only opening the
	 * page on a phone showed it.
	 */
	if (!whole() && m_section != SECTION_KEYBOARD) return;

	const bssh_app_settings::keyboard_t chosen =
	        bssh_app_settings::keyboard_t(m_keyboard->currentData().toInt());
	sk_catalog::style_t wanted;
	if (chosen == bssh_app_settings::KEYBOARD_BUILT_IN) {
		wanted = sk_catalog::STYLE_COMPACT;
	} else if (chosen == bssh_app_settings::KEYBOARD_BUILT_IN_FULL) {
		wanted = sk_catalog::STYLE_FULL;
	} else {
		m_keyboard_fallback->hide();
		return;
	}

	/*
	 * The WINDOW's size, which is what the keyboard is drawn for: the
	 * split question compares width with height, and this page's own
	 * height is the window's less whatever sits above it.
	 */
	const QWidget *frame = window();
	const sk_catalog::style_t drawn =
	        sk_keyboard_style_for(wanted, frame->width(), frame->height());
	if (drawn == wanted) {
		m_keyboard_fallback->hide();
		return;
	}

	if (drawn == sk_catalog::STYLE_SPLIT) {
		m_keyboard_fallback->setText(QStringLiteral(
		        "This screen is short and wide, so the keyboard is split either "
		        "side of the terminal. Rotate back and the keyboard you chose "
		        "returns; the choice is kept either way."));
		m_keyboard_fallback->show();
		return;
	}

	m_keyboard_fallback->setText(QStringLiteral(
	        "This screen is too narrow for the full-size keyboard, so the compact "
	        "one is shown here. Unfold or rotate to a wider screen and the "
	        "full-size keyboard appears; the choice is kept either way."));
	m_keyboard_fallback->show();
}

/*
 * The note is a function of the choice AND the width, so a resize has to
 * re-ask it. Without this, unfolding a phone leaves "this screen is too
 * narrow" on screen next to a keyboard that is no longer falling back --
 * a stale explanation being worse than none, since it describes a state
 * the user can see is not true.
 */
void bssh_app_settings_view::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	refresh_keyboard_fallback();
}

void bssh_app_settings_view::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);
	fill_store_notes();
}

void bssh_app_settings_view::fill_store_notes() {
	if (m_store_filled || !m_store_name || !m_store_detail) return;
	m_store_filled = true;

	/*
	 * Read once: the backend is chosen the first time anything asks and
	 * does not change under a running app.
	 */
	const bssh_secret_store *store = bssh_secret_store::instance();
	if (!store) return;

	m_store_name->setText(store->backend_name());
	m_store_name->setMinimumHeight(m_store_name->sizeHint().height());

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

	m_store_detail->setText(detail);
}
