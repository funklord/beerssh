#include <QRegularExpression>
#include <QScrollEvent>
#include <QScrollPrepareEvent>
#include "ui/touch_metrics.h"
#include "ui/terminal_view.h"
#include "ui/theme.h"
#include "ui/layout_manager.h"

#include <QDragEnterEvent>
#include <QEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QUrl>

#include "ui/terminal_accessible.h"

#include <QAccessible>

#include <QGesture>
#include <QGestureEvent>
#include <QFontDatabase>
#include <QPen>
#include <QFontMetricsF>

#include <algorithm>
#include <QContextMenuEvent>
#include <QPainter>
#include <QTimer>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>

#include "term/emulator.h"
#include <QResizeEvent>

#include "input/input_router.h"
#include "platform/platform.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "model/preset.h"

/*
 * Matches bssh_preset_t's default, so a view built before any settings are
 * resolved is already showing the size a session will resolve to rather
 * than jumping one point the moment it connects.
 */
static const int BSSH_DEFAULT_POINT_SIZE = 11;

/*
 * Half a second on, half off -- the rate every other terminal uses, and
 * the one people read as "here is the cursor" rather than as a fault.
 */
static const int BSSH_CURSOR_BLINK_MS = 500;

/*
 * Space kept clear at the edges for the focused-pane border (sec 8.3).
 * Without it the border is drawn over the first column and the last, and
 * the leftmost pixel of every line in the focused pane is clipped -- which
 * looks like a rendering fault in the pane the user is working in, the one
 * place they are most likely to notice.
 */
static const int BSSH_GRID_INSET_PX = 1;

/*
 * The placeholder drawn into the empty grid. It exists to prove the cell
 * metrics are right -- it is painted one cell at a time, exactly the way
 * the emulator's row renderer will paint at M2, so a wrong advance width
 * shows up as visibly uneven spacing rather than as a subtle bug later.
 */
static const char *const BSSH_PLACEHOLDER = "BeerSSH -- no session (M0 skeleton)";

bssh_terminal_view::bssh_terminal_view(QWidget *parent)
    : QWidget(parent),
      m_dragging_handle(-1),
      m_screen(nullptr),
      m_images(nullptr),
      m_scrollback(nullptr),
      m_reported_button(0),
      m_scroll_offset(0),
      m_scrollbar(nullptr),
      m_scrollbar_position(default_scrollbar_position()),
      m_focus_border(false),
      m_being_dragged(false),
      m_drop_edge(-1),
      m_pointer_inside(false),
      m_pane_handle(false),
      m_click_count(0),
      m_last_click_line(-1),
      m_selecting(false),
      m_long_press(nullptr),
      m_selection_anchor_line(-1),
      m_selection_anchor_col(0),
      m_selection_focus_line(-1),
      m_selection_focus_col(0),
      m_current_match(-1),
      m_emulator(nullptr),
      m_input_router(nullptr),
      m_font_size(BSSH_DEFAULT_POINT_SIZE),
      m_font_zoom(0),
      m_pinch_scale(1.0),
      m_cursor_style(QStringLiteral("block")),
      m_cursor_on(true),
      m_cursor_blink_wanted(false),
      m_maybe_pane_drag(false),
      m_input_mode(BSSH_INPUT_SCROLL),
      m_scroll_dragging(false),
      m_scroll_drag_moved(false),
      m_scroll_drag_start_offset(0),
      m_move_dragging(false),
      m_move_drag_moved(false),
      m_colscroll_dragging(false),
      m_colscroll_moved(false),
      m_blink_timer(nullptr),
      m_background(QColor(0x1c, 0x1c, 0x1c)),
      m_foreground(QColor(0xd0, 0xd0, 0xd0)),
      m_reverse_video(false),
      m_content_revision(1),
      m_raise_contrast(true),
      m_text_brightness(100),
      m_focus_follows_mouse(false),
      m_cell_width(1),
      m_cell_height(1),
      m_baseline(0),
      m_cols(0),
      m_rows(0) {
	/*
	 * The default from the curated set (sec 9.2), by form factor: the
	 * condensed face on a touch-primary device, where the extra columns
	 * are the difference between a usable and an unusable width, and the
	 * legibility-first one everywhere else. Same set both ways -- only the
	 * starting choice differs, so a profile that names a font still means
	 * the same thing on either.
	 *
	 * The catalog degrades on its own when none of the set is installed,
	 * so this is safe before any font has been fetched or bundled.
	 */
	set_colour_scheme(QString());

	/*
	 * A host that never chose a scheme follows the app's appearance (sec
	 * 6.2): white on black while it is dark, black on white while light.
	 */
	connect(bssh_theme::instance(), &bssh_theme::appearance_applied, this, [this] {
		if (!m_scheme_follows_default) return;

		const QString now = bssh_colour_scheme_catalog::instance().default_id();
		if (now == m_scheme_id) return;

		m_scheme_id = now;
		reload_colour_scheme();
	});

	/*
	 * The scrollbar, built now and shown when asked for (sec 8.3). A
	 * child of this widget rather than a sibling, so the grid arithmetic
	 * that has to account for it lives with the code that does the
	 * arithmetic.
	 *
	 * Dragging it scrolls the view rather than the other way round: the
	 * offset is the view's, and scroll_by() is what already tells the
	 * emulator and the repaint about it.
	 */
	/*
	 * Files and text can be dropped here (sec 6.4). A session payload is
	 * refused, so a tab dragged from another window still reaches the
	 * window that knows how to adopt it (sec 8.4).
	 */
	setAcceptDrops(true);

	m_scrollbar = new QScrollBar(Qt::Vertical, this);
	m_scrollbar->setVisible(m_scrollbar_position != SCROLLBAR_NONE);

	connect(m_scrollbar, &QScrollBar::valueChanged, this, [this](int value) {
		const int history = m_scrollback ? m_scrollback->count() : 0;
		const int wanted = qBound(0, history - value, history);

		if (wanted == m_scroll_offset) return;
		scroll_by(wanted - m_scroll_offset);
	});

	/*
	 * Press-and-hold opens the menu where there is no right button to
	 * open it with. The gesture itself is bssh_long_press, shared with
	 * the host list, because Qt does not provide it and writing the timer
	 * out twice is the shape this project keeps unpicking.
	 */
	m_long_press = new bssh_long_press(this, this);
	connect(m_long_press, &bssh_long_press::triggered, this, [this](const QPoint &at) {
		/*
		 * NOT the guard the right button gets, and the difference is
		 * the gesture rather than the platform. A right-click is
		 * something the remote asked for and can act on, so it belongs
		 * to a program that grabbed the mouse. A LONG PRESS is not a
		 * thing the remote has any concept of -- no terminal protocol
		 * carries it -- so it cannot be the remote's to take, and
		 * suppressing it left a phone with no menu, no clipboard and no
		 * way to reach either (sec 6.4).
		 */

		/*
		 * And it SELECTS first, which is sec 6.4's touch model: "long
		 * press then drag handles". The menu that follows carries Copy,
		 * so the press that opens it also gives it something to copy --
		 * before this, Copy on a phone was enabled only if the user had
		 * already dragged out a selection, and a drag is the gesture
		 * scrolling needs.
		 *
		 * The word rather than a point, because a zero-width selection
		 * is not one and because a path is what people reach for in a
		 * terminal. Handles adjust it from there.
		 */
		qint64 line = 0;
		int col = 0;
		if (!has_selection() && cell_at(mapFromGlobal(at), line, col)) {
			select_word_at(line, col);
		}

		emit context_menu_requested(at);
	});

	m_font_id = bssh_font_catalog::instance().default_id();
	m_font = bssh_font_catalog::make_font(
	        bssh_font_catalog::instance().family_for(m_font_id), font_point_size(),
	        default_font_ligatures());

	setFocusPolicy(Qt::StrongFocus);

	/*
	 * Pinch-to-zoom (sec 9.5). Grabbed rather than recognised by hand:
	 * Qt's own recogniser wants two touch points and reports a scale,
	 * and a hand-rolled one would be a second answer to "what is a
	 * pinch" living in a terminal widget.
	 *
	 * It does not disturb the one-finger gestures sec 8.7's input
	 * selector routes. Those arrive as the mouse events Qt synthesises
	 * from the primary touch point, and the pinch recogniser needs a
	 * second point before it claims anything -- so a scroll, a select,
	 * a pane move and a column pan all reach mousePressEvent exactly as
	 * they did.
	 */
	grabGesture(Qt::PinchGesture);

	/*
	 * Assistive technology sees a terminal rather than an anonymous
	 * rectangle from here on (sec 14, M11). Installed from the view's own
	 * constructor rather than from main() so that a terminal built in a
	 * test is accessible too -- which is the only place this gets
	 * exercised without a screen reader attached.
	 */
	bssh_terminal_accessible::install();
	setAccessibleName(QStringLiteral("Terminal"));
	setMouseTracking(false);
	/* Without this Qt never routes input-method events here at all. */
	setAttribute(Qt::WA_InputMethodEnabled);
	setAttribute(Qt::WA_OpaquePaintEvent);
	update_metrics();

	/*
	 * Compute the grid up front rather than waiting for the first
	 * resizeEvent. Qt does not deliver one to a widget that has never
	 * been shown, so without this the view reports a 0x0 grid until it is
	 * mapped -- and anything that arrives in the meantime (an SSH banner
	 * beats the first paint comfortably) gets trimmed away against a row
	 * count of zero. The emission is a no-op here: nothing can be
	 * connected to a widget still inside its own constructor.
	 */
	recompute_grid();
}


void bssh_terminal_view::set_font_id(const QString &id, int point_size) {
	bssh_font_catalog &catalog = bssh_font_catalog::instance();

	/*
	 * Through effective_id, so this agrees with the picker and with
	 * whatever decides to record an override. An empty value means "the
	 * default", not "no font": every shipped preset leaves the font unset
	 * (sec 10.4), and treating that as a request would drop into the
	 * fallback chain and render every default host in DejaVu.
	 */
	m_font_id = catalog.effective_id(id);

	m_font_size = point_size > 0 ? point_size : BSSH_DEFAULT_POINT_SIZE;

	/*
	 * The zoom is deliberately NOT cleared here. This is the path a
	 * resolve takes -- a preset edit, a host reopened, a pane rebuilt --
	 * and a user who has made this session's text bigger has not asked
	 * for it back every time one of those happens. It is re-clamped
	 * against the new base instead, which is what rebuild_font does.
	 */
	rebuild_font();
}

int bssh_terminal_view::font_point_size() const {
	return qBound(BSSH_PRESET_MIN_POINT_SIZE, m_font_size + m_font_zoom,
	               BSSH_PRESET_MAX_POINT_SIZE);
}

void bssh_terminal_view::set_font_zoom(int steps) {
	if (steps == m_font_zoom) return;

	/*
	 * Stored as asked and clamped in ONE place, rebuild_font, which is
	 * the only code here that knows the base. Clamping in both would be
	 * two answers to one question, and the narrower of them would win
	 * silently.
	 */
	m_font_zoom = steps;
	rebuild_font();
}

void bssh_terminal_view::rebuild_font() {
	bssh_font_catalog &catalog = bssh_font_catalog::instance();

	/*
	 * The one clamp, and it is applied to the ZOOM rather than to the
	 * sum. Bounding only the sum leaves the stored steps climbing while
	 * the drawn size stands still, so the first few steps back do
	 * nothing -- which reads, from the keyboard, as the zoom having
	 * broken. It also re-clamps after a RESOLVE, since a new base can
	 * leave a zoom that was legal against the old one out of range.
	 */
	m_font_zoom = qBound(BSSH_PRESET_MIN_POINT_SIZE - m_font_size, m_font_zoom,
	                      BSSH_PRESET_MAX_POINT_SIZE - m_font_size);

	m_font = bssh_font_catalog::make_font(catalog.family_for(m_font_id),
	                                       font_point_size(),
	                                       default_font_ligatures());

	/*
	 * A different font is a different cell size, which is a different
	 * column count, which the remote end has to be told about -- so this
	 * goes all the way through to window-change (sec 9.5). Changing the
	 * font without that leaves the shell laying out for the old width.
	 */
	update_metrics();
	recompute_grid();
	update();
}

void bssh_terminal_view::remeasure_cell() {
	/*
	 * The same three steps a font change takes, and for the same reason:
	 * a different cell is a different column count, which the remote has
	 * to be told about.
	 */
	update_metrics();
	recompute_grid();
	update();
}

void bssh_terminal_view::set_colour_scheme(const QString &id) {
	m_scheme_follows_default = id.isEmpty();
	const QString wanted = bssh_colour_scheme_catalog::instance().effective_id(id);
	if (wanted == m_scheme_id) return;

	m_scheme_id = wanted;
	reload_colour_scheme();
}

void bssh_terminal_view::reload_colour_scheme() {
	const bssh_colour_scheme_catalog &catalog = bssh_colour_scheme_catalog::instance();

	/*
	 * An unknown scheme draws in the default rather than in nothing. The
	 * id is kept as asked for, so the picker can still show what the
	 * profile wanted and say it is not in this build (sec 10.1).
	 */
	const bssh_colour_scheme_t *scheme = catalog.by_id(m_scheme_id);
	if (!scheme) scheme = catalog.by_id(catalog.default_id());
	if (!scheme) return;

	m_scheme = *scheme;
	m_background = QColor(m_scheme.background);
	m_foreground = QColor(m_scheme.foreground);

	update();
}

void bssh_terminal_view::set_cursor_style(const QString &style) {
	/*
	 * Anything unrecognised is a block. The value comes from a config file
	 * a person can type into (sec 10.6), so "blok" has to render as
	 * something rather than as no cursor at all.
	 */
	const QString wanted = style.isEmpty() ? QStringLiteral("block") : style.toLower();
	if (wanted == m_cursor_style) return;

	m_cursor_style = wanted;
	update();
}

void bssh_terminal_view::set_cursor_blink(bool blink) {
	if (blink == m_cursor_blink_wanted) return;

	m_cursor_blink_wanted = blink;
	sync_blink_timer();
}

void bssh_terminal_view::set_reverse_video(bool reverse) {
	if (reverse == m_reverse_video) return;

	m_reverse_video = reverse;
	update();
}

void bssh_terminal_view::sync_blink_timer() {
	/*
	 * Only the FOCUSED terminal blinks (sec 10.2). Four panes each
	 * animating a cursor is four things moving on a screen where one of
	 * them is the one you are typing into -- the movement is meant to
	 * say "here", and it says nothing when everything says it. It is
	 * also four timers waking twice a second on a phone, which sec 5.1
	 * cares about for its own reasons.
	 */
	const bool running = m_cursor_blink_wanted && hasFocus();
	if (running == (m_blink_timer != nullptr)) return;

	if (!running) {
		delete m_blink_timer;
		m_blink_timer = nullptr;

		/*
		 * Left ON when blinking stops, never wherever the phase happened
		 * to be: a cursor that switches off and stays off looks like a
		 * session that has died.
		 */
		m_cursor_on = true;
		update();
		return;
	}

	m_blink_timer = new QTimer(this);
	m_blink_timer->setInterval(BSSH_CURSOR_BLINK_MS);
	connect(m_blink_timer, &QTimer::timeout, this, [this] {
		m_cursor_on = !m_cursor_on;

		/*
		 * Repaints the cursor cell rather than the whole grid. A full
		 * repaint twice a second, per pane, on a phone, to animate one
		 * cell is exactly the kind of idle wakeup sec 5.1 cares about.
		 */
		if (!m_screen) return;
		/*
		 * In the same coordinates the cursor is drawn in. This is the
		 * only partial update in the class, so a region that did not
		 * match left a band of the cursor cell never repainted while
		 * it blinked -- twice a second, until a full update came.
		 */
		update(grid_origin_x() + m_screen->cursor_col() * m_cell_width,
		        BSSH_GRID_INSET_PX + m_screen->cursor_row() * m_cell_height,
		        m_cell_width, m_cell_height);
	});
	m_blink_timer->start();
}

QString bssh_terminal_view::font_substitution_note() const {
	return bssh_font_catalog::instance().substitution_note(m_font_id);
}

const QFont &bssh_terminal_view::cell_font(const QString &family, bool bold, bool italic) const {
	/*
	 * The key carries the attributes because a bold face is a different
	 * font object, and the family because the fallback chain can change it
	 * from one cell to the next -- a line of box drawing followed by CJK
	 * followed by ASCII is three families in one row.
	 */
	const QString key = family + (bold ? QLatin1Char('B') : QLatin1Char('b'))
	                           + (italic ? QLatin1Char('I') : QLatin1Char('i'));

	const auto cached = m_font_cache.constFind(key);
	if (cached != m_font_cache.constEnd()) return cached.value();

	QFont font = bssh_font_catalog::make_font(family, font_point_size(),
	                                          default_font_ligatures());
	font.setBold(bold);
	font.setItalic(italic);

	/*
	 * Forced onto the cell grid. Drawing a whole run in one call means Qt
	 * advances by the font's own metrics, which are fractional, while the
	 * cell is a whole number of device pixels (sec 9.5) -- so a run of
	 * eighty characters would drift steadily away from its columns.
	 * Absolute letter spacing pins each advance to exactly one cell, and
	 * is why runs can be drawn at all rather than one glyph at a time.
	 */
	const QFontMetricsF metrics(font);
	const qreal natural = metrics.horizontalAdvance(QLatin1Char('W'));
	font.setLetterSpacing(QFont::AbsoluteSpacing, m_cell_width - natural);

	return *m_font_cache.insert(key, font);
}

/*
 * Attaching a different screen, scrollback or image store is a content
 * change, and each of these bumps the revision to say so.
 *
 * It is the revision that invalidates the accessibility layer's cache of
 * the flattened text (terminal_accessible.cpp), and only
 * on_screen_damaged() used to move it. So a view pointed at another
 * session's screen went on reporting the first session's text to a
 * screen reader until something happened to damage the new one.
 *
 * NOT REACHABLE IN THE APP TODAY, and saying so is the point rather than
 * a hedge: bssh_pane_container::rebuild() constructs a fresh view for
 * every pane, so no view is ever re-attached and the cache is new each
 * time. The contract was therefore true by an accident of how panes are
 * rebuilt rather than by anything here -- which is the shape sec 12.4
 * already records for the scrollbar that tracked its history correctly
 * because two of three signal sites happened to damage the screen as a
 * side effect. A consumer that is right by coincidence looks exactly
 * like a wired one.
 *
 * Found by the sibling qtty asking its own question of a GATE rather
 * than a payload: its frame scheduler compared image COUNTS to decide
 * whether to send a frame, so a picture repainted in place was computed
 * correctly and never transmitted. This is the same fault in the same
 * position -- a change detector reading a property that does not change
 * when the content does.
 */
void bssh_terminal_view::attach_screen(const bssh_screen *screen) {
	if (m_screen != screen) ++m_content_revision;

	m_screen = screen;
	update();
}

void bssh_terminal_view::attach_images(const bssh_image_store *images) {
	if (m_images != images) ++m_content_revision;

	m_images = images;
	update();
}

void bssh_terminal_view::attach_scrollback(const bssh_scrollback *scrollback) {
	if (m_scrollback != scrollback) ++m_content_revision;

	m_scrollback = scrollback;
	update();
}

void bssh_terminal_view::attach_emulator(bssh_emulator *emulator) {
	if (m_emulator == emulator) return;

	disconnect(m_alternate_screen_connection);
	disconnect(m_scrollback_connection);
	m_emulator = emulator;

	/*
	 * Entering the alternate screen takes the history away (see
	 * available_history), so a view left scrolled back when a program
	 * starts has to come forward -- otherwise the shell stays on screen
	 * over the program that just took it.
	 *
	 * Wired here rather than left to whoever connects damaged(), because
	 * the rule is the view's own: a view attached by a test, or by any
	 * future caller that does not happen to wire damage, would otherwise
	 * keep an offset it is no longer entitled to.
	 */
	if (m_emulator) {
		m_alternate_screen_connection =
		        connect(m_emulator, &bssh_emulator::alternate_screen_changed,
		                 this, [this] { set_scroll_offset(m_scroll_offset); });

		/*
		 * And when the history itself changes size. This signal had no
		 * listener anywhere in the tree: pushing and popping happen to
		 * damage the screen, so on_screen_damaged() kept the bar honest
		 * by accident -- but set_scrollback_lines() trims the ring with
		 * no damage at all, so lowering a host's scrollback while the
		 * view was scrolled back left the bar advertising history that
		 * had been dropped, and the offset pointing past the end of what
		 * was left. Blank rows under a scrollbar claiming otherwise,
		 * until the next byte of output repaired it.
		 */
		m_scrollback_connection =
		        connect(m_emulator, &bssh_emulator::scrollback_changed, this, [this] {
			/*
			 * The same three things this used to do, plus the two
			 * it did not: the content revision and the reader's
			 * event. A trim changes what the terminal HAS, which
			 * is a content change whether or not a cell was
			 * damaged.
			 */
			drop_evicted_selection();
			drop_evicted_find_matches();
			note_content_changed();
		});
	}

	sync_scrollbar();
	update();
}

int bssh_terminal_view::available_history() const {
	/*
	 * The alternate screen has no history of its own -- libvterm pushes
	 * nothing off it -- so everything in the scrollback belongs to the
	 * shell UNDERNEATH the full-screen program. Showing it is not
	 * scrolling back; it is drawing the shell's output over vim's screen.
	 * The lines are kept rather than dropped: leaving the program gives
	 * them back, which is the whole point of the alternate screen.
	 */
	if (m_emulator && m_emulator->alternate_screen()) return 0;

	return m_scrollback ? m_scrollback->count() : 0;
}

void bssh_terminal_view::set_scroll_offset(int lines) {
	const int clamped = qBound(0, lines, available_history());

	if (clamped == m_scroll_offset) return;

	m_scroll_offset = clamped;

	/*
	 * The bar follows the view however the view was scrolled -- a wheel,
	 * Shift+PageUp, a click that jumped to live. A bar that only moved
	 * when dragged would sit still while the terminal scrolled under it.
	 */
	sync_scrollbar();
	update();
}

void bssh_terminal_view::scroll_by(int lines) {
	set_scroll_offset(m_scroll_offset + lines);
}

void bssh_terminal_view::set_focus_border(bool focused) {
	if (m_focus_border == focused) return;

	m_focus_border = focused;
	update();
}

void bssh_terminal_view::scroll_to_live() {
	set_scroll_offset(0);
}

bool bssh_terminal_view::event(QEvent *event) {
	/*
	 * Pinch-to-zoom (sec 9.5). Qt recognises the gesture from the touch
	 * points and delivers it here; there is no pinchEvent() to override.
	 *
	 * It emits the SAME signal Ctrl+wheel and the Text Size menu emit,
	 * so every question already settled about zooming stays settled: the
	 * session owns the number, the clamp lives in rebuild_font, and the
	 * host keeps it (sec 9.5). This adds an input route and decides
	 * nothing else.
	 */
	if (event->type() == QEvent::Gesture) {
		QGestureEvent *gestures = static_cast<QGestureEvent *>(event);
		if (QGesture *found = gestures->gesture(Qt::PinchGesture)) {
			QPinchGesture *pinch = static_cast<QPinchGesture *>(found);
			handle_pinch(pinch->scaleFactor());
			gestures->accept(Qt::PinchGesture);
			return true;
		}
	}

	return QWidget::event(event);
}

void bssh_terminal_view::handle_pinch(qreal change) {
	/*
	 * A step per 20% of scale, accumulated from the INCREMENTAL factor
	 * each update carries rather than from the gesture's total.
	 *
	 * That choice is what makes this testable at all, and it is not a
	 * compromise. Reading the total needs to know where each pinch
	 * BEGAN, and a gesture's state is set by Qt's recogniser through
	 * private friends -- no test can produce a GestureStarted, so the
	 * reset branch would have been the one thing here nothing could
	 * reach. Accumulating instead has no beginning to detect: a new
	 * pinch simply continues from whatever fraction of a step was left
	 * over, which is at most one step's worth and is what a user who
	 * pinches twice in the same direction would expect anyway.
	 *
	 * Steps rather than a continuous scale because the zoom is an
	 * integer point-size offset (sec 9.5): the grid is whole cells and
	 * there is no half a point to show. A continuous factor would have
	 * to be rounded to these same steps one layer further from the
	 * thing that owns them.
	 */
	static const qreal step = 1.2;

	if (change <= 0.0) return;
	if (m_pinch_scale <= 0.0) m_pinch_scale = 1.0;

	m_pinch_scale *= change;

	int steps = 0;
	while (m_pinch_scale >= step) {
		m_pinch_scale /= step;
		++steps;
	}
	while (m_pinch_scale <= 1.0 / step) {
		m_pinch_scale *= step;
		--steps;
	}

	if (steps != 0) emit zoom_requested(steps);
}

void bssh_terminal_view::wheelEvent(QWheelEvent *event) {
	const int notches = event->angleDelta().y() / 120;

	/*
	 * Ctrl+wheel zooms, and it is tested FIRST -- before the remote's
	 * claim on the wheel below -- because that is the one place a user
	 * reaches for it. Text too small to read is most often text inside
	 * less, vim or htop, and those are exactly the programs that have
	 * turned mouse reporting on; a zoom that worked everywhere except
	 * there would be missing at the moment it is wanted.
	 *
	 * It steals nothing from the shell. A wheel is not a key, the remote
	 * hears no zoom either way, and Ctrl+wheel is what every other
	 * terminal does with this gesture.
	 */
	if (event->modifiers().testFlag(Qt::ControlModifier)) {
		if (notches != 0) emit zoom_requested(notches);
		event->accept();
		return;
	}

	/*
	 * When the remote asked for the mouse, the wheel is ITS wheel -- the
	 * same rule a click follows, and Shift is the same escape hatch
	 * (sec 6.4). This is how less, vim and htop scroll, and none of them
	 * has anything in this terminal's scrollback to offer instead: they
	 * are on the alternate screen, where the history belongs to the shell
	 * underneath. Without this the wheel did nothing at all inside a
	 * full-screen program, which is the one place a user reaches for it.
	 *
	 * Buttons 4 and 5 are wheel up and down. The position goes first
	 * because libvterm reports the wheel at the last position it was
	 * told about, so a wheel with no move ahead of it lands wherever the
	 * previous click did.
	 */
	if (m_emulator && m_emulator->mouse_reporting()
	     && !event->modifiers().testFlag(Qt::ShiftModifier)) {
		qint64 line = 0;
		int col = 0;
		if (notches != 0 && cell_at(event->position().toPoint(), line, col)) {
			m_emulator->send_mouse_move(screen_row_for_line(line), col, 0);

			/*
			 * One report per notch, and PRESS only: a wheel has no
			 * release, and libvterm emits one for buttons 4 and 5 if
			 * asked, which every client would read as a second turn.
			 */
			const int button = notches > 0 ? 4 : 5;
			for (int i = 0; i < qAbs(notches); ++i) {
				m_emulator->send_mouse_button(button, true, 0);
			}
		}
		event->accept();
		return;
	}

	if (!m_scrollback) {
		QWidget::wheelEvent(event);
		return;
	}

	/*
	 * Three lines per notch, the usual desktop convention. Touch
	 * (QScroller, sec 6.4) will drive the same set_scroll_offset rather
	 * than growing a second scrolling path.
	 */
	if (notches != 0) scroll_by(notches * 3);

	event->accept();
}

/*
 * The xterm 256-colour palette: 16 system colours, a 6x6x6 cube, then 24
 * greys. Built rather than tabulated -- the cube and the ramp are exact
 * formulas, and a 256-entry literal is 256 chances to fat-finger one.
 *
 * The 16 system colours are the one part that is a choice rather than a
 * formula, and at M10 they become a preset-owned colour scheme (sec 10.2).
 */
void bssh_terminal_view::draw_images(QPainter &painter, int offset, bool below_text) const {
	if (!m_images) return;

	/*
	 * Cropped at the grid, which is what a terminal does with an image
	 * wider than the room left for it -- and what this did not do. The
	 * cull below is on ROWS only, so an image placed near the right edge,
	 * or one whose cell footprint the remote simply overstated, was drawn
	 * stretched across everything the widget would accept: the grid's own
	 * inset, the scrollbar's strip, and the partial row at the bottom.
	 * Most of that was invisible -- a QPainter on a widget is clipped to
	 * the widget, and a child scrollbar paints over its own rectangle
	 * afterwards -- so what it cost was fidelity and wasted rasterising
	 * on every repaint rather than a visible mess.
	 */
	painter.save();
	painter.setClipRect(0, 0, m_cols * m_cell_width, m_rows * m_cell_height);

	for (const bssh_image_placement_t &placement : m_images->placements()) {
		if ((placement.z < 0) != below_text) continue;

		/*
		 * Shifted by the scroll offset, so an image scrolled into history
		 * comes back with the text it was drawn beside. A negative row is
		 * an image above the live screen (sec 7's image scrollback); with
		 * the view scrolled back far enough it lands on screen again.
		 */
		const int row = placement.row + offset;
		if (row + placement.rows <= 0 || row >= m_rows) continue;

		/*
		 * Through the store rather than the field: a picture scrolled far
		 * enough back has been spilled to disk, and asking for it is what
		 * reads it back (sec 7.1). Null only if it could not be.
		 */
		const QImage image = m_images->image(placement);
		if (image.isNull()) continue;

		const QRect target(placement.col * m_cell_width,
		                    row * m_cell_height,
		                    placement.cols * m_cell_width,
		                    placement.rows * m_cell_height);
		painter.drawImage(target, image);
	}

	painter.restore();
}

QColor bssh_terminal_view::resolve(const bssh_color_t &color, const QColor &fallback) const {
	if (color.kind == bssh_color_t::RGB) return QColor(color.rgb);
	if (color.kind != bssh_color_t::INDEXED) return fallback;

	const int index = color.index;

	/*
	 * The low sixteen come from the host's colour scheme (sec 10.2); the
	 * cube and the grey ramp below do not, because they are exact formulas
	 * every terminal computes identically. A program asking for colour 137
	 * has asked for a particular RGB value, not for the user's taste, and
	 * recolouring those would make this terminal disagree with every
	 * other one about what 256-colour output looks like.
	 */
	/*
	 * Shared with the emulator, which has to give the same answer when a
	 * remote asks with OSC 4 -- a client quantising its own colours into
	 * this palette relies on the two agreeing exactly, and two copies of
	 * a formula is the way they stop.
	 */
	return QColor(bssh_palette_colour(m_scheme.ansi, index));
}

void bssh_terminal_view::update_metrics() {
	/*
	 * Every cached font carries a letter spacing computed from the cell
	 * width it was built against (see cell_font), and the key does not
	 * carry that width -- so a stale entry is indistinguishable from a
	 * fresh one. This is the only function that moves m_cell_width, so
	 * clearing here makes "no cached font outlives the width it was built
	 * against" an invariant rather than a duty each caller remembers.
	 *
	 * It used to be a duty, discharged in set_font_id() alone, and
	 * remeasure_cell() -- the screen-change path from pane_container --
	 * did not discharge it. Its own comment says it takes "the same three
	 * steps a font change takes"; it took three of the four. A window
	 * dragged between monitors of different scaling then drew every glyph
	 * of a run at the OLD advance while the run's origin, the cell
	 * backgrounds, the underlines and the cursor all used the new one:
	 * error zero at the run's first column and growing by the difference
	 * per column after it, so text separated visibly from its own
	 * background and overran the right margin.
	 *
	 * Cheap to throw away: an entry is one QFont plus one advance
	 * measurement, the cache is keyed by family-bold-italic so it is
	 * bounded by the fallback chain, and this runs on construction, on a
	 * font change and on a screen change -- never per frame.
	 *
	 * NOT pinned by a test, and deliberately recorded as such: driving it
	 * needs a device-pixel-ratio change, which the offscreen platform
	 * cannot produce -- the same gap layout_test already records for the
	 * two-screen re-layout it cannot drive either.
	 */
	m_font_cache.clear();

	const QFontMetricsF metrics(m_font);

	/*
	 * Rounded, never fractional: the grid has to land on whole pixels or
	 * the accumulated error across a wide window blurs the right-hand
	 * columns (sec 9.5). Fractional display scaling (125%, 150%) is
	 * exactly where this matters, which is a desktop case Android never
	 * exercises.
	 *
	 * 'W' is the measured glyph rather than the font's averageCharWidth,
	 * which is an average over a proportional-ish sample and is wrong for
	 * some fonts that ARE monospace. A font whose glyphs disagree about
	 * advance width is not a terminal font at all -- sec 9.6's CI check
	 * exists to keep any such font out of the bundled set.
	 */
	m_cell_width = qMax(1, qRound(metrics.horizontalAdvance(QLatin1Char('W'))));
	m_cell_height = qMax(1, qRound(metrics.height()));
	m_baseline = qRound(metrics.ascent());

	/*
	 * ...and whole DEVICE pixels, not merely whole logical ones, which is
	 * what sec 9.5 means by fractional scaling being where this matters.
	 *
	 * At 150% a 9-pixel cell is 13.5 device pixels, so every second
	 * column boundary lands on a half pixel. Each glyph is then
	 * antialiased at a different sub-pixel offset from its neighbour, and
	 * box-drawing characters -- whose whole job is to meet exactly at the
	 * cell edge -- stop meeting. A TUI's borders come out with a faint
	 * seam at every other column, which reads as a cheap terminal rather
	 * than as a scaling artefact. It is plainly visible at 150% and
	 * absent at 100%.
	 *
	 * So the cell is rounded to a multiple of however many logical pixels
	 * it takes to land on a whole device pixel: 1 at 100% and 200%, 2 at
	 * 150%, 4 at 125%. Beyond that the correction would cost more in
	 * glyph fidelity than the seam costs in appearance, so an unusual
	 * ratio is left alone.
	 */
	const qreal ratio = devicePixelRatioF();
	m_cell_width = bssh_snap_to_device_pixels(m_cell_width, ratio);
	m_cell_height = bssh_snap_to_device_pixels(m_cell_height, ratio);
}

int bssh_snap_to_device_pixels(int pixels, qreal device_ratio) {
	if (pixels < 1 || device_ratio <= 0) return qMax(1, pixels);

	/*
	 * How many logical pixels it takes to land on a whole device pixel:
	 * 1 at 100% and 200%, 2 at 150%, 4 at 125%.
	 */
	int period = 0;
	for (int k = 1; k <= 4; ++k) {
		if (qAbs(k * device_ratio - qRound(k * device_ratio)) < 0.001) {
			period = k;
			break;
		}
	}

	/*
	 * An unusual ratio (110%, say) would need a period of ten, and
	 * rounding a nine-pixel cell to ten costs more in glyph fidelity than
	 * the seam costs in appearance. Left alone.
	 */
	if (period <= 1) return pixels;

	return qMax(period, int(qRound(pixels / qreal(period))) * period);
}

namespace {

/*
 * What a new view starts with. A static rather than a constructor
 * argument, because the views are built by the pane container in a loop
 * that has no business knowing about device settings -- and because it is
 * a fact about the device, like the chords (sec 6.1).
 */
bssh_terminal_view::scrollbar_position_t default_position = bssh_terminal_view::SCROLLBAR_NONE;

/* Ligatures off by default (sec 9.2); set once from the device settings,
 * read by every view's font build, for the same reason as the scrollbar. */
bool default_ligatures = false;

/* The remote may change the cursor shape by default (sec 6.4); set from the
 * device settings, read when a DECSCUSR arrives so a change takes hold at
 * once rather than at the next rebuild. */
bool honour_remote_cursor = true;

} /* namespace */

void bssh_terminal_view::set_default_scrollbar_position(scrollbar_position_t where) {
	default_position = where;
}

bssh_terminal_view::scrollbar_position_t bssh_terminal_view::default_scrollbar_position() {
	return default_position;
}

void bssh_terminal_view::set_default_font_ligatures(bool ligatures) {
	default_ligatures = ligatures;
}

bool bssh_terminal_view::default_font_ligatures() {
	return default_ligatures;
}

void bssh_terminal_view::set_honour_remote_cursor_shape(bool honour) {
	honour_remote_cursor = honour;
}

bool bssh_terminal_view::honour_remote_cursor_shape() {
	return honour_remote_cursor;
}

int bssh_terminal_view::grid_origin_x() const {
	/*
	 * The grid starts after the inset, and after the bar as well when the
	 * bar is on the left. Everything that converts between pixels and
	 * cells asks this rather than adding the inset itself, which is what
	 * kept a left-hand scrollbar from being a rewrite of the mouse code.
	 */
	if (m_scrollbar_position == SCROLLBAR_LEFT && m_scrollbar && !m_scrollbar->isHidden()) {
		return BSSH_GRID_INSET_PX + m_scrollbar->sizeHint().width();
	}
	return BSSH_GRID_INSET_PX;
}

int bssh_terminal_view::grid_width() const {
	int taken = 2 * BSSH_GRID_INSET_PX;
	if (m_scrollbar && !m_scrollbar->isHidden()) taken += m_scrollbar->sizeHint().width();

	return qMax(0, width() - taken);
}

void bssh_terminal_view::set_scrollbar_position(scrollbar_position_t where) {
	if (where == m_scrollbar_position) return;

	m_scrollbar_position = where;
	m_scrollbar->setVisible(where != SCROLLBAR_NONE);

	place_scrollbar();

	/*
	 * The grid is a different width now, so a view that is ON SCREEN has
	 * to tell the remote -- recompute_grid emits grid_resized, which is
	 * what carries a window-change (sec 7). Without it the far end lays
	 * out for a column that is no longer there.
	 *
	 * And only on screen. A widget that has never been laid out is still
	 * at QWidget's default 100x30, which is about one row: recomputing
	 * there sends the remote a geometry nobody has, and the line that was
	 * on the screen goes into the scrollback to make room for it. Found
	 * by a tear-out test whose windows are never shown -- the same shape
	 * as an Android activity that has not been resumed yet. The next
	 * resizeEvent recomputes with the bar included, which is where a
	 * window that IS shown gets its answer anyway.
	 */
	if (isVisible()) recompute_grid();
	update();
}

void bssh_terminal_view::place_scrollbar() {
	if (!m_scrollbar) return;

	const int bar = m_scrollbar->sizeHint().width();
	const int x = m_scrollbar_position == SCROLLBAR_LEFT ? 0 : width() - bar;

	m_scrollbar->setGeometry(x, 0, bar, height());
	sync_scrollbar();
}

void bssh_terminal_view::sync_scrollbar() {
	if (!m_scrollbar || m_scrollbar->isHidden()) return;

	const int history = available_history();

	/*
	 * The BOTTOM is the live screen, which is where every terminal since
	 * xterm puts it and the opposite of how the offset is counted -- so
	 * the value is the history minus how far back the view is. Getting
	 * this backwards produces a scrollbar that works and moves the wrong
	 * way, which is worse than none.
	 */
	const QSignalBlocker quiet(m_scrollbar);

	m_scrollbar->setRange(0, history);
	m_scrollbar->setPageStep(qMax(1, m_rows));
	m_scrollbar->setSingleStep(1);
	m_scrollbar->setValue(history - qBound(0, m_scroll_offset, history));
}

/*
 * Twenty by four, and both numbers are arguments rather than taste.
 *
 * Twenty columns is about the narrowest that holds a prompt and a short
 * command without wrapping mid-word; four rows shows the prompt, what was
 * typed, and two lines of what came back. Below that a window is not a
 * terminal in any useful sense, and the point of a minimum is to stop the
 * window becoming something it is not.
 *
 * Small enough to constrain nothing that matters: four panes side by side
 * only happen above sec 8.2's 840dp class, and four twenty-column grids
 * fit inside that comfortably.
 */
static const int BSSH_MIN_GRID_COLS = 20;
static const int BSSH_MIN_GRID_ROWS = 4;

QSize bssh_terminal_view::minimumSizeHint() const {
	const QSize wanted = size_for_grid(BSSH_MIN_GRID_COLS, BSSH_MIN_GRID_ROWS);
	return wanted.isEmpty() ? QWidget::minimumSizeHint() : wanted;
}

QSize bssh_terminal_view::size_for_grid(int cols, int rows) const {
	/*
	 * The exact inverse of recompute_grid below: the inset, and the bar
	 * where there is one. A --geometry that ignored the scrollbar would
	 * be a column short the moment somebody turned one on (sec 8.6).
	 */
	int extra = 2 * BSSH_GRID_INSET_PX;
	if (m_scrollbar && !m_scrollbar->isHidden()) extra += m_scrollbar->sizeHint().width();

	return QSize(qMax(1, cols) * m_cell_width + extra,
	              qMax(1, rows) * m_cell_height + 2 * BSSH_GRID_INSET_PX);
}

namespace {

/*
 * A path as a shell will read it back as ONE argument.
 *
 * Single quotes, because inside them a shell interprets nothing at all --
 * no $, no backtick, no backslash, no glob. The one character that cannot
 * appear is a single quote itself, and the standard trick closes the
 * quoting, emits an escaped quote and opens it again: it's -> \'it\''s\'.
 *
 * Only when it is needed. Quoting every path would be safe and would also
 * mean that dropping /etc/hosts pastes something the user then has to
 * clean up, which is the kind of help that stops being help.
 */
QString shell_quoted(const QString &path) {
	static const QString safe = QStringLiteral(
	  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	  "@%+=:,./-_");

	bool needs_quoting = path.isEmpty();
	for (const QChar c : path) {
		if (!safe.contains(c)) needs_quoting = true;
	}

	if (!needs_quoting) return path;

	QString quoted = path;
	quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));

	return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

} /* namespace */

QString bssh_terminal_view::text_for_drop(const QMimeData *mime) {
	if (!mime) return QString();

	/*
	 * Files first, because a file manager offers BOTH: dragging a file
	 * carries its URL and, as a courtesy, that URL as text. Taking the
	 * text would paste "file:///home/nabbe/notes.txt", which is not a
	 * thing a shell can do anything with.
	 */
	if (mime->hasUrls()) {
		QStringList paths;

		for (const QUrl &url : mime->urls()) {
			/*
			 * A remote URL is pasted as it stands: this terminal may
			 * well be somewhere that can fetch it, and turning it
			 * into a local path would be a guess about a machine
			 * this app is not on.
			 */
			paths.append(url.isLocalFile() ? shell_quoted(url.toLocalFile())
			                                : shell_quoted(url.toString()));
		}

		/*
		 * Space-separated, never newline-separated: several files are
		 * several ARGUMENTS to one command, and joining them with
		 * newlines would run each of them as a command instead.
		 */
		return paths.join(QLatin1Char(' '));
	}

	if (mime->hasText()) return mime->text();

	return QString();
}

QString bssh_session_mime_type() {
	return QStringLiteral("application/x-bssh-session");
}

int bssh_drop_edge_at(const QPoint &at, const QSize &pane) {
	if (pane.width() <= 0 || pane.height() <= 0) return bssh_layout_manager::EDGE_MIDDLE;

	/* Distance to each edge as a share of the pane, so a wide pane and a
	 * tall one are divided alike. */
	const double left = double(at.x()) / pane.width();
	const double right = 1.0 - left;
	const double top = double(at.y()) / pane.height();
	const double bottom = 1.0 - top;

	const double nearest = qMin(qMin(left, right), qMin(top, bottom));
	if (nearest >= 0.25) return bssh_layout_manager::EDGE_MIDDLE;

	if (nearest == left) return bssh_layout_manager::EDGE_LEFT;
	if (nearest == right) return bssh_layout_manager::EDGE_RIGHT;
	if (nearest == top) return bssh_layout_manager::EDGE_TOP;
	return bssh_layout_manager::EDGE_BOTTOM;
}

void bssh_terminal_view::dragEnterEvent(QDragEnterEvent *event) {
	/*
	 * Text this view can paste, OR a session for the window to adopt.
	 *
	 * The session half used to be REFUSED here, with a comment saying it
	 * was refused "so that it reaches the window, which is what knows how
	 * to adopt one". That is not how Qt routes a drag. The target is
	 * chosen before delivery -- findDnDTarget() walks up from the widget
	 * under the pointer to the first one with acceptDrops(), and this
	 * view has it -- and a child that ignores the event does NOT hand it
	 * to an ancestor. The whole drag was ignored, the no-drop cursor
	 * shown, and no drop was ever delivered anywhere.
	 *
	 * The view covers essentially the whole pane area, so that was every
	 * drop onto the panes. What the user saw: dragging a tab onto another
	 * window's terminal produced a NEW WINDOW every time, because
	 * start_tab_drag() reads IgnoreAction as "nobody wanted it" and tears
	 * the tab out. sec 8.4's tile-by-dropping could not be reached at all.
	 *
	 * So it is accepted here and handed up, which is the only arrangement
	 * that gets the window the drop.
	 */
	if (!text_for_drop(event->mimeData()).isEmpty()
	     || event->mimeData()->hasFormat(bssh_session_mime_type())) {
		event->acceptProposedAction();
	}
}

/*
 * Where a pane would land, followed as the pointer moves, so the drop is
 * aimed rather than guessed: the zone under the pointer is lit and the
 * rest of the pane is not.
 */
void bssh_terminal_view::dragMoveEvent(QDragMoveEvent *event) {
	if (!event->mimeData()->hasFormat(bssh_session_mime_type())) {
		QWidget::dragMoveEvent(event);
		return;
	}

	const int edge = bssh_drop_edge_at(event->position().toPoint(), size());
	if (edge != m_drop_edge) {
		m_drop_edge = edge;
		update();
	}
	event->acceptProposedAction();
}

void bssh_terminal_view::dragLeaveEvent(QDragLeaveEvent *event) {
	if (m_drop_edge != -1) {
		m_drop_edge = -1;
		update();
	}
	QWidget::dragLeaveEvent(event);
}

void bssh_terminal_view::dropEvent(QDropEvent *event) {
	/*
	 * A session goes up to the window untouched: this view knows how to
	 * paste text and nothing about adopting a session, and sec 8.4 keeps
	 * it that way. Accepted above only so that Qt delivers it here at
	 * all -- refusing meant nobody was delivered anything.
	 */
	if (event->mimeData()->hasFormat(bssh_session_mime_type())) {
		const int edge = bssh_drop_edge_at(event->position().toPoint(), size());
		m_drop_edge = -1;
		update();

		event->acceptProposedAction();
		emit session_dropped(event->mimeData(), edge);
		return;
	}

	const QString text = text_for_drop(event->mimeData());
	if (text.isEmpty()) return;

	event->acceptProposedAction();

	/*
	 * Focus first, and it matters. A drop lands on the pane under the
	 * pointer, which need not be the focused one -- and the window pastes
	 * into the session it considers current. Taking focus the way a click
	 * does makes the pane that was dropped on the pane that receives it,
	 * and it is also what wires this signal up: the window connects to
	 * the FOCUSED view (see m_wired_view), so emitting first would emit
	 * into nothing.
	 */
	setFocus(Qt::MouseFocusReason);

	emit paste_requested(text);
}

void bssh_terminal_view::recompute_grid() {
	/*
	 * A widget with no width has no grid, and must not claim one.
	 *
	 * The arithmetic below floors at one column and one row, so a view
	 * that has not been given a size yet -- a pane in a window nobody has
	 * shown, a splitter child squeezed to nothing mid-layout -- would
	 * announce a 1-column terminal and the remote would reflow every line
	 * it has to match. Found exactly that way: a tab torn into an unshown
	 * window came out 0 pixels wide, told its session it was one column,
	 * and the line that had been on screen went into the scrollback a
	 * character at a time.
	 *
	 * The floor itself is right -- a terminal is never zero columns -- but
	 * it is an answer to "how small can a grid be", not to "does this
	 * widget have one yet". sec 8.4 records the same shape once already,
	 * in a status line that reported "1x41 cells" on a screen plainly
	 * showing more.
	 */
	if (width() <= 0 || height() <= 0) return;

	/*
	 * The focus border's width is reserved whether or not the border is
	 * drawn. Reserving it only when focused would change the column count
	 * as focus moved between panes -- a window-change message to the
	 * remote every time the user clicked a different pane, and a reflow
	 * of whatever they were reading.
	 */
	const int cols = qMax(1, grid_width() / m_cell_width);
	const int rows = qMax(1, (height() - 2 * BSSH_GRID_INSET_PX) / m_cell_height);

	if (cols == m_cols && rows == m_rows) return;

	m_cols = cols;
	m_rows = rows;

	/* The page step is a screenful, and a screenful just changed. */
	sync_scrollbar();

	/*
	 * Only a view that is ON SCREEN tells its session how big it is.
	 *
	 * A window-change message is a claim about what the user can see, and
	 * a pane in a window nobody has shown is not showing anything. Qt
	 * still lays such a widget out -- with whatever the splitter had
	 * before it had a real size -- and this used to pass that on: the
	 * measured symptom was a session told it was 1x2, which made the
	 * remote reflow every line it had to one column. A tab torn into an
	 * unshown window then had its screen in the scrollback, a character
	 * per line.
	 *
	 * The computed cols and rows above are kept either way, so anything
	 * asking this view what it holds gets an answer; what is withheld is
	 * only the message to the far end. When the window is shown, the
	 * resize that follows says it properly.
	 */
	if (isVisible()) emit grid_resized(m_cols, m_rows);
}

void bssh_terminal_view::contextMenuEvent(QContextMenuEvent *event) {
	/*
	 * One handler for both gestures. Qt raises this from a right-click on
	 * a desktop and from a long press on Android, which is how the host
	 * list already gets its menu on both -- so the two do not need
	 * separate code, only different LAYOUTS, which the window applies.
	 *
	 * Not raised while the remote is reading the mouse: a full-screen
	 * program that asked for button events is entitled to the right
	 * button, and a menu appearing over it would be the terminal
	 * answering for it. Shift is the conventional override there and
	 * reaches the selection path instead, as it does for clicks.
	 */
	if (m_emulator && m_emulator->mouse_reporting()
	     && !(event->modifiers() & Qt::ShiftModifier)) {
		event->ignore();
		return;
	}

	emit context_menu_requested(event->globalPos());
	event->accept();
}

void bssh_terminal_view::keyPressEvent(QKeyEvent *event) {
	/*
	 * Both paths matter, and this is only one of them. Android delivers
	 * some keys as IME commit text rather than key events (sec 6.1), so
	 * inputMethodEvent needs the same forwarding before the Android build
	 * is usable with a soft keyboard. Noted rather than stubbed, because
	 * a stub here would look done.
	 */
	/*
	 * Typing returns to the live screen. Anything else means a keystroke
	 * disappears into history the user is reading -- they type, nothing
	 * appears, and the terminal looks broken.
	 */
	if (m_input_router && m_input_router->handle_key(event)) {
		scroll_to_live();
		event->accept();
		return;
	}

	QWidget::keyPressEvent(event);
}

void bssh_terminal_view::keyReleaseEvent(QKeyEvent *event) {
	/*
	 * Releases matter only to the kitty protocol's event types (sec 7.1),
	 * and the router answers only for a key whose press it sent that way.
	 */
	if (m_input_router && m_input_router->handle_key_release(event)) {
		event->accept();
		return;
	}

	QWidget::keyReleaseEvent(event);
}

void bssh_terminal_view::inputMethodEvent(QInputMethodEvent *event) {
	/*
	 * The second delivery path sec 6.1 warns about. On Android some keys
	 * arrive only as IME commit text and never as key events; on desktop
	 * this is where dead keys and compose sequences arrive. A terminal
	 * that implements only keyPressEvent silently drops both.
	 *
	 * Pre-edit text is deliberately ignored: an in-progress composition
	 * is not input yet, and echoing it to the remote would send
	 * characters the user has not committed to.
	 */
	if (m_input_router && !event->commitString().isEmpty()) {
		m_input_router->send_text(event->commitString());
		event->accept();
		return;
	}

	QWidget::inputMethodEvent(event);
}

void bssh_terminal_view::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	/* Placed before the grid is recomputed: the bar's width is part of
	 * the arithmetic, not a decoration applied afterwards. */
	place_scrollbar();
	recompute_grid();
}

void bssh_terminal_view::leaveEvent(QEvent *event) {
	m_pointer_inside = false;
	update(pane_grip_rect().adjusted(-2, -2, 2, 2));
	QWidget::leaveEvent(event);
}

void bssh_terminal_view::set_pane_handle(bool handle) {
	if (m_pane_handle == handle) return;

	m_pane_handle = handle;
	update(pane_grip_rect().adjusted(-2, -2, 2, 2));
}


void bssh_terminal_view::show_drop_zone(int edge) {
	if (edge == m_drop_edge) return;
	m_drop_edge = edge;
	update();
}

void bssh_terminal_view::set_being_dragged(bool dragged) {
	if (m_being_dragged == dragged) return;

	m_being_dragged = dragged;
	update();
}

QRect bssh_terminal_view::pane_grip_rect() const {
	/*
	 * Sized from the CELL rather than from a constant, so it stays in
	 * proportion to the text at any font size, and clamped so it is
	 * neither a speck on a tiny font nor a slab on a huge one.
	 */
	const int side = qBound(14, int(m_cell_height * 1.1), 26);

	/*
	 * A pane too narrow to spare the corner has no handle, rather than
	 * one hanging off its left edge. Measured in a test that asked for
	 * the rect before the splitter had given the pane any width: the
	 * arithmetic happily returned x = -25 in a view 0 wide, which draws
	 * nothing and would still have answered contains() for a press.
	 */
	if (width() < side * 3 || height() < side * 2) return QRect();

	return QRect(width() - side - 3, 3, side, side);
}

void bssh_terminal_view::paintEvent(QPaintEvent *event) {
	Q_UNUSED(event);

	QPainter painter(this);
	painter.fillRect(rect(), effective_background());
	painter.setFont(m_font);
	painter.setPen(m_foreground);

	/*
	 * Everything below is drawn in GRID coordinates, inset far enough for
	 * the focus border to sit beside the cells rather than on top of them.
	 * The border itself is drawn at the end, after the transform is put
	 * back.
	 */
	painter.translate(grid_origin_x(), BSSH_GRID_INSET_PX);

	if (!m_screen) {
		/* No session yet: name the app rather than showing a blank void. */
		const QString placeholder = QString::fromLatin1(BSSH_PLACEHOLDER);
		for (int col = 0; col < placeholder.size() && col < m_cols; ++col) {
			painter.drawText(QPointF(col * m_cell_width, m_baseline), QString(placeholder.at(col)));
		}
		return;
	}

	const int rows = qMin(m_rows, m_screen->rows());
	const int cols = qMin(m_cols, m_screen->cols());

	/*
	 * With an offset, the top of the view comes from the scrollback and
	 * the live screen slides down out of sight by the same amount. The
	 * screen itself is untouched: scrolling back is something the VIEW
	 * does, so the session carries on and the grid the remote believes in
	 * never changes.
	 */
	const int history = m_scrollback ? m_scrollback->count() : 0;
	const int offset = qBound(0, m_scroll_offset, history);

	/*
	 * Images the remote asked to put UNDER the text (kitty's z < 0, how a
	 * program puts a background behind a TUI). This pass has to run before
	 * the cells are drawn, which is why there are two of them.
	 *
	 * The comment here used to describe exactly this, next to a single
	 * loop that drew everything over the text -- a comment describing an
	 * intention rather than the code beneath it, which is worse than no
	 * comment at all because it stops the reader looking.
	 */
	draw_images(painter, offset, true);

	/*
	 * Cell by cell, and per-cell QFont changes for bold/italic. That is
	 * the naive renderer on purpose: sec 9.5's shared glyph atlas and
	 * per-row glyph runs are M9, and building a cache now -- before the
	 * font pipeline exists and before there is anything to profile --
	 * would be a cache written to be thrown away.
	 */
	for (int row = 0; row < rows; ++row) {
		const qreal baseline = m_baseline + row * m_cell_height;

		/*
		 * Rows above the fold come from history; the rest from the live
		 * screen, shifted down by however far back the view is.
		 */
		const int history_index = history - offset + row;
		const bool from_history = offset > 0 && history_index < history;
		const QList<bssh_cell_t> &history_line =
		        from_history ? m_scrollback->line(history_index) : QList<bssh_cell_t>();
		const int screen_row = row - offset;

		/*
		 * Drawn in RUNS rather than cell by cell (sec 9.5's "per-row
		 * glyph runs"). One drawText per cell measured at 94 ms for a
		 * full screen -- eleven frames a second, which is visible lag
		 * while output is streaming -- because each call re-sets the
		 * painter's font and re-lays out a one-character string.
		 *
		 * A run is a stretch of ordinary cells sharing everything that
		 * decides how they are drawn. Anything that changes mid-row
		 * (colour, weight, the family serving the glyph, a wide glyph,
		 * the selection) ends the run and starts the next.
		 */
		QString run_text;
		int run_start = 0;
		QColor run_fg;
		QString run_family;
		bool run_bold = false;
		bool run_italic = false;

		auto flush_run = [&] {
			if (run_text.isEmpty()) return;

			painter.setFont(cell_font(run_family, run_bold, run_italic));
			painter.setPen(run_fg);
			painter.drawText(QPointF(run_start * m_cell_width, baseline), run_text);

			run_text.clear();
		};

		for (int col = 0; col < cols; ++col) {
			static const bssh_cell_t blank;
			const bssh_cell_t &cell =
			        from_history ? (col < history_line.size() ? history_line.at(col) : blank)
			                     : (screen_row >= 0 ? m_screen->cell(screen_row, col) : blank);

			/* The far half of a double-width glyph: already drawn. */
			if (cell.width == 0) continue;

			QColor fg = resolve(cell.fg, m_foreground);
			QColor bg = resolve(cell.bg, m_background);

			if (cell.attrs & bssh_cell_t::INVERSE) std::swap(fg, bg);

			/*
			 * DECSCNM reverse video (sec 6.4) flips the whole screen, so
			 * it swaps every cell here, on top of the per-cell inverse and
			 * before the selection swap -- a selection stays visible by
			 * re-inverting whatever it lands on. The comparisons below use
			 * effective_background(), so a default cell now matches the
			 * base fill and is not redundantly repainted.
			 */
			if (m_reverse_video) std::swap(fg, bg);

			/*
			 * Selection inverts rather than tinting: it stays visible
			 * whatever colours the cell already carries, which a fixed
			 * highlight colour cannot promise against 256 possible
			 * backgrounds.
			 */
			if (is_selected(line_for_row(row), col)) std::swap(fg, bg);

			/*
			 * sec 9.3's floor, applied AFTER both swaps and to the
			 * colour that ends up drawing the glyph, against the one
			 * that ends up behind it.
			 *
			 * The order is the whole point. Lifting before the swaps
			 * puts a lifted colour behind an inverse cell, which
			 * repaints a faithful palette's background block for no
			 * readability gain -- the text on that block is the other
			 * colour, and lifting THAT is what makes it readable. A
			 * background is not text, and only text is lifted.
			 */
			if (m_raise_contrast) fg = QColor(bssh_raise_contrast(fg.rgb(), bg.rgb()));

			/*
			 * And dimmed after the floor, never before: the floor
			 * lifts until it clears, so a dim applied first is simply
			 * undone. Text only -- the background keeps its pure
			 * black, so this turns the light down rather than
			 * greying the screen out.
			 */
			if (m_text_brightness != 100) {
				fg = QColor(bssh_dim(fg.rgb(), m_text_brightness));

				/*
				 * And the block behind it, where there is one. An
				 * inverse cell and a selection both draw a solid
				 * rectangle in what was the FOREGROUND -- the
				 * brightest object on the screen -- so leaving it
				 * alone made the night setting miss the one thing
				 * most likely to have prompted it. Measured: at 50%
				 * the text went 255 -> 127 and the inverse block
				 * stayed 255.
				 *
				 * The terminal's own background is excluded by the
				 * same test the fill below uses, so pure black stays
				 * pure black and only a cell that asked for a colour
				 * of its own comes down.
				 */
				if (bg != effective_background()) {
					bg = QColor(bssh_dim(bg.rgb(), m_text_brightness));
				}
			}

			/*
			 * Find highlight (sec 6.4), last so the brightness slider does
			 * not dim the one thing a search exists to make findable: the
			 * current match burns amber, the rest a muted amber, dark text
			 * on both. A fixed pair rather than the scheme's, because a
			 * match has to stand out against 256 possible backgrounds the
			 * way a browser's find does.
			 */
			if (!m_find_matches.isEmpty()) {
				const qint64 abs_line = line_for_row(row);
				if (is_current_find_match(abs_line, col)) {
					bg = QColor(0xff, 0xd5, 0x4f);
					fg = QColor(0x1c, 0x1c, 0x1c);
				} else if (is_find_match(abs_line, col)) {
					bg = QColor(0x9a, 0x7a, 0x00);
					fg = QColor(0xf4, 0xf4, 0xf4);
				}
			}

			const int x = col * m_cell_width;
			const int y = row * m_cell_height;
			const int width = qMax(1, int(cell.width)) * m_cell_width;

			if (bg != effective_background()) painter.fillRect(x, y, width, m_cell_height, bg);

			const bool blank_cell = cell.ch == U'\0' || cell.ch == U' ';
			const bool wide = cell.width > 1;

			if (!blank_cell) {
				/*
				 * Which family carries THIS character (sec 9.3). Asked
				 * per cell because that is the granularity the answer
				 * changes at, and cheap because the catalog caches
				 * coverage: the work is a hash lookup, not a font
				 * construction.
				 */
				const QString family =
				        bssh_font_catalog::instance().family_for_glyph(m_font_id, cell.ch);
				const bool bold = cell.attrs & bssh_cell_t::BOLD;
				const bool italic = cell.attrs & bssh_cell_t::ITALIC;

				const bool continues = !run_text.isEmpty() && !wide &&
				                        run_start + run_text.size() == col &&
				                        family == run_family && fg == run_fg &&
				                        bold == run_bold && italic == run_italic;

				if (!continues) {
					flush_run();
					run_start = col;
					run_fg = fg;
					run_family = family;
					run_bold = bold;
					run_italic = italic;
				}

				/*
				 * Appended without building a temporary QString per
				 * character: fromUcs4 allocates, and this runs once per
				 * cell on screen per repaint. Only the rare non-BMP
				 * codepoint needs the slow path.
				 */
				if (cell.ch < 0x10000) {
					run_text.append(QChar(char16_t(cell.ch)));
				} else {
					run_text.append(QString::fromUcs4(&cell.ch, 1));
				}

				/*
				 * A double-width glyph is drawn alone: the run relies on
				 * every glyph advancing exactly one cell (see
				 * cell_font's letter spacing), and a glyph that advances
				 * two would push the rest of the run half a screen out
				 * of line.
				 */
				if (wide) flush_run();
			} else {
				flush_run();
			}

			/*
			 * A link is underlined so it is visibly one. Terminals
			 * differ on whether to colour them too; underlining alone
			 * keeps the remote's own colours intact, which matters when
			 * the link IS the output (ls --color, a compiler error).
			 */
			if (cell.link != 0 || (cell.attrs & bssh_cell_t::UNDERLINE)) {
				painter.setPen(fg);
				painter.drawLine(x, y + m_cell_height - 1, x + width - 1,
				                  y + m_cell_height - 1);
			}
		}

		flush_run();
	}

	/* The second of the two image passes: everything at z >= 0, over the
	 * text it covers. The z < 0 pass ran before the cells were drawn. */
	draw_images(painter, offset, false);

	/*
	 * No cursor while reading history: it belongs to the live screen. And
	 * none on the dark half of a blink (sec 10.2).
	 */
	if (m_screen->cursor_visible() && offset == 0 && m_cursor_on) {
		const int x = m_screen->cursor_col() * m_cell_width;
		const int y = m_screen->cursor_row() * m_cell_height;
		const bssh_cell_t &cell = m_screen->cell(m_screen->cursor_row(), m_screen->cursor_col());

		/*
		 * Underline and bar are thin marks laid over the cell, so the
		 * character stays as it was drawn. Only the block has to punch
		 * the glyph out of itself: a solid block that simply covered the
		 * cell would hide the one character the user is looking at.
		 */
		/*
		 * Dimmed with everything else. A block cursor is the brightest
		 * thing on the screen and it BLINKS, so leaving it at full
		 * strength would leave the one object most likely to be the
		 * reason somebody reached for the slider.
		 */
		/*
		 * sec 9.3's floor reaches the cursor too, and it did not.
		 *
		 * The setting says it lifts "a drawn colour ... when the
		 * scheme's own value falls under it", and a cursor is a drawn
		 * colour a person picks: the colour editor offers Background,
		 * Text and Cursor, so a cursor that vanishes into its own
		 * background is reachable through the feature that exists to
		 * choose one. The five shipped schemes all set cursor equal to
		 * foreground, which is why nothing had ever noticed -- their
		 * cursor clears the floor because their TEXT does.
		 *
		 * Against the background, before the dim, in that order: the
		 * comment on the text path twenty lines up says why -- the
		 * floor lifts until it clears, so a dim applied first is simply
		 * undone.
		 */
		QRgb cursor_ink = m_scheme.cursor;
		if (m_raise_contrast)
			cursor_ink = bssh_raise_contrast(cursor_ink,
			                                   effective_background().rgb());

		const QColor cursor_colour(bssh_dim(cursor_ink, m_text_brightness));

		/*
		 * An UNFOCUSED terminal gets a hollow block, whatever shape it
		 * would otherwise draw -- konsole's convention, and xterm's and
		 * gnome-terminal's before it (sec 10.2).
		 *
		 * Whatever shape, because the outline is doing a different job
		 * from the shape. The shape says what kind of cursor the host
		 * asked for; the outline says "your keystrokes are not going
		 * here", and an unfilled bar or underline is a one-pixel line
		 * with a one-pixel hole in it -- a distinction nobody can see at
		 * the moment they most need it. This is the same safety question
		 * sec 8.3 calls non-negotiable about the focus border, asked of
		 * the one mark that is on screen even with a single pane, where
		 * that border is deliberately not drawn.
		 *
		 * hasFocus() rather than the focus border's flag: the border is
		 * suppressed when there is only one pane, and a lone terminal in
		 * a window the user has clicked away from is exactly a terminal
		 * that should not look live.
		 */
		if (!hasFocus()) {
			const qreal pen = qMax(1, m_cell_height / 12);
			painter.setPen(QPen(cursor_colour, pen));
			painter.setBrush(Qt::NoBrush);
			painter.drawRect(QRectF(x + pen / 2.0, y + pen / 2.0,
			                         m_cell_width - pen, m_cell_height - pen));
		} else if (m_cursor_style == QStringLiteral("underline")) {
			const int thickness = qMax(1, m_cell_height / 10);
			painter.fillRect(x, y + m_cell_height - thickness,
			                  m_cell_width, thickness, cursor_colour);
		} else if (m_cursor_style == QStringLiteral("bar")) {
			const int thickness = qMax(1, m_cell_width / 6);
			painter.fillRect(x, y, thickness, m_cell_height, cursor_colour);
		} else {
			painter.fillRect(x, y, m_cell_width, m_cell_height, cursor_colour);
			if (cell.ch != U'\0' && cell.ch != U' ') {
				/*
				 * The font the CELL was drawn with, not m_font:
				 * the family that actually covers this glyph
				 * (sec 9.3) and cell_font's letter spacing.
				 * m_font is neither, so the punch-out was a
				 * different shape and a different width from the
				 * glyph it is cutting out of the block -- and for
				 * anything outside the chosen family it was the
				 * notdef box, since make_font sets NoFontMerging.
				 */
				const QString family = bssh_font_catalog::instance()
				        .family_for_glyph(m_font_id, cell.ch);
				painter.setFont(cell_font(family,
				                            cell.attrs & bssh_cell_t::BOLD,
				                            cell.attrs & bssh_cell_t::ITALIC));
				/*
				 * The screen's background, and it needs no
				 * floor of its own: the block above was lifted
				 * AGAINST this very colour, so the glyph drawn
				 * on it clears the floor by construction
				 * whenever the setting is on -- and when it is
				 * off, faithful colours are what was asked for.
				 *
				 * Written down because the obvious edit here is
				 * to raise it against the cursor, which was
				 * tried: it is a no-op in every reachable case,
				 * and a change nothing can observe is one the
				 * next reader has to re-derive.
				 */
				painter.setPen(effective_background());
				painter.drawText(QPointF(x, m_baseline + m_screen->cursor_row() * m_cell_height),
				                  QString::fromUcs4(&cell.ch, 1));
			}
		}
	}

	/*
	 * The grid transform comes off only now. Everything above is in GRID
	 * coordinates -- including the cursor, which used to be drawn below
	 * this line in raw widget coordinates and therefore landed one pixel
	 * left and up of the cell it marks, or a whole scrollbar's width away
	 * with the bar on the left. Measured: the cell painted at x 1..9 and
	 * the block at 0..9. Only the focus border belongs outside, because
	 * it frames the widget rather than the grid.
	 */
	painter.resetTransform();

	if (m_focus_border) {
		/*
		 * Drawn last, over everything, and inset by half a pen width so
		 * the whole line lands inside the widget rather than half of it
		 * being clipped away by the neighbouring pane.
		 */
		/*
		 * The theme's accent rather than a third copy of the literal
		 * (sec 6.2). Read from the palette so it follows whatever is
		 * applied, including a palette the platform supplied.
		 */
		QPen border(palette().color(QPalette::Highlight));
		border.setWidth(2);
		painter.setPen(border);
		painter.drawRect(rect().adjusted(1, 1, -1, -1));
	}

	/*
	 * The pane's drag handle, under the pointer only.
	 *
	 * Filled with the terminal's own background rather than blended,
	 * because a grip half-showing the character underneath reads as a
	 * rendering fault rather than as a control -- so while it is up it
	 * occludes those cells, and that is why it is up only while the
	 * pointer is in this pane. Two pixel tests made that decision: drawn
	 * always, it put ink in a run-of-text check and swallowed a cursor.
	 */
	if (grip_visible()) {
		const QRect grip = pane_grip_rect();
		painter.fillRect(grip, m_background);

		QColor ink(bssh_raise_contrast(m_foreground.rgb(), m_background.rgb()));
		ink.setAlphaF(0.45);
		QPen grip_pen(ink);
		grip_pen.setWidthF(qMax(1.0, grip.height() / 12.0));
		grip_pen.setCapStyle(Qt::RoundCap);
		painter.setPen(grip_pen);

		for (int i = 0; i < 3; ++i) {
			const qreal y = grip.top() + grip.height() * (0.32 + i * 0.18);
			painter.drawLine(QPointF(grip.left() + grip.width() * 0.24, y),
			                  QPointF(grip.left() + grip.width() * 0.76, y));
		}
	}

	/*
	 * And, while a drag is carrying this pane, a net over the whole of
	 * it and a border around it.
	 *
	 * Drawn LAST, over the focus border and the grip, because it is the
	 * one thing on the pane that has to be unmistakable: the question a
	 * user is asking mid-drag is "which terminal am I about to move",
	 * and an answer that a cursor or a title bar could obscure is not an
	 * answer. The diagonal net rather than a tint, because a tint over a
	 * dark scheme is a slightly different dark and reads as nothing.
	 */
	/*
	 * The zone a dragged pane would land in: half the pane at the edge it
	 * would split (the half the arrival would get), or a frame inset from
	 * all four sides for the swap. Filled rather than hatched, because the
	 * hatch already means "this is the pane being carried".
	 */
	if (m_drop_edge >= 0) {
		QRect zone = rect();
		switch (m_drop_edge) {
		case bssh_layout_manager::EDGE_LEFT:   zone.setWidth(width() / 2); break;
		case bssh_layout_manager::EDGE_RIGHT:  zone.setLeft(width() / 2); break;
		case bssh_layout_manager::EDGE_TOP:    zone.setHeight(height() / 2); break;
		case bssh_layout_manager::EDGE_BOTTOM: zone.setTop(height() / 2); break;
		default:
			zone = rect().adjusted(width() / 4, height() / 4, -width() / 4, -height() / 4);
			break;
		}

		QColor fill(palette().color(QPalette::Highlight));
		fill.setAlphaF(0.35);
		painter.fillRect(zone, fill);
	}

	if (m_being_dragged) {
		const QColor accent = palette().color(QPalette::Highlight);

		QColor mesh(accent);
		mesh.setAlphaF(0.35);
		QPen net(mesh);
		net.setWidthF(qMax(1.0, m_cell_height / 8.0));
		painter.setPen(net);

		const int step = qMax(10, m_cell_height);
		for (int x = -height(); x < width(); x += step) {
			painter.drawLine(x, height(), x + height(), 0);
		}

		QColor edge(accent);
		edge.setAlphaF(0.9);
		QPen outline(edge);
		outline.setWidth(qMax(3, m_cell_height / 4));
		painter.setPen(outline);
		painter.setBrush(Qt::NoBrush);
		const int inset = outline.width() / 2;
		painter.drawRect(rect().adjusted(inset, inset, -inset - 1, -inset - 1));
	}

	/*
	 * The selection handles, last so nothing draws over them (sec 6.4).
	 *
	 * Drawn SMALLER than the rect that answers for them: the circle is a
	 * third of the touch target, because a 48dp disc over a terminal hides
	 * several cells of the text it is there to help select, while the
	 * thing a finger has to hit stays 48dp. What you see is a marker and
	 * what you press is a target.
	 */
	for (int which = 0; which < 2; ++which) {
		const QRect box = selection_handle_rect(which == 0);
		if (!box.isValid()) continue;

		const int radius = box.width() / 6;
		const QPoint centre(box.center().x(), box.top() + radius);

		painter.setPen(Qt::NoPen);
		painter.setBrush(palette().highlight());
		painter.drawEllipse(centre, radius, radius);

		/* A stem up to the cell, so the disc reads as attached to the
		 * end of the selection rather than floating under it. */
		QPen stem(palette().highlight().color());
		stem.setWidth(qMax(2, radius / 3));
		painter.setPen(stem);
		painter.drawLine(centre.x(), box.top() - m_cell_height,
		                  centre.x(), box.top());
	}
}

/*
 * Absolute line numbering: history lines come first, the live screen
 * continues straight on from them. One coordinate space for both is what
 * lets a selection span the fold and survive scrolling.
 */
QRect bssh_terminal_view::cell_rect(qint64 line, int col) const {
	/*
	 * A VIEW row here, unlike screen_row_for_line below: this answers
	 * "where is it drawn", and what is drawn includes the scrollback, so
	 * the offset belongs in. The two conversions sit next to each other
	 * deliberately -- they differ by exactly that offset, and picking the
	 * wrong one is how a click while scrolled back named the wrong line.
	 */
	const qint64 row = line - line_for_row(0);
	if (row < 0 || row >= m_rows || col < 0 || col >= m_cols) return QRect();

	return QRect(grid_origin_x() + col * m_cell_width,
	              BSSH_GRID_INSET_PX + int(row) * m_cell_height,
	              m_cell_width, m_cell_height);
}

bool bssh_terminal_view::take_shift_override(Qt::KeyboardModifiers modifiers) {
	if (modifiers.testFlag(Qt::ShiftModifier)) return true;

	if (!m_input_router) return false;
	if (!(m_input_router->effective_modifiers() & bssh_emulator::MOD_SHIFT)) return false;

	/*
	 * The ARMED half is consumed, like a key consumes it: leaving it set
	 * would make the next tap select too, which is not what "one shot"
	 * means anywhere else.
	 *
	 * The LOCKED half is deliberately NOT consumed. A user who locked
	 * Shift to work inside a program that has grabbed the mouse (sec 6.2)
	 * wants the next tap to select as well; a lock that the first tap
	 * quietly released would be a lock for exactly as long as it went
	 * unused.
	 */
	m_input_router->set_sticky_modifiers(
	        m_input_router->sticky_modifiers() & ~quint8(bssh_emulator::MOD_SHIFT));

	return true;
}

int bssh_terminal_view::screen_row_for_line(qint64 line) const {
	/*
	 * libvterm's mouse reports are in LIVE SCREEN rows, so the scroll
	 * offset must come out -- `line - line_for_row(0)` leaves it in, and
	 * a click while scrolled back then named a row that far down the
	 * screen. Scrolled back five lines, a mouse-aware program on the
	 * primary screen acted on the wrong line every time.
	 */
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	return int(line - history_base() - history);
}

qint64 bssh_terminal_view::history_base() const {
	/*
	 * Where the scrollback's own line 0 sits in absolute numbering. Zero
	 * until the ring first evicts, and then one higher per line dropped
	 * -- which is what stops an anchored selection sliding down the
	 * content as output arrives (see bssh_scrollback::base).
	 */
	return m_scrollback ? m_scrollback->base() : 0;
}

qint64 bssh_terminal_view::line_for_row(int row) const {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	return history_base() + history - m_scroll_offset + row;
}

const bssh_cell_t &bssh_terminal_view::cell_for_line(qint64 line, int col) const {
	static const bssh_cell_t blank;

	const qint64 history = m_scrollback ? m_scrollback->count() : 0;

	/*
	 * Absolute in, buffer index out. A line whose name is below the base
	 * has been evicted -- it is gone rather than renumbered, so the
	 * honest answer is blank rather than whatever now sits at that index.
	 */
	const qint64 index = line - history_base();
	if (index < 0) return blank;

	if (index < history) {
		const QList<bssh_cell_t> &stored = m_scrollback->line(int(index));
		return col >= 0 && col < stored.size() ? stored.at(col) : blank;
	}

	if (!m_screen) return blank;
	return m_screen->cell(screen_row_for_line(line), col);
}

bool bssh_terminal_view::cell_at(const QPoint &point, qint64 &line, int &col) const {
	if (m_cell_width <= 0 || m_cell_height <= 0) return false;

	const int row = qBound(0, (point.y() - BSSH_GRID_INSET_PX) / m_cell_height,
	                        qMax(0, m_rows - 1));
	col = qBound(0, (point.x() - grid_origin_x()) / m_cell_width, qMax(0, m_cols - 1));
	line = line_for_row(row);
	return true;
}

void bssh_terminal_view::normalised_selection(qint64 &first_line, int &first_col,
                                               qint64 &last_line, int &last_col) const {
	first_line = m_selection_anchor_line;
	first_col = m_selection_anchor_col;
	last_line = m_selection_focus_line;
	last_col = m_selection_focus_col;

	/* Dragging upwards or leftwards is the same selection, backwards. */
	if (last_line < first_line || (last_line == first_line && last_col < first_col)) {
		qSwap(first_line, last_line);
		qSwap(first_col, last_col);
	}
}

bool bssh_terminal_view::is_selected(qint64 line, int col) const {
	if (!has_selection()) return false;

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	if (line < first_line || line > last_line) return false;
	if (line == first_line && col < first_col) return false;
	if (line == last_line && col > last_col) return false;
	return true;
}

bool bssh_terminal_view::has_selection() const {
	if (m_selection_anchor_line < 0 || m_selection_focus_line < 0) return false;
	return m_selection_anchor_line != m_selection_focus_line
	        || m_selection_anchor_col != m_selection_focus_col;
}

void bssh_terminal_view::set_long_press_menu(bool enabled) {
	m_long_press->set_enabled(enabled);
}

void bssh_terminal_view::select_all() {
	/*
	 * From the BASE, not from zero.
	 *
	 * Line names carry history_base() everywhere else in this file --
	 * line_for_row builds them with it, cell_for_line and
	 * line_is_continuation subtract it -- because once the ring evicts, a
	 * name below the base does not describe anything. This function named
	 * 0 .. count+rows-1 and so selected a window that begins below the
	 * base and ends before the live screen: the front resolves to blanks
	 * and what the user is actually looking at falls outside the
	 * selection.
	 *
	 * Measured on an eight-line ring with sixty lines fed through it:
	 * Select All then Copy produced twelve characters and none of the
	 * screen. The painting agrees with the anchors, so it looks right and
	 * only the clipboard disagrees -- which is word for word how the
	 * defect that introduced base() behaved.
	 */
	const qint64 base = history_base();
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 last_line = base + history + qMax(0, m_rows - 1);

	m_selection_anchor_line = base;
	m_selection_anchor_col = 0;
	m_selection_focus_line = last_line;
	m_selection_focus_col = qMax(0, m_cols - 1);

	update();
	note_selection_changed();
}

void bssh_terminal_view::drop_evicted_selection() {
	if (!has_selection()) return;

	/*
	 * bssh_scrollback::clear says why this is necessary rather than
	 * tidy: "a cleared line is gone rather than renumbered, so anything
	 * still holding a name for one must fail to find it". A selection is
	 * exactly such a name -- two absolute line numbers -- and failing to
	 * find them is not something the view noticed. It went on reporting
	 * a selection, so Copy stayed enabled and answered with the blanks
	 * those lines resolve to, which reaches the clipboard as newlines.
	 *
	 * Only when the WHOLE selection has gone. A ring evicting its oldest
	 * line under a selection that reaches down into the live screen has
	 * taken part of it, and the part that survives still copies what it
	 * says -- dropping that would lose a selection because output
	 * arrived, which is the commonest thing a terminal does.
	 */
	const qint64 base = history_base();
	if (m_selection_anchor_line >= base || m_selection_focus_line >= base) return;

	clear_selection();
}

void bssh_terminal_view::clear_selection() {
	if (m_selection_anchor_line < 0) return;

	m_selection_anchor_line = -1;
	m_selection_focus_line = -1;
	update();
	note_selection_changed();
}

bool bssh_terminal_view::line_is_continuation(qint64 line) const {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 index = line - history_base();

	/* Evicted: gone rather than renumbered, so nothing is known of it. */
	if (index < 0) return false;

	if (index < history) {
		return m_scrollback && m_scrollback->is_continuation(int(index));
	}

	return m_emulator && m_emulator->row_is_continuation(int(index - history));
}

QString bssh_terminal_view::selection_text() const {
	if (!has_selection()) return QString();

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	QString out;
	bool continued_into_this = false;

	for (qint64 line = first_line; line <= last_line; ++line) {
		const int from = line == first_line ? first_col : 0;
		const int to = line == last_line ? last_col : m_cols - 1;

		QString text;
		for (int col = from; col <= to; ++col) {
			const bssh_cell_t &cell = cell_for_line(line, col);
			if (cell.width == 0) continue;
			text.append(QString::fromUcs4(&cell.ch, 1));
		}

		/*
		 * WRAPPED lines are rejoined, because they were never two
		 * lines. libvterm knows which rows are the tail of the row
		 * above; a newline inserted between them turns one command into
		 * two, and pasting that into a shell runs both halves -- the
		 * larger version of the padding problem below, which this
		 * function was already careful about.
		 */
		const bool continued = line < last_line && line_is_continuation(line + 1);

		/*
		 * Trailing blanks are padding, not content: a terminal line is
		 * always m_cols wide, and pasting the padding back into a shell
		 * is how a copied command arrives with a tail of spaces.
		 *
		 * Not on a row that CONTINUES, though: there the last column is
		 * where the text carries on, so a space at the end of it is
		 * content and trimming it would silently join two words.
		 */
		if (!continued) {
			while (text.endsWith(QLatin1Char(' '))) text.chop(1);
		}

		if (!out.isEmpty() || line != first_line) {
			if (!continued_into_this) out.append(QLatin1Char('\n'));
		}
		out.append(text);
		continued_into_this = continued;
	}

	return out;
}

namespace {

/*
 * Where a URL stops, which is not where the whitespace does.
 *
 * `(see https://example.com/page).' ends the address at `page', and a
 * terminal that hands the browser `page).' has found the URL and broken
 * it. Trailing sentence punctuation is never part of an address in
 * practice, and a closing bracket only is when its opener is inside the
 * URL -- which is why the count matters rather than the character:
 * Wikipedia's own addresses carry balanced parens.
 */
QString trim_url_tail(QString url) {
	forever {
		if (url.isEmpty()) break;

		const QChar last = url.at(url.size() - 1);

		if (QStringLiteral(".,;:!?'\"").contains(last)) {
			url.chop(1);
			continue;
		}

		if (last == QLatin1Char(')') || last == QLatin1Char(']')
		    || last == QLatin1Char('>')) {
			const QChar opener = last == QLatin1Char(')') ? QLatin1Char('(')
			                   : last == QLatin1Char(']') ? QLatin1Char('[')
			                                              : QLatin1Char('<');
			if (url.count(opener) < url.count(last)) {
				url.chop(1);
				continue;
			}
		}

		break;
	}
	return url;
}

} /* namespace */

QString bssh_terminal_view::link_at(const QPoint &widget_pos) const {
	qint64 line = 0;
	int col = 0;
	if (!cell_at(widget_pos, line, col)) return QString();

	/* A marked link first: it is what the program that drew it meant,
	 * and it carries a target the text need not contain. */
	if (m_emulator) {
		const bssh_cell_t &cell = cell_for_line(line, col);
		if (cell.link != 0) {
			const QString uri = m_emulator->link_uri(cell.link);
			if (!uri.isEmpty()) return uri;
		}
	}

	return url_at(line, col);
}

QString bssh_terminal_view::url_at(qint64 line, int col) const {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 base = history_base();
	const qint64 end = base + history + m_rows;

	if (line < base || line >= end) return QString();

	const int width = m_screen ? m_screen->cols() : m_cols;
	if (width <= 0) return QString();

	/*
	 * Back to the row the logical line starts on. A click lands on
	 * whichever physical row the eye found, and that is as likely to be
	 * the continuation as the start.
	 */
	qint64 first = line;
	while (first > base && line_is_continuation(first)) --first;

	QString text;
	int clicked = -1;

	qint64 phys = first;
	do {
		for (int c = 0; c < width; ++c) {
			const bssh_cell_t &cell = cell_for_line(phys, c);
			if (cell.width == 0) continue;
			if (phys == line && c == col) clicked = text.size();
			text.append(cell.ch ? QString::fromUcs4(&cell.ch, 1)
			                    : QStringLiteral(" "));
		}
		++phys;
	} while (phys < end && line_is_continuation(phys));

	if (clicked < 0) return QString();

	static const QRegularExpression pattern(
	        QStringLiteral("https?://[^\\s]+"),
	        QRegularExpression::CaseInsensitiveOption);

	QRegularExpressionMatchIterator it = pattern.globalMatch(text);
	while (it.hasNext()) {
		const QRegularExpressionMatch match = it.next();
		const QString trimmed = trim_url_tail(match.captured());
		if (trimmed.isEmpty()) continue;

		const int start = match.capturedStart();
		if (clicked < start || clicked >= start + trimmed.size()) continue;

		/*
		 * A scheme and something after it. `https://' alone is not an
		 * address, and QUrl will happily accept it.
		 */
		const QUrl url(trimmed);
		if (!url.isValid() || url.host().isEmpty()) return QString();

		return trimmed;
	}

	return QString();
}

int bssh_terminal_view::find_text(const QString &query) {
	m_find_matches.clear();
	m_current_match = -1;
	m_find_query = query;

	if (query.isEmpty()) {
		update();
		return 0;
	}

	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 base = history_base();
	const qint64 end = base + history + m_rows;

	/*
	 * The content's width, not the widget's: the scrollback and screen are
	 * the emulator's size, which a laid-out view matches but an unshown one
	 * (m_cols still zero) does not. Searching what is stored rather than
	 * what is on screen keeps find independent of the pane's pixel size.
	 */
	const int width = m_screen ? m_screen->cols() : m_cols;

	/*
	 * Search LOGICAL lines, not physical rows: a line that wrapped is
	 * stored as several rows, and a match straddling the wrap would be
	 * missed if each row were searched alone. So a row is joined with the
	 * continuation rows below it -- line_is_continuation marks them, the
	 * fact selection_text rejoins on -- and each character remembers which
	 * row and column it came from, so a match maps back to cells that may
	 * span rows.
	 */
	for (qint64 line = base; line < end;) {
		QString text;
		QList<qint64> line_of;
		QList<int> col_of;

		qint64 phys = line;
		do {
			for (int col = 0; col < width; ++col) {
				const bssh_cell_t &cell = cell_for_line(phys, col);
				if (cell.width == 0) continue;
				text.append(cell.ch ? QString::fromUcs4(&cell.ch, 1)
				                    : QStringLiteral(" "));
				line_of.append(phys);
				col_of.append(col);
			}
			++phys;
		} while (phys < end && line_is_continuation(phys));

		/* Trailing padding on the last row is not content, as selection has it. */
		while (text.endsWith(QLatin1Char(' '))) {
			text.chop(1);
			line_of.removeLast();
			col_of.removeLast();
		}

		int from = 0;
		while (from <= text.size() - query.size()) {
			const int idx = text.indexOf(query, from, Qt::CaseInsensitive);
			if (idx < 0) break;

			const int last = idx + query.size() - 1;
			find_match_t match;
			match.start_line = line_of.at(idx);
			match.start_col = col_of.at(idx);
			match.end_line = line_of.at(last);
			/*
			 * The far edge of the last matched cell: the column before the
			 * next character when it is on the same row, else the row's end
			 * -- which covers a wide glyph's second cell and a match ending
			 * at a wrap.
			 */
			match.end_col =
			        (last + 1 < col_of.size() && line_of.at(last + 1) == match.end_line)
			                ? col_of.at(last + 1) - 1
			                : width - 1;
			m_find_matches.append(match);
			from = idx + 1;
		}

		line = phys;
	}

	if (!m_find_matches.isEmpty()) {
		/* The last match is the one nearest the live screen -- where a
		 * search usually means "the most recent time this appeared". */
		m_current_match = m_find_matches.size() - 1;
		scroll_find_match_into_view(m_find_matches.at(m_current_match));
	}

	update();
	return m_find_matches.size();
}

void bssh_terminal_view::find_step(int direction) {
	if (m_find_matches.isEmpty()) return;

	const int n = m_find_matches.size();
	const int step = direction < 0 ? -1 : 1;
	m_current_match = ((m_current_match + step) % n + n) % n;

	scroll_find_match_into_view(m_find_matches.at(m_current_match));
	update();
}

void bssh_terminal_view::drop_evicted_find_matches() {
	if (m_find_matches.isEmpty()) return;

	/*
	 * A match is a pair of absolute line numbers, which is the same kind
	 * of name a selection is and fails the same way when the scrollback
	 * drops the lines under it. What a person sees is worse than a stale
	 * highlight, because the count is a NUMBER: the bar goes on saying
	 * three matches with none of them anywhere, Next walks lines that are
	 * gone, and the view scrolls to where they used to be.
	 *
	 * Dropped on the START line, unlike the selection's all-or-nothing
	 * rule, and for a reason rather than for variety. A selection is a
	 * range the user drew and half of it is still half of it; a match is
	 * an occurrence of a string, and an occurrence whose beginning has
	 * gone is not a shorter occurrence -- it is not one.
	 */
	const qint64 base = history_base();

	const int before = m_find_matches.size();
	const find_match_t current = m_current_match >= 0 && m_current_match < before
	                              ? m_find_matches.at(m_current_match)
	                              : find_match_t{ -1, 0, -1, 0 };

	QList<find_match_t> kept;
	for (const find_match_t &match : m_find_matches) {
		if (match.start_line >= base) kept.append(match);
	}

	if (kept.size() == before) return;

	m_find_matches = kept;

	/*
	 * Keep the user on the match they were on where it survived. Falling
	 * back to the last one matches what a fresh search does -- nearest
	 * the live screen -- rather than to an index that now names somebody
	 * else's match.
	 */
	m_current_match = -1;
	for (int i = 0; i < m_find_matches.size(); ++i) {
		if (m_find_matches.at(i).start_line == current.start_line
		    && m_find_matches.at(i).start_col == current.start_col) {
			m_current_match = i;
			break;
		}
	}
	if (m_current_match < 0 && !m_find_matches.isEmpty())
		m_current_match = m_find_matches.size() - 1;

	update();
	emit find_matches_changed();
}

void bssh_terminal_view::clear_find() {
	if (m_find_matches.isEmpty() && m_find_query.isEmpty()) return;

	m_find_matches.clear();
	m_current_match = -1;
	m_find_query.clear();
	update();
}

int bssh_terminal_view::current_find_index() const {
	return m_current_match < 0 ? 0 : m_current_match + 1;
}

bool bssh_terminal_view::is_find_match(qint64 line, int col) const {
	for (const find_match_t &match : m_find_matches) {
		if (line < match.start_line || line > match.end_line) continue;
		if (line == match.start_line && col < match.start_col) continue;
		if (line == match.end_line && col > match.end_col) continue;
		return true;
	}
	return false;
}

bool bssh_terminal_view::is_current_find_match(qint64 line, int col) const {
	if (m_current_match < 0) return false;

	const find_match_t &match = m_find_matches.at(m_current_match);
	if (line < match.start_line || line > match.end_line) return false;
	if (line == match.start_line && col < match.start_col) return false;
	if (line == match.end_line && col > match.end_col) return false;
	return true;
}

void bssh_terminal_view::scroll_find_match_into_view(const find_match_t &match) {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 top = history_base() + history - m_scroll_offset;
	const qint64 bottom = top + m_rows - 1;

	/* Already on screen: leave the view where it is rather than jumping. */
	if (match.start_line >= top && match.start_line <= bottom) return;

	/*
	 * Otherwise place its first row a third of the way down, so there is
	 * context above. set_scroll_offset clamps to the available history.
	 */
	const qint64 target_row = m_rows / 3;
	const qint64 offset = history_base() + history + target_row - match.start_line;
	set_scroll_offset(int(qBound(qint64(0), offset, qint64(available_history()))));
}

void bssh_terminal_view::focusInEvent(QFocusEvent *event) {
	QWidget::focusInEvent(event);

	/*
	 * On focusIn rather than on the mouse press, so every route in is
	 * covered by one hook: a click, a tap, Tab between panes, and the
	 * container's own setFocus. Handling it in mousePressEvent would have
	 * left Tab moving the keyboard without moving the border.
	 */
	emit focus_received();

	/*
	 * And ask for the keyboard, because nothing else will (sec 6.2).
	 * Here as well as on a tap: focusIn covers arriving by Tab, by the
	 * container's own setFocus and by the first touch, and a tap covers
	 * coming back after the user has dismissed the panel -- which does
	 * not move focus, so this alone would never fire again.
	 */
	request_input_panel();

	/* Blinking belongs to whoever has focus. */
	sync_blink_timer();

	/* Filled cursor here, hollow in the pane that just lost it. */
	update();
}

void bssh_terminal_view::enterEvent(QEnterEvent *event) {
	QWidget::enterEvent(event);

	/*
	 * The drag grip shows itself under the pointer, whatever the focus
	 * policy is: it is a handle on this pane rather than a consequence
	 * of focusing it, and the early return below is about focus.
	 */
	m_pointer_inside = true;
	update(pane_grip_rect().adjusted(-2, -2, 2, 2));

	if (!m_focus_follows_mouse) return;

	/*
	 * MouseFocusReason, because that is what this is: the pointer chose
	 * the pane. Anything downstream that distinguishes how focus arrived
	 * sees a mouse, which is true.
	 */
	setFocus(Qt::MouseFocusReason);
}

void bssh_terminal_view::focusOutEvent(QFocusEvent *event) {
	QWidget::focusOutEvent(event);
	emit focus_lost();

	sync_blink_timer();
	update();
}

void bssh_terminal_view::mousePressEvent(QMouseEvent *event) {
	/*
	 * When the remote has asked for the mouse, clicks are its business --
	 * unless Shift is held, the conventional override that lets a user
	 * select text out of a full-screen program (sec 6.4).
	 */
	/*
	 * Alt and the left button picks the PANE up (sec 8.4). Before
	 * anything else, including the remote's mouse reporting: a drag that
	 * only works outside vim is not a way to rearrange a window, and
	 * Alt+click is not a gesture any terminal program expects -- the meta
	 * prefix belongs to the keyboard, where it still goes.
	 *
	 * The drag is not started here. It starts when the pointer has
	 * actually moved, so that Alt+click on its own is not a gesture at
	 * all: pressing a modifier and clicking is something people do by
	 * accident, and a window that rearranges itself on one is worse than
	 * one that cannot be rearranged.
	 */
	/*
	 * The handle does the same thing without the modifier, which is the
	 * whole reason it exists: Alt+drag works and nobody discovers it.
	 * Checked first, and with a touch-sized reach around the drawn grip
	 * so a thumb does not have to be accurate to the pixel.
	 */
	const QRect grip = pane_grip_rect();
	const bool on_grip = event->button() == Qt::LeftButton && grip_visible()
	        && !grip.isNull()
	        && grip.adjusted(-8, -8, 4, 8).contains(event->pos());

	if (on_grip || (event->button() == Qt::LeftButton
	                 && (event->modifiers() & Qt::AltModifier))) {
		m_pane_drag_from = event->pos();
		m_maybe_pane_drag = true;
		event->accept();
		return;
	}

	/*
	 * A selection handle claims the press before anything else looks at
	 * it (sec 6.4), and BEFORE sec 8.7's input mode is consulted.
	 *
	 * Dragging a handle is neither scrolling nor starting a selection,
	 * so neither mode should see it -- and in the DEFAULT touch mode the
	 * scroll branch returns before any of the selection code runs, which
	 * left the handles unreachable exactly where they are for. Found by
	 * the test rather than by reading: the handle drew, the hit-test was
	 * correct, and nothing ever called it.
	 */
	if (has_selection()) {
		for (int which = 0; which < 2; ++which) {
			if (!selection_handle_rect(which == 0).contains(event->pos())) continue;

			m_dragging_handle = which;
			event->accept();
			return;
		}
	}

	const bool touch = bssh_platform::instance()
	                    && bssh_platform::instance()->is_touch_primary();
	const bool shift = take_shift_override(event->modifiers());

	/*
	 * On touch the input mode is the override that Shift is on a desktop
	 * (sec 8.7): scroll and select keep the gesture local, click lets the
	 * remote have it. A phone has no Shift, so without this a full-screen
	 * program that grabbed the mouse could be neither scrolled nor
	 * selected out of -- the defect sec 8.7 answers.
	 */
	const bool keep_local = touch ? (m_input_mode != BSSH_INPUT_CLICK) : shift;

	if (m_emulator && m_emulator->mouse_reporting() && !keep_local) {
		qint64 line = 0;
		int col = 0;
		if (cell_at(event->pos(), line, col)) {
			const int button = event->button() == Qt::RightButton ? 3
			                    : event->button() == Qt::MiddleButton ? 2 : 1;

			m_emulator->send_mouse_move(screen_row_for_line(line), col, 0);
			m_emulator->send_mouse_button(button, true, 0);

			/* The release is the press's business -- see below. */
			m_reported_button = button;
		}
		event->accept();
		return;
	}

	/*
	 * Middle-click pastes the primary selection: X11's own convention,
	 * older than any of the menus below and still the fastest way to move
	 * a word between two terminals. The selection half of it was already
	 * here -- selecting text published it -- so only the receiving half
	 * was missing, which made the convention work in one direction and
	 * look broken in the other.
	 */
	if (event->button() == Qt::MiddleButton) {
		emit primary_paste_requested();
		event->accept();
		return;
	}

	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	qint64 line = 0;
	int col = 0;
	if (!cell_at(event->pos(), line, col)) return;

	/*
	 * Ctrl+click opens a hyperlink, not a plain click. The URL comes from
	 * the remote host, so a stray tap on a line of output should not be
	 * able to launch a browser at an address someone else chose -- which
	 * is why every terminal that supports OSC 8 asks for a modifier.
	 */
	/*
	 * Ctrl+click is the pointer's way in. The long-press menu is the
	 * other, and both ask link_at so the two front doors cannot answer
	 * differently -- the same signal either way, so the window's prompt
	 * applies whichever was used.
	 */
	if (event->modifiers().testFlag(Qt::ControlModifier)) {
		const QString uri = link_at(event->pos());
		if (!uri.isEmpty()) {
			emit link_activated(uri);
			event->accept();
			return;
		}
	}

	if (touch) {
		/*
		 * A body drag scrolls this terminal's scrollback (sec 8.7's
		 * default), captured absolutely from the press so the offset
		 * cannot drift; a tap is told from a drag on release. Select mode
		 * falls through to the selection path below. Click mode has
		 * already reached the remote where it wanted the mouse, and with
		 * nothing listening a tap here just asks for the keyboard on
		 * release rather than starting a selection the mode disallows.
		 */
		if (m_input_mode == BSSH_INPUT_SCROLL) {
			m_scroll_dragging = true;
			m_scroll_drag_moved = false;
			m_scroll_drag_from = event->pos();
			m_scroll_drag_start_offset = m_scroll_offset;
			event->accept();
			return;
		}
		/*
		 * Move mode: a drag reorders this pane in the column. The reorder
		 * itself waits for release (it rebuilds the panes and would
		 * destroy this view mid-gesture), so the press only arms it.
		 */
		if (m_input_mode == BSSH_INPUT_MOVE) {
			m_move_dragging = true;
			m_move_drag_moved = false;
			m_move_drag_from = event->pos();
			event->accept();
			return;
		}
		/*
		 * Pan mode: a drag scrolls the column's viewport. Tracked from the
		 * press so each move can emit the step since the last one.
		 */
		if (m_input_mode == BSSH_INPUT_COLUMN_SCROLL) {
			m_colscroll_dragging = true;
			m_colscroll_moved = false;
			m_colscroll_last = event->pos();
			event->accept();
			return;
		}
		if (m_input_mode != BSSH_INPUT_SELECT) {
			event->accept();
			return;
		}
	}

	/*
	 * Qt delivers a triple-click as press, double-click, press -- there is
	 * no triple-click event -- so the third press is counted here. The
	 * interval is the platform's own double-click time, because a
	 * hardcoded one is wrong for anyone who has changed it, and the row
	 * must match too: three clicks that wandered onto another line are
	 * three clicks, not a line selection.
	 */
	const bool in_time = m_multi_click_timer.isValid()
	        && m_multi_click_timer.elapsed() < QApplication::doubleClickInterval();
	m_click_count = (in_time && line == m_last_click_line) ? m_click_count + 1 : 1;
	m_multi_click_timer.restart();
	m_last_click_line = line;

	if (m_click_count >= 3) {
		/*
		 * The whole line, trailing blanks included in the range but
		 * trimmed by selection_text() -- selecting a line should give
		 * the line, not the line plus the padding a terminal row always
		 * carries.
		 */
		m_selection_anchor_line = line;
		m_selection_anchor_col = 0;
		m_selection_focus_line = line;
		m_selection_focus_col = qMax(0, m_cols - 1);
		m_selecting = false;
		m_click_count = 0;

		update();
		note_selection_changed();
		event->accept();
		return;
	}

	m_selecting = true;
	m_selection_anchor_line = line;
	m_selection_anchor_col = col;
	m_selection_focus_line = line;
	m_selection_focus_col = col;

	update();
	event->accept();
}

void bssh_terminal_view::mouseMoveEvent(QMouseEvent *event) {
	/*
	 * Moving a handle moves ONE end and leaves the other where it is,
	 * which is what a handle means -- dragging the start up must not
	 * carry the end with it.
	 */
	if (m_dragging_handle >= 0) {
		qint64 line = 0;
		int col = 0;
		if (cell_at(event->pos(), line, col)) {
			qint64 first_line = 0;
			int first_col = 0;
			qint64 last_line = 0;
			int last_col = 0;
			normalised_selection(first_line, first_col, last_line, last_col);

			/*
			 * Anchor the end NOT being dragged, then move the other.
			 * Normalising afterwards lets a handle be dragged past its
			 * partner and keep working, rather than collapsing the
			 * selection the moment the two cross.
			 */
			if (m_dragging_handle == 0) {
				m_selection_anchor_line = last_line;
				m_selection_anchor_col = last_col;
			} else {
				m_selection_anchor_line = first_line;
				m_selection_anchor_col = first_col;
			}
			m_selection_focus_line = line;
			m_selection_focus_col = col;

			update();
			note_selection_changed();
		}
		event->accept();
		return;
	}

	/*
	 * The pane drag begins once the pointer has moved far enough to be a
	 * drag rather than a click -- Qt's own threshold, so it agrees with
	 * every other drag the user has ever started.
	 *
	 * The view emits and does not act. It has no idea which session it is
	 * showing, which window holds it, or what a drop would mean; the
	 * container knows the first and the window knows the rest (sec 8.4's
	 * "a window asks the registry and never reaches into another
	 * window"). One flag, one signal.
	 */
	if (m_maybe_pane_drag) {
		if ((event->pos() - m_pane_drag_from).manhattanLength()
		    < QApplication::startDragDistance()) {
			event->accept();
			return;
		}

		m_maybe_pane_drag = false;
		emit pane_drag_requested();
		event->accept();
		return;
	}

	if (m_scroll_dragging) {
		const QPoint delta = event->pos() - m_scroll_drag_from;
		if (!m_scroll_drag_moved
		    && delta.manhattanLength() >= QApplication::startDragDistance())
			m_scroll_drag_moved = true;

		if (m_scroll_drag_moved && m_cell_height > 0) {
			/*
			 * Content follows the finger: dragging down reveals the
			 * lines above, which is further back in the scrollback, so
			 * a downward delta raises the offset. Measured from the
			 * offset at the press, so a slow drag cannot accumulate
			 * rounding into a drift.
			 */
			const int lines = delta.y() / m_cell_height;
			set_scroll_offset(m_scroll_drag_start_offset + lines);
		}

		event->accept();
		return;
	}

	if (m_move_dragging) {
		/*
		 * Only tell a drag from a tap here; the reorder is decided on
		 * release, from the net direction the finger travelled. Doing it
		 * live would reorder and rebuild the panes under the finger.
		 */
		if (!m_move_drag_moved
		    && (event->pos() - m_move_drag_from).manhattanLength()
		           >= QApplication::startDragDistance())
			m_move_drag_moved = true;
		event->accept();
		return;
	}

	if (m_colscroll_dragging) {
		/*
		 * Emit the step since the last move and remember where the finger
		 * is now: the container has the scroll area, so the view reports
		 * deltas and keeps no offset of its own.
		 */
		const int dy = event->pos().y() - m_colscroll_last.y();
		m_colscroll_last = event->pos();
		if (dy != 0) {
			m_colscroll_moved = true;
			emit column_scroll_requested(dy);
		}
		event->accept();
		return;
	}

	if (!m_selecting) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	qint64 line = 0;
	int col = 0;
	if (cell_at(event->pos(), line, col)) {
		m_selection_focus_line = line;
		m_selection_focus_col = col;
		update();
	}

	event->accept();
}

void bssh_terminal_view::mouseReleaseEvent(QMouseEvent *event) {
	if (m_dragging_handle >= 0) {
		m_dragging_handle = -1;
		event->accept();
		return;
	}

	/* An Alt+click that never moved is not a gesture. */
	if (m_maybe_pane_drag) {
		m_maybe_pane_drag = false;
		event->accept();
		return;
	}

	if (m_scroll_dragging) {
		const bool tap = !m_scroll_drag_moved;
		m_scroll_dragging = false;

		/*
		 * A press that never became a drag is a tap, and a tap asks for
		 * the keyboard -- the same thing the no-selection release below
		 * does, said here because the scroll path returns before it.
		 */
		if (tap) request_input_panel();
		event->accept();
		return;
	}

	if (m_move_dragging) {
		const bool moved = m_move_drag_moved;
		const int dy = event->pos().y() - m_move_drag_from.y();
		m_move_dragging = false;

		/*
		 * A drag with vertical travel reorders by one place -- up the
		 * screen toward the top of the column, down toward the bottom.
		 * A press that never moved is a tap and asks for the keyboard,
		 * as the scroll path above does; a purely sideways drag reorders
		 * nothing and is left alone.
		 */
		if (moved && dy != 0) emit pane_move_requested(dy < 0 ? -1 : 1);
		else if (!moved) request_input_panel();
		event->accept();
		return;
	}

	if (m_colscroll_dragging) {
		const bool tap = !m_colscroll_moved;
		m_colscroll_dragging = false;

		/* A press that never scrolled is a tap, and asks for the keyboard
		 * as the other drag paths do. */
		if (tap) request_input_panel();
		else emit column_scroll_settled();
		event->accept();
		return;
	}

	/*
	 * A release answers the PRESS. It used to hardcode button 1 and to
	 * re-read Shift at release time, and both are wrong for the same
	 * reason: libvterm keeps a button MASK, so a release naming a button
	 * that was never pressed clears the wrong bit and leaves the real one
	 * set. After one right-click inside a mouse-reporting program the
	 * remote never saw the release, and every later right-click hit
	 * libvterm's "no change" early return and vanished -- right-click
	 * worked exactly once per session.
	 *
	 * Not re-checked against mouse_reporting() either. If the remote
	 * turned the mode off between the press and the release, the release
	 * still has to go: libvterm drops the bytes but clears the mask
	 * first, which is what stops the bit being stranded.
	 */
	if (m_reported_button != 0) {
		if (m_emulator) m_emulator->send_mouse_button(m_reported_button, false, 0);
		m_reported_button = 0;
		event->accept();
		return;
	}

	if (!m_selecting) {
		/*
		 * A tap that selected nothing is somebody asking to type. This
		 * is where QLineEdit asks too, and for the same reason: the
		 * panel can be dismissed without focus moving, so a release is
		 * the only event left that means "I want the keyboard back".
		 */
		request_input_panel();
		QWidget::mouseReleaseEvent(event);
		return;
	}

	m_selecting = false;
	note_selection_changed();
	event->accept();
}

void bssh_terminal_view::request_input_panel() {
	/*
	 * Said first and unconditionally: the gesture happened, whatever is
	 * going to answer it. The built-in keyboard is hidden by its own key
	 * (sec 6.2) and a tap is what brings it back, so a report gated on
	 * the system panel's attribute would be silent in the one mode that
	 * needs it.
	 */
	emit input_panel_requested();

	/*
	 * And only then the SYSTEM panel, when this widget would actually
	 * receive what it sends. The built-in keyboard clears
	 * WA_InputMethodEnabled precisely to decline the system one, and
	 * asking for it anyway would stack two keyboards on a screen that had
	 * room for neither.
	 */
	if (!testAttribute(Qt::WA_InputMethodEnabled)) return;

	QGuiApplication::inputMethod()->show();
}

QRect bssh_terminal_view::selection_handle_rect(bool start) const {
	const bssh_platform *platform = bssh_platform::instance();
	if (!platform || !platform->is_touch_primary()) return QRect();
	if (!has_selection()) return QRect();

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	/*
	 * The start handle hangs under the LEFT edge of the first cell and
	 * the end handle under the RIGHT edge of the last, which is where
	 * every touch platform puts them -- a handle over the text would
	 * cover the thing being selected.
	 */
	const QRect cell = start ? cell_rect(first_line, first_col)
	                         : cell_rect(last_line, last_col);
	if (!cell.isValid()) return QRect();

	const int side = BSSH_TOUCH_TARGET_DP;
	const int x = (start ? cell.left() : cell.right() + 1) - side / 2;
	return QRect(x, cell.bottom(), side, side);
}

void bssh_terminal_view::select_word_at(qint64 line, int col) {
	/*
	 * Word selection, using the same word characters a shell does -- a
	 * double-click on a path should take the whole path, which is most of
	 * what anyone double-clicks in a terminal for.
	 */
	const QString word_chars = QStringLiteral("_-./~:@");
	int from = col;
	int to = col;

	while (from > 0) {
		const QChar previous = QChar(cell_for_line(line, from - 1).ch);
		if (!previous.isLetterOrNumber() && !word_chars.contains(previous)) break;
		--from;
	}
	while (to < m_cols - 1) {
		const QChar next = QChar(cell_for_line(line, to + 1).ch);
		if (!next.isLetterOrNumber() && !word_chars.contains(next)) break;
		++to;
	}

	m_selection_anchor_line = line;
	m_selection_anchor_col = from;
	m_selection_focus_line = line;
	m_selection_focus_col = to;
	m_selecting = false;

	update();
	note_selection_changed();
}

void bssh_terminal_view::mouseDoubleClickEvent(QMouseEvent *event) {
	qint64 line = 0;
	int col = 0;
	if (event->button() != Qt::LeftButton || !cell_at(event->pos(), line, col)) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

	select_word_at(line, col);

	/*
	 * The double-click counts itself. Qt delivers press, double-click,
	 * press for a triple-click, and a double-click does NOT arrive as a
	 * press -- so counting only presses leaves the third one at two, and
	 * the line selection never fires.
	 */
	m_click_count = 2;
	m_multi_click_timer.restart();
	m_last_click_line = line;

	update();
	note_selection_changed();
	event->accept();
}

void bssh_apply_appearance(bssh_terminal_view *view, const QJsonObject &settings) {
	if (!view) return;

	view->set_colour_scheme(settings.value(QStringLiteral("colour_scheme")).toString());
	view->set_cursor_style(settings.value(QStringLiteral("cursor_style")).toString());
	view->set_cursor_blink(settings.value(QStringLiteral("cursor_blink")).toBool());

	/*
	 * The font and its size go together in one call: they are two halves
	 * of one cell measurement, and applying them separately means two
	 * grid recomputations and two window-change messages for what the
	 * user did as a single change.
	 */
	const QString family = settings.value(QStringLiteral("font_family")).toString();
	const int size = settings.value(QStringLiteral("font_size")).toInt();

	view->set_font_id(family, size);
}

void bssh_terminal_view::note_content_changed() {
	/*
	 * The revision the accessibility layer caches its flattened text
	 * against. It was bumped here and nowhere else, so the one content
	 * change that damages nothing -- set_scrollback_lines() trimming the
	 * ring -- left a reader describing lines that had been dropped, with
	 * every offset past the trim wrong. characterRect made it worse
	 * rather than better: it asks the view for geometry using
	 * history_base(), which the trim DOES move, so the rectangles
	 * pointed at the wrong rows too.
	 *
	 * That is sec 9.5's scrollbar defect exactly, arriving at the second
	 * consumer of one signal: scrollback_changed was wired for the bar
	 * and not for this.
	 */
	++m_content_revision;

	/*
	 * Entering the alternate screen takes the history away, so an offset
	 * that was legal a moment ago need not be now -- and a view left
	 * scrolled back when a program starts would keep the shell on screen
	 * over it.
	 */
	set_scroll_offset(m_scroll_offset);

	/* Output scrolling off the top is more history to point at. */
	sync_scrollbar();
	update();

	/*
	 * Only when something is listening. QAccessible::isActive() is false
	 * with no assistive client attached, which is the common case, and
	 * flattening the screen for an event nobody receives would put that
	 * cost on every damaged() -- the signal this project has spent two
	 * commits making cheap.
	 */
	if (!QAccessible::isActive()) return;

	QAccessibleTextUpdateEvent event(this, 0, QString(), QString());
	QAccessible::updateAccessibility(&event);

	/*
	 * And the CARET, which was never announced either.
	 *
	 * This is the same defect note_selection_changed() records and fixes
	 * one function below, left standing one function above it:
	 * cursorPosition() answers correctly whenever it is asked, and
	 * nothing ever said it had moved, so a client learned of a move only
	 * by re-querying. For a terminal that is the worse half to leave out
	 * -- the caret is where your own typing appears, and following it is
	 * how a reader follows you typing.
	 *
	 * libvterm's movecursor callback routes through notify_damaged() like
	 * everything else, so a caret move reached a client as "the text
	 * changed" and never as "the caret moved".
	 *
	 * Offsets come from the accessible interface rather than being
	 * recomputed here, for the reason given below: the mapping from a
	 * cell to an offset in the flattened text is that class's, and a
	 * second copy is how the two stop agreeing. flatten() is cached on
	 * the content revision, so asking costs one flatten per damage and
	 * only while a client is attached.
	 *
	 * Sent only when it CHANGED. Output that does not move the cursor is
	 * common -- a scrolling log leaves it on the last line -- and a
	 * caret event per damage would be a reader announcing a move that
	 * did not happen.
	 */
	QAccessibleInterface *self = QAccessible::queryAccessibleInterface(this);
	QAccessibleTextInterface *as_text = self ? self->textInterface() : nullptr;
	if (!as_text) return;

	const int offset = as_text->cursorPosition();
	if (offset == m_last_cursor_offset) return;
	m_last_cursor_offset = offset;

	QAccessibleTextCursorEvent moved(this, offset);
	QAccessible::updateAccessibility(&moved);
}

void bssh_terminal_view::on_screen_damaged() {
	note_content_changed();
}

void bssh_terminal_view::note_selection_changed() {
	emit selection_changed();

	/*
	 * And the reader, which was never told. sec 14's M11 exposes the
	 * selection through the text interface -- selectionCount() and
	 * selection() answer correctly whenever they are asked -- but
	 * nothing ever announced a change, so an assistive client learned of
	 * one only if it happened to re-query. For a terminal that is the
	 * wrong half to leave out: the selection is what the user is about
	 * to copy, and being told what it now covers is the whole feedback.
	 *
	 * The one accessibility event this program emitted was the text
	 * update in note_content_changed(). This is the second.
	 *
	 * Offsets come from the accessible interface rather than being
	 * recomputed here, because the mapping from a selection in cells to
	 * an offset in the flattened history-and-screen text is that class's
	 * and a second copy is how the two stop agreeing (sec 8.4's rule
	 * about one place computing one thing).
	 *
	 * Guarded like the text event: QAccessible::isActive() is false with
	 * no client attached, which is the common case, and flattening for
	 * an event nobody receives would put that cost on every drag.
	 */
	if (!QAccessible::isActive()) return;

	QAccessibleInterface *self = QAccessible::queryAccessibleInterface(this);
	QAccessibleTextInterface *as_text = self ? self->textInterface() : nullptr;
	if (!as_text) return;

	int start = 0;
	int end = 0;
	if (as_text->selectionCount() > 0) as_text->selection(0, &start, &end);

	QAccessibleTextSelectionEvent event(this, start, end);
	QAccessible::updateAccessibility(&event);
}

void bssh_terminal_view::set_raise_contrast(bool raise) {
	if (m_raise_contrast == raise) return;

	m_raise_contrast = raise;
	update();
}

void bssh_terminal_view::set_text_brightness(int percent) {
	const int wanted = qBound(10, percent, 100);
	if (m_text_brightness == wanted) return;

	m_text_brightness = wanted;
	update();
}

bool bssh_terminal_view::selection_range(qint64 &first_line, int &first_col,
                                          qint64 &last_line, int &last_col) const {
	if (!has_selection()) return false;

	normalised_selection(first_line, first_col, last_line, last_col);
	return true;
}
