#include "ui/terminal_view.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QUrl>

#include "ui/terminal_accessible.h"

#include <QAccessible>

#include <QFontDatabase>
#include <QPen>
#include <QFontMetricsF>

#include <algorithm>
#include <QContextMenuEvent>
#include <QPainter>
#include <QTimer>
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
      m_screen(nullptr),
      m_images(nullptr),
      m_scrollback(nullptr),
      m_scroll_offset(0),
      m_scrollbar(nullptr),
      m_scrollbar_position(default_scrollbar_position()),
      m_focus_border(false),
      m_click_count(0),
      m_last_click_line(-1),
      m_selecting(false),
      m_long_press(nullptr),
      m_selection_anchor_line(-1),
      m_selection_anchor_col(0),
      m_selection_focus_line(-1),
      m_selection_focus_col(0),
      m_emulator(nullptr),
      m_input_router(nullptr),
      m_font_size(BSSH_DEFAULT_POINT_SIZE),
      m_cursor_style(QStringLiteral("block")),
      m_cursor_on(true),
      m_blink_timer(nullptr),
      m_background(QColor(0x1c, 0x1c, 0x1c)),
      m_foreground(QColor(0xd0, 0xd0, 0xd0)),
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
		/* The same guard the right button gets. */
		if (m_emulator && m_emulator->mouse_reporting()) return;
		emit context_menu_requested(at);
	});

	const bssh_platform *platform = bssh_platform::instance();
	const bool compact = platform && platform->is_touch_primary();

	m_font_id = bssh_font_catalog::instance().default_id(compact);
	m_font = bssh_font_catalog::make_font(
	        bssh_font_catalog::instance().family_for(m_font_id), m_font_size);

	setFocusPolicy(Qt::StrongFocus);

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
	 * platform's default", not "no font": every shipped preset leaves the
	 * font unset (sec 10.4), and treating that as a request would drop
	 * into the fallback chain and render every default host in DejaVu.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	m_font_id = catalog.effective_id(id, platform && platform->is_touch_primary());

	m_font_size = point_size > 0 ? point_size : BSSH_DEFAULT_POINT_SIZE;

	m_font = bssh_font_catalog::make_font(catalog.family_for(m_font_id), m_font_size);

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
	if (blink == (m_blink_timer != nullptr)) return;

	if (!blink) {
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

	QFont font = bssh_font_catalog::make_font(family, m_font_size);
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

void bssh_terminal_view::attach_screen(const bssh_screen *screen) {
	m_screen = screen;
	update();
}

void bssh_terminal_view::attach_images(const bssh_image_store *images) {
	m_images = images;
	update();
}

void bssh_terminal_view::attach_scrollback(const bssh_scrollback *scrollback) {
	m_scrollback = scrollback;
	update();
}

void bssh_terminal_view::attach_emulator(bssh_emulator *emulator) {
	if (m_emulator == emulator) return;

	disconnect(m_alternate_screen_connection);
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

void bssh_terminal_view::wheelEvent(QWheelEvent *event) {
	const int notches = event->angleDelta().y() / 120;

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
			m_emulator->send_mouse_move(int(line - line_for_row(0)), col, 0);

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

		const QRect target(placement.col * m_cell_width,
		                    row * m_cell_height,
		                    placement.cols * m_cell_width,
		                    placement.rows * m_cell_height);
		painter.drawImage(target, placement.image);
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
	if (index < 16) return QColor(m_scheme.ansi[index]);

	if (index < 232) {
		const int offset = index - 16;
		const int steps[6] = {0, 95, 135, 175, 215, 255};
		return QColor(steps[(offset / 36) % 6], steps[(offset / 6) % 6], steps[offset % 6]);
	}

	const int grey = 8 + (index - 232) * 10;
	return QColor(grey, grey, grey);
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

} /* namespace */

void bssh_terminal_view::set_default_scrollbar_position(scrollbar_position_t where) {
	default_position = where;
}

bssh_terminal_view::scrollbar_position_t bssh_terminal_view::default_scrollbar_position() {
	return default_position;
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

void bssh_terminal_view::dragEnterEvent(QDragEnterEvent *event) {
	/*
	 * Only what this view can turn into text. A tab dragged from another
	 * window is a SESSION payload (sec 8.4) and is refused here so that
	 * it reaches the window, which is what knows how to adopt one.
	 */
	if (!text_for_drop(event->mimeData()).isEmpty()) event->acceptProposedAction();
}

void bssh_terminal_view::dropEvent(QDropEvent *event) {
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

void bssh_terminal_view::paintEvent(QPaintEvent *event) {
	Q_UNUSED(event);

	QPainter painter(this);
	painter.fillRect(rect(), m_background);
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
			 * Selection inverts rather than tinting: it stays visible
			 * whatever colours the cell already carries, which a fixed
			 * highlight colour cannot promise against 256 possible
			 * backgrounds.
			 */
			if (is_selected(history - offset + row, col)) std::swap(fg, bg);

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
				if (bg != m_background) {
					bg = QColor(bssh_dim(bg.rgb(), m_text_brightness));
				}
			}

			const int x = col * m_cell_width;
			const int y = row * m_cell_height;
			const int width = qMax(1, int(cell.width)) * m_cell_width;

			if (bg != m_background) painter.fillRect(x, y, width, m_cell_height, bg);

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
		const QColor cursor_colour(bssh_dim(m_scheme.cursor, m_text_brightness));

		if (m_cursor_style == QStringLiteral("underline")) {
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
				painter.setPen(m_background);
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

}

/*
 * Absolute line numbering: history lines come first, the live screen
 * continues straight on from them. One coordinate space for both is what
 * lets a selection span the fold and survive scrolling.
 */
qint64 bssh_terminal_view::line_for_row(int row) const {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	return history - m_scroll_offset + row;
}

const bssh_cell_t &bssh_terminal_view::cell_for_line(qint64 line, int col) const {
	static const bssh_cell_t blank;

	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	if (line < 0) return blank;

	if (line < history) {
		const QList<bssh_cell_t> &stored = m_scrollback->line(int(line));
		return col >= 0 && col < stored.size() ? stored.at(col) : blank;
	}

	if (!m_screen) return blank;
	return m_screen->cell(int(line - history), col);
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
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	const qint64 last_line = history + qMax(0, m_rows - 1);

	m_selection_anchor_line = 0;
	m_selection_anchor_col = 0;
	m_selection_focus_line = last_line;
	m_selection_focus_col = qMax(0, m_cols - 1);

	update();
	emit selection_changed();
}

void bssh_terminal_view::clear_selection() {
	if (m_selection_anchor_line < 0) return;

	m_selection_anchor_line = -1;
	m_selection_focus_line = -1;
	update();
	emit selection_changed();
}

QString bssh_terminal_view::selection_text() const {
	if (!has_selection()) return QString();

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	QStringList lines;
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
		 * Trailing blanks are padding, not content: a terminal line is
		 * always m_cols wide, and pasting the padding back into a shell
		 * is how a copied command arrives with a tail of spaces.
		 */
		while (text.endsWith(QLatin1Char(' '))) text.chop(1);
		lines.append(text);
	}

	return lines.join(QLatin1Char('\n'));
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
}

void bssh_terminal_view::enterEvent(QEnterEvent *event) {
	QWidget::enterEvent(event);

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
}

void bssh_terminal_view::mousePressEvent(QMouseEvent *event) {
	/*
	 * When the remote has asked for the mouse, clicks are its business --
	 * unless Shift is held, the conventional override that lets a user
	 * select text out of a full-screen program (sec 6.4).
	 */
	const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
	if (m_emulator && m_emulator->mouse_reporting() && !shift) {
		qint64 line = 0;
		int col = 0;
		if (cell_at(event->pos(), line, col)) {
			m_emulator->send_mouse_move(int(line - line_for_row(0)), col, 0);
			m_emulator->send_mouse_button(event->button() == Qt::RightButton ? 3
			                               : event->button() == Qt::MiddleButton ? 2 : 1,
			                               true, 0);
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
	if (event->modifiers().testFlag(Qt::ControlModifier) && m_emulator) {
		const bssh_cell_t &cell = cell_for_line(line, col);
		if (cell.link != 0) {
			const QString uri = m_emulator->link_uri(cell.link);
			if (!uri.isEmpty()) {
				emit link_activated(uri);
				event->accept();
				return;
			}
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
		emit selection_changed();
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
	if (m_emulator && m_emulator->mouse_reporting()
	    && !event->modifiers().testFlag(Qt::ShiftModifier)) {
		m_emulator->send_mouse_button(1, false, 0);
		event->accept();
		return;
	}

	if (!m_selecting) {
		QWidget::mouseReleaseEvent(event);
		return;
	}

	m_selecting = false;
	emit selection_changed();
	event->accept();
}

void bssh_terminal_view::mouseDoubleClickEvent(QMouseEvent *event) {
	qint64 line = 0;
	int col = 0;
	if (event->button() != Qt::LeftButton || !cell_at(event->pos(), line, col)) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

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
	emit selection_changed();
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

void bssh_terminal_view::on_screen_damaged() {
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
