#ifndef BSSH_TERMINAL_VIEW_H
#define BSSH_TERMINAL_VIEW_H

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QWidget>

#include <functional>

#include "model/multiplexer.h"
#include "term/colour_scheme.h"
#include "term/image_store.h"
#include "term/screen.h"
#include "term/scrollback.h"
#include "ui/input_mode.h"

class QDragEnterEvent;
class QDropEvent;
class QMimeData;

/*
 * The drag payload a torn-out tab carries (sec 8.4).
 *
 * Declared with the VIEW rather than with the window because the pane is
 * where a drop lands, and a view must not depend on the window it sits
 * in. The window delegates to this.
 */
QString bssh_session_mime_type();

/*
 * The payload a session drag carries: "<pid>:<registry index>", and after
 * them, when the session can be asked for from another process, its
 * handoff id and where this process listens: ":<id>:<path>" (sec 8.4).
 *
 * The process is part of it because an index means something only in the
 * registry that issued it -- a bare index, dropped on another BeerSSH,
 * named one of THAT process's sessions and moved it. The id and the path
 * are what that other BeerSSH needs instead: whom to ask, and for what.
 * The path goes last because it is the one part that might hold a colon.
 */
QByteArray bssh_session_payload(int registry_index, const QString &handoff_id = QString(),
                                const QString &handoff_path = QString());

/*
 * For a session drag from ANOTHER process that can be asked for: where it
 * listens and the session's handoff id. False for this process's own
 * payloads, and for another's that carry neither.
 */
bool bssh_session_payload_handoff(const QMimeData *mime, QString *path, QString *handoff_id);

/*
 * The registry index a payload names in THIS process, or -1. `foreign` is
 * set when the payload is a session drag from another process -- including
 * a bare index, the format before the process was stamped in.
 */
int bssh_session_payload_index(const QMimeData *mime, bool *foreign);

/*
 * Which part of a pane a dropped pane lands on (sec 8.3), as
 * bssh_layout_manager::drop_edge_t: the four edges split the pane that
 * way and the middle swaps. A band a quarter of the pane deep along each
 * edge; the rest is the middle, and where two bands meet in a corner the
 * nearer edge wins. Pure, so the regions are asserted rather than
 * eyeballed.
 */
int bssh_drop_edge_at(const QPoint &at, const QSize &pane);

/*
 * What a drop on a pane can come to besides the five zones (sec 8.4's
 * picture): CANCEL does nothing and says so -- the pane being carried, for
 * one -- and REFUSED is not accepted at all, so the pointer shows no-drop.
 */
const int BSSH_DROP_CANCEL = -2;
const int BSSH_DROP_REFUSED = -3;

/*
 * A thin band along each edge of a pane that shows no tab bar: a terminal
 * dropped there joins the pane as a tab and the tab bar is put on that side
 * -- the holder, 2026-10-29: "if there is no tab bar then a terminal should
 * be droppable on any one of the four sides to create a tab bar there in
 * addition to the other drop zones we already have" (sec 8.7a). BASE plus
 * the drop_edge_t of the side.
 */
const int BSSH_DROP_TAB_BASE = 10;
inline bool bssh_is_tab_drop(int zone) { return zone >= BSSH_DROP_TAB_BASE && zone < BSSH_DROP_TAB_BASE + 4; }

/*
 * The zone under `at`: bssh_drop_edge_at's five, or a tab zone where the
 * point is within `band` pixels of the edge it is nearest. Whether a tab
 * zone is honoured is the planner's to say.
 */
int bssh_drop_zone_at(const QPoint &at, const QSize &pane, int band);

/* The tab zones' width for a pane whose text is `cell_height` tall. */
int bssh_drop_band(int cell_height);

/*
 * The window's answer to "dropped here, at this zone, what happens": a
 * drop_edge_t, BSSH_DROP_CANCEL or BSSH_DROP_REFUSED. The pane draws what
 * this says and the window does what it says, so the two cannot disagree.
 */
typedef std::function<int(const QMimeData *mime, int edge)> bssh_drop_planner_t;

class QMouseEvent;
class QPainter;
class QScrollBar;
#include "ui/long_press.h"
class QWheelEvent;
class bssh_emulator;
class bssh_input_router;

/*
 * bssh_terminal_view -- the character-cell grid widget (project.md sec 7).
 *
 * At M0 it renders an empty grid and nothing else: no emulator, no
 * session, no scrollback. What it DOES already do is own the cell metrics,
 * because everything downstream depends on them being right --
 * cols x rows drives the SSH window-change message (sec 7), the split
 * dividers snap to whole cells (sec 8.3), and the grid is snapped to
 * integer pixel metrics so 200 columns don't accumulate rounding error
 * (sec 9.5).
 *
 * It attaches to a session; it does not own one. Session state lives in
 * the registry, never in a widget (sec 3, core principle 2), which is
 * what lets this view be destroyed and recreated freely at M7/M8.
 */
class bssh_multiplexer_drawer;
class bssh_pane_corner;

class bssh_terminal_view : public QWidget {
	Q_OBJECT

public:
	explicit bssh_terminal_view(QWidget *parent = nullptr);

	int cols() const { return m_cols; }
	int rows() const { return m_rows; }

	/* Cell metrics in pixels -- the emulator needs these to size images. */
	/*
	 * sec 9.3's contrast floor, applied when drawing rather than to the
	 * palette (see bssh_raise_contrast). Set from the device setting.
	 */
	/*
	 * Bumped whenever what the widget would draw changes. The
	 * accessibility interface flattens the screen AND the scrollback into
	 * one string, which is hundreds of kilobytes on a full buffer and far
	 * too much to rebuild on every query a screen reader makes -- so it
	 * caches against this.
	 */
	quint64 content_revision() const { return m_content_revision; }

	void set_raise_contrast(bool raise);

	/*
	 * How bright drawn text is, 10-100 (sec 9.3). The background is not
	 * touched, so black stays black and only the light on it comes down --
	 * which is what a dark room wants.
	 */
	void set_text_brightness(int percent);
	int text_brightness() const { return m_text_brightness; }

	/*
	 * The pointer entering this pane gives it the keyboard (sec 8.3).
	 *
	 * Implemented by asking for focus rather than by telling the container
	 * directly, so hover goes down the SAME path a click does -- the
	 * border, the status line, what copy and paste act on, and sec 7.1's
	 * focus reporting all follow from focusInEvent and none of them needs
	 * to learn a second way of being focused.
	 */
	void set_focus_follows_mouse(bool follows) { m_focus_follows_mouse = follows; }
	bool raise_contrast() const { return m_raise_contrast; }

	int cell_width() const { return m_cell_width; }
	int cell_height() const { return m_cell_height; }

	/*
	 * The resolved base font the grid draws with, exposed so a test can
	 * confirm what the view actually built -- the font is painted rather
	 * than set on the widget, so QWidget::font() does not answer for it.
	 */
	const QFont &display_font() const { return m_font; }

	/*
	 * How big this widget has to be to hold a grid of cols x rows
	 * (project.md sec 8.6's --geometry).
	 *
	 * Asked of the view rather than computed by the caller, because the
	 * answer is not just the cell metrics: a pane keeps a pixel clear at
	 * each edge for the focused-pane border (sec 8.3), so a window sized
	 * to exactly cols * cell_width is a window one column short. That
	 * inset is this class's, and a second copy of it in the window would
	 * be a second thing to keep in step -- which is how the first version
	 * of --geometry produced a 99-column terminal for `100x30`.
	 */
	QSize size_for_grid(int cols, int rows) const;

	/*
	 * The smallest this is still a terminal (sec 8.2).
	 *
	 * The view declared no minimum at all, so Qt gave it zero and the
	 * window inherited a minimum made entirely of chrome: a menu bar, a
	 * tab strip and a status line with no terminal between them. Rendered
	 * at its own claimed minimum the window contained no grid whatever.
	 * A minimum that small is not a harmless number -- a window manager
	 * honouring it squeezes rows past their minimums until they draw on
	 * top of each other, which sec 8.3 already records for other layouts.
	 */
	QSize minimumSizeHint() const override;

	/*
	 * Where this pane's scrollbar sits, or that it has none (sec 8.3).
	 *
	 * Konsole's three answers, and the reason it is a real QScrollBar
	 * rather than something painted: a scrollbar is the one part of a
	 * terminal a user expects to behave exactly like every other
	 * scrollbar on their desktop -- the wheel over it, the page-jump
	 * click, the drag, the theme's own arrows.
	 *
	 * It lives INSIDE the view rather than beside it, because the view is
	 * what owns the grid arithmetic: the columns it can show are the ones
	 * left after the bar has taken its width, and the code that maps a
	 * click to a cell has to start where the grid starts. Putting the bar
	 * in a wrapper widget would have split that arithmetic across two
	 * classes.
	 */
	enum scrollbar_position_t {
		SCROLLBAR_NONE,
		SCROLLBAR_RIGHT,
		SCROLLBAR_LEFT
	};

	/*
	 * What a drop should put into the terminal (sec 6.4), or empty for a
	 * payload this is not willing to take.
	 *
	 * Static and pure, so the quoting can be tested without a drag: what
	 * makes this worth testing is not the plumbing but the SHELL
	 * QUOTING -- a path with a space in it pasted raw is two arguments,
	 * and a path with a quote in it is a syntax error at best.
	 */
	static QString text_for_drop(const QMimeData *mime);

	void set_scrollbar_position(scrollbar_position_t where);
	scrollbar_position_t scrollbar_position() const { return m_scrollbar_position; }

	/*
	 * What a view is BUILT with, so that none of them ever lives a moment
	 * at the wrong width.
	 *
	 * Views are destroyed and rebuilt on every layout change (sec 8.2),
	 * and the device settings used to be applied a beat afterwards -- so
	 * a new pane came up with no bar, measured its columns, told the
	 * remote, and then acquired one and did it again. The second resize
	 * reflowed the text: a tab torn into a new window pushed the line
	 * that was on screen into the scrollback, which is what caught it.
	 */
	static void set_default_scrollbar_position(scrollbar_position_t where);
	static scrollbar_position_t default_scrollbar_position();

	/*
	 * Ligatures for the terminal font (sec 9.2), a device default set from
	 * the settings and read by every view's font build -- the same shape
	 * as the scrollbar default, and for the same reason: the pane
	 * container builds views in a loop that has no business reading
	 * settings.
	 */
	static void set_default_font_ligatures(bool ligatures);
	static bool default_font_ligatures();

	/*
	 * Whether the remote's DECSCUSR is allowed to override the preset
	 * cursor shape (sec 6.4). On by default -- that is how vim's cursor
	 * works -- and a device setting, like the two above, so a user who
	 * wants one fixed cursor can turn it off process-wide. Read live when
	 * a shape change arrives, so toggling it takes effect at once.
	 */
	static void set_honour_remote_cursor_shape(bool honour);
	static bool honour_remote_cursor_shape();

	/*
	 * The curated set's way in (project.md sec 9): a stable font id and a
	 * point size, resolved through bssh_font_catalog rather than by naming
	 * a family here. Going through the catalog is what gets the sec 9.3
	 * fallback chain, so a CJK character or a box-drawing glyph the chosen
	 * face lacks is served by DejaVu or Noto instead of by whatever Qt
	 * would have merged in.
	 */
	void set_font_id(const QString &id, int point_size);

	QString font_id() const { return m_font_id; }

	/*
	 * Zoom (sec 9.5): a per-session step ON TOP of the size the host
	 * resolved to, rather than a second size. Kept as a delta so that a
	 * preset edit arriving while a pane is zoomed still moves the base --
	 * storing the zoomed size instead would make the next resolve either
	 * lose the zoom or silently overwrite the preset's answer.
	 *
	 * The step is one point, and the sum is bounded by the same limits
	 * sec 10.5 already clamps a preset's font size to: a zoom that could
	 * reach a size the settings layer refuses would be a second opinion
	 * about what this program can render.
	 */
	void set_font_zoom(int steps);
	int font_zoom() const { return m_font_zoom; }

	/* What is actually being drawn: the resolved size plus the zoom. */
	int font_point_size() const;

	/*
	 * Non-empty when the chosen font is not on this device and something
	 * else is being drawn (sec 9.4). The window shows it; the view only
	 * knows it, because a terminal that pops up its own banner is a
	 * terminal that covers the output it exists to display.
	 */
	QString font_substitution_note() const;

	/* The sixteen ANSI colours, the default fg/bg and the cursor colour
	 * (sec 10.2). Empty selects the default scheme. */
	void set_colour_scheme(const QString &id);
	QString colour_scheme() const { return m_scheme_id; }

	/*
	 * The two colours this pane is actually painted in, after the
	 * contrast floor and the brightness slider have had their say --
	 * which is what makes them the honest answer to OSC 10 and OSC 11
	 * (sec 7.1). A client compositing an image's alpha against the
	 * background wants the colour on screen, not the one in the scheme
	 * file.
	 */
	QColor terminal_foreground() const { return m_foreground; }
	QColor terminal_background() const { return m_background; }

	/*
	 * DECSCNM reverse video (sec 6.4): the remote flips the whole screen's
	 * foreground and background. The scheme colours above are unchanged --
	 * an OSC 10/11 query still answers what the user configured -- but what
	 * is PAINTED swaps, so the paint path reads the effective pair and
	 * inverts every cell. Off by default; set by the emulator's signal.
	 */
	void set_reverse_video(bool reverse);
	bool reverse_video() const { return m_reverse_video; }
	QColor effective_background() const {
		return m_reverse_video ? m_foreground : m_background;
	}
	QColor effective_foreground() const {
		return m_reverse_video ? m_background : m_foreground;
	}

	/*
	 * The host's low sixteen, for the emulator to answer OSC 4 with. The
	 * scheme's own values rather than anything the renderer has adjusted:
	 * OSC 4 means the mapping from index to colour, and the brightness
	 * slider is a per-cell display adjustment that no single index has a
	 * fixed answer for.
	 */
	const QRgb *terminal_palette() const { return m_scheme.ansi; }

	/*
	 * Re-reads the current scheme from the catalog. set_colour_scheme()
	 * early-returns when the id has not changed, which is right for
	 * switching between fixed schemes and wrong for the custom one: its
	 * id stays "custom" while its sixteen colours change underneath.
	 */
	void reload_colour_scheme();

	/*
	 * Colour resolution, exposed for the tests that check a scheme
	 * repaints the sixteen it owns and leaves the 256-colour cube alone.
	 * The renderer's own path is private and stays that way.
	 */
	QColor colour_for_test(const bssh_color_t &colour) const { return resolve(colour, m_foreground); }

	/* block | underline | bar, and whether it blinks (sec 10.2). */
	void set_cursor_style(const QString &style);

	/*
	 * Re-measures the cell against the display it is on now (sec 9.5).
	 *
	 * The cell is snapped to whole DEVICE pixels, so it depends on the
	 * device pixel ratio -- which changes under a window dragged between
	 * monitors of different scaling. Nothing recomputed it there, so the
	 * grid kept the old monitor's metrics and the glyphs were scaled from
	 * a stale cell rather than laid out for the new one.
	 *
	 * Goes all the way through to window-change: recompute_grid() emits
	 * grid_resized, which the pane container has wired to the session.
	 */
	void remeasure_cell();

	/*
	 * The family this view is actually drawing in.
	 *
	 * Exists because `make shots` captioned its terminal pictures by
	 * re-deriving the font instead of asking, and got a different answer:
	 * the view resolves an empty id through default_id(), while the
	 * caption called family_for(QString()) and was handed the FALLBACK
	 * chain's first entry. The pictures said DejaVu Sans Mono and were
	 * drawn in JetBrains Mono.
	 *
	 * Sec 8.3 prints that family precisely because a shot of a renderer
	 * cannot be interpreted without knowing what rendered it -- so the
	 * safeguard against drawing in the wrong font was itself naming the
	 * wrong font. Asking the view is the only answer that cannot drift.
	 */
	QString font_family() const { return m_font.family(); }
	void set_cursor_blink(bool blink);
	QString cursor_style() const { return m_cursor_style; }

	/*
	 * Whether this pane's cursor is blinking RIGHT NOW, which is not the
	 * same question as whether the host asked for a blinking cursor: only
	 * the focused terminal blinks (sec 10.2). An unfocused pane is asleep
	 * whatever its settings say, so this reports the timer.
	 */
	bool cursor_blinks() const { return m_blink_timer != nullptr; }

	/* What the settings asked for, regardless of where focus is. */
	bool cursor_blink_wanted() const { return m_cursor_blink_wanted; }

	/* Whether the cursor is in the ON half of its blink right now. */
	bool cursor_shown() const { return m_cursor_on; }

	/* The tmux and screen drawer, while one is offered. */
	bssh_multiplexer_drawer *multiplexer_drawer() const { return m_mux_drawer; }

private:
	/*
	 * The font to draw one cell in, cached. Returned by reference because
	 * the paint loop asks per cell and a QFont copy per cell is the cost
	 * this cache exists to remove.
	 */
	const QFont &cell_font(const QString &family, bool bold, bool italic) const;

public:

	/*
	 * The grid to render. The view does not own it and does not write to
	 * it: the emulator owns the screen, the view draws whatever is in it
	 * when a repaint comes (project.md sec 3, core principle 2 -- state
	 * lives outside the widget, so this view can be destroyed and rebuilt
	 * at M7/M8 without losing anything).
	 *
	 * nullptr renders an empty grid, which is what the app shows before a
	 * session exists.
	 */
	void attach_screen(const bssh_screen *screen);

	/* Images drawn over (or under) the grid -- see sec 7.1, tier 4. */
	void attach_images(const bssh_image_store *images);

	/* Lines that scrolled off, for scrolling back through (sec 7). */
	void attach_scrollback(const bssh_scrollback *scrollback);
	const bssh_scrollback *scrollback() const { return m_scrollback; }

	/*
	 * How many lines above the live screen the view is showing. 0 is
	 * live; scrolling back is a VIEW state, not a session state -- the
	 * remote is not told, and output keeps arriving while the user reads
	 * history.
	 */
	int scroll_offset() const { return m_scroll_offset; }

	/*
	 * The two conversions between a point on this widget and a cell, in
	 * public because more than this class needs them -- sec 6.4's
	 * accessibility interface asks the same questions a mouse does, and
	 * when it could not ask, it grew its own copy of the arithmetic and
	 * inherited every bug the copy did not know about. One implementation
	 * or two answers.
	 *
	 * Both work in ABSOLUTE lines, so they span the scrollback, and both
	 * account for the grid origin, the inset and the scroll offset.
	 * cell_rect returns a widget-local rectangle, or a null one when the
	 * line is not currently on screen.
	 */
	bool cell_at(const QPoint &point, qint64 &line, int &col) const;
	QRect cell_rect(qint64 line, int col) const;
	void set_scroll_offset(int lines);
	void scroll_by(int lines);
	void scroll_to_live();

	/*
	 * What a drag or a click does here (sec 8.7, 8.7a). The window's one
	 * mode, the same for every pane, shown in the focused pane's corner.
	 * On touch it is the override Shift is on a desktop; on the desktop
	 * Click is the old behaviour, and Select and Scroll keep the pointer
	 * local inside a program that grabbed the mouse.
	 */
	void set_input_mode(bssh_input_mode_t mode);
	bssh_input_mode_t input_mode() const { return m_input_mode; }

	/* Move and Pan are offered: the window is a column (sec 8.7). */
	void set_column_available(bool available);

	/*
	 * The corner (sec 8.7a): grip, mode drum and multiplexer button over
	 * the grid's top-right corner. A pane has one; it shows while this is
	 * the focused pane -- "hidden while that window is inactive".
	 */
	bssh_pane_corner *corner() const { return m_corner; }
	void set_corner_active(bool active);

	/*
	 * The hidden bars' stand-ins in the corner (sec 8.7a): this pane's
	 * tab count while the tab bar is hidden (empty for none), and the
	 * menus while the menu bar is. Set by the window, which knows both.
	 */
	void set_corner_tabs(const QString &text);
	void set_corner_menu(bool shown);

	/*
	 * Draws the focused-pane border (sec 8.3). Owned by the view because
	 * it is painted with the grid, but decided by the pane container --
	 * a view has no idea whether it has siblings.
	 */
	void set_focus_border(bool focused);
	const bssh_screen *screen() const { return m_screen; }

	/*
	 * The router that turns this widget's key events into bytes (sec
	 * 6.3). The view does no translation of its own -- it forwards, so
	 * that the on-screen key row and a physical keyboard cannot drift
	 * apart.
	 */
	void attach_input_router(bssh_input_router *router) { m_input_router = router; }

	/*
	 * The emulator, for mouse reporting: a click goes to the remote when
	 * it has asked for the mouse, and selects text otherwise (sec 6.4).
	 * The view needs to ask which, per click.
	 */
	void attach_emulator(bssh_emulator *emulator);

	/*
	 * The current selection as text, with trailing blanks trimmed per
	 * line. Empty when nothing is selected.
	 */
	QString selection_text() const;
	bool has_selection() const;

	/*
	 * The selection in absolute line coordinates, or false when there is
	 * none. Public because the accessibility interface reports the same
	 * range in its own offsets, and two ways of normalising one selection
	 * is two ways for them to disagree.
	 */
	bool selection_range(qint64 &first_line, int &first_col,
	                      qint64 &last_line, int &last_col) const;
	void clear_selection();

	/*
	 * Drops a selection whose lines the scrollback has evicted (sec 6.4).
	 * Called wherever the history changes; see the definition for why it
	 * refuses to drop one that is only partly gone.
	 */
	void drop_evicted_selection();

	/*
	 * The same for the find matches, which name lines the same way
	 * (sec 6.4). Dropped on the start line; see the definition for why
	 * that differs from the selection's rule.
	 */
	void drop_evicted_find_matches();

	/*
	 * Everything the view can reach: the whole scrollback and the screen
	 * below it. One coordinate space covers both (see line_for_row), so
	 * this is line zero to the last line rather than anything special.
	 */
	void select_all();

	/*
	 * Find (project.md sec 6.4): search the scrollback and the screen for
	 * a case-insensitive substring. The view searches its OWN content --
	 * it holds the scrollback, the screen, and the one coordinate space
	 * that spans them (see line_for_row) -- so the find bar hands it a
	 * query and reads back matches rather than reaching into the buffers
	 * itself.
	 *
	 * find_text records every match and makes the one nearest the live
	 * screen current, scrolling it into view; find_step walks them
	 * (+1 toward the screen, -1 up the history), wrapping; clear_find
	 * forgets them. The is_*_match pair is what the paint path asks per
	 * cell, the same shape as is_selected.
	 */
	/*
	 * The bare URL under a cell, or empty where there is none.
	 *
	 * OSC 8 marks a run of cells as a link and the emulator holds the
	 * target, which is the reliable case and the one Ctrl+click already
	 * had. It is also the rare one: almost every URL a person sees in a
	 * terminal is plain text that some program printed without knowing
	 * anything about OSC 8 -- curl output, a git remote, a log line, the
	 * address in an error message. Those were not clickable at all.
	 *
	 * Searched across the LOGICAL line, for the reason find_text is: a
	 * URL long enough to be worth clicking is long enough to wrap, and
	 * the half on the next row is the half with the path in it.
	 *
	 * http and https only. The OSC 8 path takes whatever scheme the host
	 * sends, because a marked link is an explicit act by the program that
	 * drew it; text that merely LOOKS like a link is not, so what is
	 * inferred from it is deliberately narrower than what is declared.
	 */
	QString url_at(qint64 line, int col) const;

	/*
	 * The link under a widget position, marked or inferred, or empty.
	 *
	 * The click path had this logic inline and Ctrl-gated, which made the
	 * whole hyperlink feature desktop-only: a phone has no Ctrl, so
	 * neither an OSC 8 link nor a bare one could be opened there at all.
	 * Pulled out so the long-press menu can ask the same question and get
	 * the same answer -- sec 14's rule is that a milestone lands on both
	 * platforms, and an override only one input device can produce is not
	 * an override on the other.
	 */
	QString link_at(const QPoint &widget_pos) const;

	/*
	 * Select the word under a cell, the way a double-click does.
	 *
	 * Extracted because a long press wants the same answer (sec 6.4's
	 * touch model) and two copies of "what counts as a word" is two
	 * places for a path to stop being one thing.
	 */
	void select_word_at(qint64 line, int col);


	/*
	 * Where the two selection handles sit (sec 6.4), in widget
	 * coordinates, or a null rect where there are none.
	 *
	 * `start' is the handle on the beginning of the selection. They are
	 * BSSH_TOUCH_TARGET_DP across because they are dragged with a
	 * finger, which is wider than the cell they mark -- the CIRCLE is
	 * drawn smaller than the rect it answers with, so the thing a person
	 * can hit is bigger than the thing they can see. That asymmetry is
	 * the point rather than a mistake.
	 */
	QRect selection_handle_rect(bool start) const;

	int find_text(const QString &query);
	void find_step(int direction);
	void clear_find();
	int find_match_count() const { return m_find_matches.size(); }
	int current_find_index() const;
	QString find_query() const { return m_find_query; }
	bool is_find_match(qint64 line, int col) const;
	bool is_current_find_match(qint64 line, int col) const;

	/*
	 * Whether a press-and-hold opens the menu.
	 *
	 * On by default nowhere: it is set from the resolved menu layout, so
	 * a device using the touch layout gets the gesture that layout exists
	 * for, and a pointer keeps press-and-hold meaning "start a selection
	 * and think about it".
	 *
	 * Built on the mouse events a long press already arrives as, rather
	 * than on Qt's tap-and-hold gesture. That was tried first and never
	 * fired on Android: the recogniser wants real touch events, and what
	 * reaches this widget is the synthesised mouse press -- which is
	 * exactly why a long press on the host list registers as an ordinary
	 * tap today.
	 */
	void set_long_press_menu(bool enabled);

public slots:
	/*
	 * Repaint, and tell assistive technology the text changed (sec 14,
	 * M11). Both, from one slot, because a screen reader that is never
	 * told is a screen reader still reading the login banner while a
	 * build scrolls past.
	 */
	/*
	 * Everything that follows from the content changing, in one place.
	 *
	 * Damage is one cause and a scrollback trim is another, and only the
	 * first used to do all of it -- see the definition.
	 *
	 * Only the screen rows first_row..last_row are repainted; the
	 * defaults are the whole view.
	 */
	void note_content_changed(int first_row = 0, int last_row = INT_MAX);

	/*
	 * The same, for the selection. Emits selection_changed() AND tells
	 * an assistive client, which nothing did: the text interface
	 * declares selectionCount()/selection() and never announced that
	 * either had moved.
	 */
	void note_selection_changed();

	void on_screen_damaged();

	/* The emulator's rows_damaged(): repaints only those rows. */
	void on_rows_damaged(int first_row, int last_row);

	/* The emulator's rows_scrolled(): moves the pixels it has by delta rows. */
	void on_rows_scrolled(int first_row, int last_row, int delta);

	/*
	 * The tmux and screen drawer on this terminal's edge (sec 10.2a),
	 * shown while the session's offer stands and gone once it is
	 * answered. A view is rebuilt with every layout change, so the
	 * session keeps the offer and each new view is handed it again.
	 */
	void offer_multiplexers(const QList<bssh_mux_session_t> &found);
	void withdraw_multiplexers();

signals:
	/* The drawer's answer (sec 10.2a); see offer_multiplexers(). */
	void multiplexer_chosen(const bssh_mux_session_t &chosen);

	/*
	 * Ctrl+wheel (sec 9.5): zoom this pane by the given number of steps.
	 * A request rather than an action, for the same reason as the drag
	 * signals below -- the view does not know which session it shows, and
	 * the zoom belongs to the session so that it survives this widget,
	 * which is destroyed and rebuilt whenever the panes change.
	 */
	void zoom_requested(int steps);

	/*
	 * Alt+drag: this pane wants to be picked up and dropped somewhere
	 * else -- another window, the tab bar, or the desktop (sec 8.4).
	 * The view does not know which session it shows, so it says only
	 * that the gesture happened and lets the container name it.
	 */
	void pane_drag_requested();

	/* The corner's drum chose a mode; the window applies it everywhere. */
	void input_mode_chosen(bssh_input_mode_t mode);

	/* The corner's tabs or menu button: pop the list or the menus here. */
	void corner_tabs_requested(const QPoint &global);
	void corner_menu_requested(const QPoint &global);

	/*
	 * A body drag in Move mode (sec 8.7): reorder this pane within the
	 * column, by one place, in the given direction -- negative toward the
	 * top, positive toward the bottom. Emitted on RELEASE, not during the
	 * drag: acting on it reorders the column, which rebuilds the panes and
	 * destroys this view, so it must not fire while this view's own event
	 * handler is still on the stack. The container names the session and
	 * the window does the move, for the same reason as pane_drag_requested.
	 */
	void pane_move_requested(int direction);

	/*
	 * A body drag in Pan mode (sec 8.7): scroll the column's viewport by
	 * this many pixels since the last move. Unlike the reorder this
	 * rebuilds nothing -- the container only moves its scroll area -- so it
	 * fires live during the drag and the view keeps receiving the gesture.
	 * The container owns that scroll area, so it is what listens.
	 */
	void column_scroll_requested(int delta);

	/*
	 * That drag ended (sec 8.7): the container settles its scroll on a
	 * pane top, so a terminal starts at the top of the viewport rather
	 * than half off it. Emitted only when the drag actually moved --
	 * a tap has nothing to settle and would jump the column under a
	 * finger that never asked it to.
	 */
	void column_scroll_settled();

	/*
	 * A tap that selected nothing: somebody asking to type (sec 6.2).
	 *
	 * Emitted whichever keyboard is in use, and BEFORE the system-panel
	 * request below decides whether it is the one to raise -- because the
	 * built-in keyboard clears WA_InputMethodEnabled to decline the
	 * system panel, so a signal emitted only on that path would never
	 * fire in exactly the mode that needs it. The window decides what to
	 * show; the view only reports the gesture, which is the same division
	 * pane_drag_requested draws.
	 */
	void input_panel_requested();

	/*
	 * Emitted only when the grid size actually changes, not on every
	 * resize event -- at M5 this is what triggers window-change for one
	 * session, and a desktop window drag would otherwise fire a storm of
	 * them (sec 8.1).
	 */
	void grid_resized(int cols, int rows);

	/*
	 * This pane has the keyboard, however it got it -- a click, a tap, Tab,
	 * or somebody calling setFocus.
	 *
	 * The pane container needs telling because the two ideas of "focused"
	 * are separate: Qt's, which follows the click on its own because this
	 * widget takes StrongFocus, and the container's, which drives the focus
	 * border, the status line and what copy and paste act on (sec 8.3).
	 * They came apart on any click that was not also a keyboard chord.
	 */
	void focus_received();

	/*
	 * And focus leaving, which sec 7.1's focus reporting needs as much as
	 * arrival. Qt sends a focus-out when the WINDOW is deactivated too, so
	 * this one signal covers both switching panes and switching
	 * applications -- which is what mode 1004 is actually asking about.
	 */
	void focus_lost();

	/*
	 * Where a context menu was asked for, in global coordinates, and a
	 * middle-click paste. Raised rather than handled here because the
	 * actions belong to the window: paste needs the clipboard and the
	 * session's input router, exactly as the Ctrl+Shift+V shortcut does.
	 */
	void context_menu_requested(const QPoint &at);

	/*
	 * A tab from another window was dropped on this pane (sec 8.4). The
	 * view accepts the drag only so Qt delivers it -- Qt picks a target
	 * before delivery and never re-targets to an ancestor, so refusing
	 * here meant the window was never offered the drop at all.
	 */
	void session_dropped(const QMimeData *mime, int edge);
	void primary_paste_requested();

	/*
	 * Something was dropped on this pane and it came out as text
	 * (sec 6.4). The VIEW does not paste it: a paste has to go through
	 * the window, which is what asks about more than one line and what
	 * owns the router the bytes end up in.
	 */
	void paste_requested(const QString &text);

	/* A selection was made or cleared -- the window updates the clipboard. */
	void selection_changed();

	/*
	 * The find matches changed without anybody asking for a search (sec
	 * 6.4). The bar's count is PULLED -- update_find_status runs when the
	 * query changes or a step is taken -- so a drop that nothing
	 * announces leaves a true-looking number on screen until the next
	 * keystroke.
	 */
	void find_matches_changed();

	/*
	 * A hyperlink was activated (sec 7.1). The view does not open it: what
	 * to do with a URL a remote host chose is the window's decision, and
	 * a terminal widget opening browsers on its own is a component with
	 * more authority than it needs.
	 */
	void link_activated(const QString &uri);

public:
	/*
	 * Where the pane's drag handle sits, in widget coordinates.
	 *
	 * A pane has no chrome of its own -- panes are bare views in a
	 * splitter -- so the only place a handle can go is ON the terminal,
	 * and it costs the couple of cells it covers. That is the trade, and
	 * it buys the one gesture a pane had no discoverable route to: a
	 * window showing a single session shows it as a pane, which sec 8.3
	 * keeps out of the tab bar, so there was nothing to take hold of and
	 * the only way out was an Alt+drag nobody finds.
	 *
	 * Top RIGHT rather than top left, where a prompt and its command
	 * live. It is the top slot of the pane's corner now (sec 8.7a), so
	 * this is the corner's grip mapped into the view, and empty while the
	 * corner or its grip is hidden.
	 */
	QRect pane_grip_rect() const;

	/*
	 * Mark this pane as the one a drag is carrying.
	 *
	 * A drag with no visible source is a gesture you have to remember
	 * having started: the pointer leaves, nothing on screen says which
	 * terminal is about to move, and dropping somewhere wrong is
	 * indistinguishable from dropping somewhere right until it lands.
	 * So the pane it came from is hatched and outlined for as long as
	 * the drag is up.
	 */
	void set_being_dragged(bool dragged);
	bool being_dragged() const { return m_being_dragged; }

	/*
	 * Lights the zone a dragged pane would land in (a
	 * bssh_layout_manager::drop_edge_t), or clears it with -1. The drag
	 * events do this themselves; the desktop's own session drag, which
	 * is not a platform drag and sends none, calls it directly.
	 */
	void show_drop_zone(int edge);
	int drop_zone() const { return m_drop_edge; }

	/*
	 * Who decides what a session dropped here does. Without one the zone
	 * under the pointer is taken as it is, which is what a bare view in a
	 * test wants; a window sets one on every pane it shows.
	 */
	void set_drop_planner(bssh_drop_planner_t planner) { m_drop_planner = std::move(planner); }

	/*
	 * Whether the grip shows right now: in the focused pane's corner
	 * (sec 8.7a), on the desktop.
	 *
	 * TOUCH gets no grip, and a grip it could reach would be worse than
	 * none, which was measured rather than assumed. Dragging one hands
	 * the gesture to QDrag, Android gives the whole window to the
	 * platform's drag-and-drop, and this device's window manager claims
	 * it as drag-to-split-screen: the app is halved and an app picker
	 * appears, and dropEvent never runs. Confirmed by hand on a Fold, at
	 * two drag distances. Touch reorders panes with the Move mode and the
	 * context menu instead (sec 8.4), which nothing can intercept.
	 */
	bool grip_visible() const;

	/*
	 * Whether this view is a PANE, and so has a handle at all.
	 *
	 * Off by default and turned on by bssh_pane_container, because the
	 * handle belongs to a pane rather than to a terminal: a view built
	 * on its own -- a preview, a test -- is not something anybody can
	 * drag out of a window, and drawing a grip on it would be a control
	 * that does nothing.
	 *
	 * It is also what keeps the grip out of the renderer's pixel tests
	 * honestly. Those measure the ink in a frame and a grip is ink; the
	 * fix is that they build bare views, which are not panes, rather
	 * than teaching the assertions to ignore a corner.
	 */
	void set_pane_handle(bool handle);

protected:
	void leaveEvent(QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

	void keyPressEvent(QKeyEvent *event) override;

	void keyReleaseEvent(QKeyEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

	/*
	 * Pinch-to-zoom (sec 9.5). Qt delivers a recognised gesture as
	 * QEvent::Gesture through event() rather than to a handler of its
	 * own, so this is the one place it can be caught.
	 */
	bool event(QEvent *event) override;

private:
	/*
	 * One pinch update: emits as many zoom steps as the scale has
	 * crossed since the last one. Separate from event() so a test can
	 * reach it without Qt's recogniser, which needs real touch points.
	 */
	void handle_pinch(qreal change);

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void focusInEvent(QFocusEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;

	/*
	 * Asks for the on-screen keyboard (sec 6.2).
	 *
	 * Nothing raises it for a plain QWidget. Qt does it from QLineEdit,
	 * QTextEdit and the spin boxes, which call the private
	 * QWidgetPrivate::handleSoftwareInputPanel() themselves -- a custom
	 * widget gets nothing. So the terminal took focus, declared that it
	 * accepted input methods, and no keyboard ever appeared: the
	 * attribute says a panel WOULD be understood, not that one should be
	 * shown.
	 */
	void request_input_panel();

	/* sec 6.4's drops: a file's path, or the text itself. */
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:

	/*
	 * One place that builds m_font from the id, the size and the zoom,
	 * and then re-measures. Both ways in -- a resolve and a zoom -- go
	 * through it, because the three steps a font change takes are the
	 * thing that was already forgotten once (see update_metrics).
	 */
	void rebuild_font();

	void update_metrics();
	void recompute_grid();

	/*
	 * One pass for z < 0 (under the text) and one for the rest. Two passes
	 * rather than a sort: a handful of images, and a comparator would be
	 * more code to read than the second loop.
	 */
	void draw_images(QPainter &painter, int offset, bool below_text) const;

	QColor resolve(const bssh_color_t &color, const QColor &fallback) const;

	/* View row/col <-> absolute line, the one conversion selection needs. */
	/*
	 * Where the scrollback's line 0 sits in absolute numbering. Every
	 * conversion below goes through it, because "absolute" stopped being
	 * true the moment the ring evicted.
	 */
	qint64 history_base() const;

	/*
	 * Whether an absolute line is the TAIL of the one before it -- one
	 * logical line that ran past the right edge rather than two lines.
	 *
	 * Answered from whichever half holds it: the scrollback stores the
	 * flag for lines that have left the screen, and libvterm answers for
	 * the rows still on it.
	 */
	bool line_is_continuation(qint64 line) const;

	/* Which handle a drag is moving: -1 for none, 0 start, 1 end. */
	int m_dragging_handle;

	qint64 line_for_row(int row) const;

	/*
	 * An absolute line as a LIVE SCREEN row, which is what libvterm's
	 * mouse reports are in. Not line_for_row's inverse: that one carries
	 * the scroll offset and this one must not.
	 */
	int screen_row_for_line(qint64 line) const;

	/*
	 * Whether a Shift is in force for THIS gesture, counting the key
	 * row's sticky one, and consuming it if so.
	 *
	 * Shift is the conventional override that lets a user select text
	 * out of a program that grabbed the mouse (sec 6.4) -- and a phone
	 * has no Shift to hold, so on touch that override did not exist and
	 * a mouse-reporting program took the selection, the clipboard and
	 * the menu with it, with no way back. The key row's sticky modifier
	 * is the Shift a touch device has; this is what lets it reach a
	 * gesture rather than only a keystroke.
	 *
	 * One-shot, exactly as it is for a key: pressed, used once, cleared.
	 */
	bool take_shift_override(Qt::KeyboardModifiers modifiers);

	const bssh_cell_t &cell_for_line(qint64 line, int col) const;
	bool is_selected(qint64 line, int col) const;
	void normalised_selection(qint64 &first_line, int &first_col,
	                           qint64 &last_line, int &last_col) const;

	const bssh_screen *m_screen;
	const bssh_image_store *m_images;
	const bssh_scrollback *m_scrollback;
	/*
	 * The button the PRESS reported to the remote, or 0 when the press
	 * was ours. The release has to name the same button -- libvterm
	 * tracks a button mask, so a release naming a different one clears
	 * the wrong bit and leaves the real one set for ever, after which
	 * every further press of that button is swallowed as "no change".
	 */
	int m_reported_button;

	int m_scroll_offset;

	/*
	 * Always built, shown only where it is wanted. A widget created on
	 * demand would have to be wired up on demand too, and the wiring is
	 * what goes wrong.
	 */
	QScrollBar *m_scrollbar;
	scrollbar_position_t m_scrollbar_position;

	/*
	 * Where the grid starts and how wide it is, once the bar has taken
	 * its side. One place, because four things have to agree about it:
	 * the column count, the painter's origin, the point-to-cell mapping
	 * and size_for_grid's answer.
	 */
	int grid_origin_x() const;
	int grid_width() const;

	/*
	 * How far back the view may scroll, which is NOT simply how many
	 * lines the scrollback holds: on the alternate screen it is nothing.
	 * One place, because the offset, the scrollbar's range and the
	 * re-clamp on damage all have to agree about it.
	 */
	int available_history() const;

	/*
	 * Held so the view can drop its OWN connection to an emulator it is
	 * letting go of. A blanket disconnect(emulator, nullptr, this,
	 * nullptr) would take the pane container's damage wiring with it,
	 * which is somebody else's connection between the same two objects.
	 */
	QMetaObject::Connection m_alternate_screen_connection;

	/*
	 * Held for the same reason: the view's own connection, dropped when
	 * it lets an emulator go, without disturbing anyone else's.
	 */
	QMetaObject::Connection m_scrollback_connection;

	/* Range, value and page step from the scrollback and the offset. */
	void sync_scrollbar();
	void place_scrollbar();
	bool m_focus_border;
	bool m_being_dragged;

	/*
	 * The zone a pane being dragged over this one would land in, or -1
	 * when none is -- drawn so the user sees where it will go before
	 * letting go.
	 */
	int m_drop_edge;
	bssh_drop_planner_t m_drop_planner;
	bool m_pointer_inside;
	bool m_pane_handle;

	/*
	 * Selection endpoints in ABSOLUTE line coordinates -- history line
	 * index, continuing into the live screen -- not view rows. Scrolling
	 * while a selection exists must not move the selection, and view rows
	 * change meaning the moment the offset does.
	 */
	/*
	 * Triple-click is a double-click followed by a third press inside the
	 * platform's double-click interval: Qt reports the third press as an
	 * ordinary press, so it has to be recognised here (sec 6.4).
	 */
	QElapsedTimer m_multi_click_timer;
	int m_click_count;
	qint64 m_last_click_line;

	bool m_selecting;
	bssh_long_press *m_long_press;
	qint64 m_selection_anchor_line;
	int m_selection_anchor_col;
	qint64 m_selection_focus_line;
	int m_selection_focus_col;

	/*
	 * A find match as a range in absolute line coordinates -- start and end
	 * cell -- so it can span the wrap of a logical line the same way a
	 * selection does, and the highlight covers a wide glyph's whole width.
	 * The absolute coordinate is the one the selection uses, so a match
	 * survives the ring evicting lines beneath it (see history_base).
	 * m_current_match indexes into the list, or -1 when nothing is found.
	 */
	struct find_match_t {
		qint64 start_line;
		int start_col;
		qint64 end_line;
		int end_col;
	};
	void scroll_find_match_into_view(const find_match_t &match);
	QList<find_match_t> m_find_matches;
	int m_current_match;
	QString m_find_query;

	bssh_emulator *m_emulator;
	/*
	 * The caret offset last ANNOUNCED to an assistive client, so a cursor
	 * event goes out when it moves rather than on every damage. -1 until
	 * the first one, which no real offset can be.
	 */
	int m_last_cursor_offset = -1;

	bssh_input_router *m_input_router;
	QFont m_font;

	/*
	 * The catalog id behind m_font, and the size it was built at. Kept as
	 * the id rather than derived back from the QFont, because m_font may
	 * be carrying a substitute family and the question "what did the user
	 * ask for" still has to have an answer.
	 */
	QString m_font_id;
	int m_font_size;

	/*
	 * Points added to m_font_size by the user's zoom, and never folded
	 * into it: m_font_size stays the answer the host resolved to, so the
	 * two can be told apart when the next resolve arrives.
	 */
	int m_font_zoom;

	/*
	 * Scale accumulated since the last zoom step a pinch produced, so a
	 * pinch that keeps growing keeps stepping. See handle_pinch for why
	 * it accumulates rather than reading the gesture's total.
	 */
	qreal m_pinch_scale;

	/*
	 * Per-repaint font cache, keyed by family and the two attributes that
	 * change one. Building a QFont per cell showed up as the obvious cost
	 * once the fallback chain meant the family could change mid-row: a
	 * screen of 80x24 constructs it 1920 times otherwise, and the answer
	 * is the same handful of fonts every time. sec 9.5's shared glyph
	 * atlas is a bigger structure than this and still open.
	 */
	mutable QHash<QString, QFont> m_font_cache;

	QString m_scheme_id;
	bssh_colour_scheme_t m_scheme;

	/* The host left the scheme unset, so it follows the app (sec 6.2). */
	bool m_scheme_follows_default = true;

	QString m_cursor_style;
	bool m_cursor_on;
	bool m_cursor_blink_wanted;

	/* Alt+drag picks the pane up (sec 8.4); see mousePressEvent. */
	bool m_maybe_pane_drag;
	QPoint m_pane_drag_from;

	/*
	 * A touch body-drag in scroll mode (sec 8.7). The offset is captured
	 * at the press so the drag is absolute -- a delta re-read each move
	 * accumulates rounding into a drift the finger never asked for -- and
	 * `moved` tells a scroll from a tap, so a tap still asks for the
	 * keyboard rather than being eaten as a zero-length scroll.
	 */
	bssh_input_mode_t m_input_mode;
	bool m_scroll_dragging;
	bool m_scroll_drag_moved;
	QPoint m_scroll_drag_from;
	int m_scroll_drag_start_offset;

	/*
	 * A touch body-drag in Move mode (sec 8.7), which reorders the column.
	 * `moved` tells a drag from a tap so a tap here is inert rather than
	 * an accidental reorder, and the start point gives the net direction
	 * the finger travelled, decided once on release.
	 */
	bool m_move_dragging;
	bool m_move_drag_moved;
	QPoint m_move_drag_from;

	/*
	 * A touch body-drag in Pan mode (sec 8.7), which scrolls the column's
	 * viewport. Tracked incrementally -- each move emits the delta since
	 * the last one -- because the thing it scrolls is the container's, not
	 * this view's, so there is no absolute offset here to measure from.
	 * `moved` tells a drag from a tap, as the others do.
	 */
	bool m_colscroll_dragging;
	bool m_colscroll_moved;
	QPoint m_colscroll_last;
	QTimer *m_blink_timer;

	/*
	 * Starts or stops the blink for what is true now -- the setting AND
	 * where focus is. Both inputs change independently, so one place
	 * decides rather than two setters each doing half of it.
	 */
	void sync_blink_timer();

	/*
	 * Typing or output shows the cursor and starts its blink over, so it
	 * is never caught in the off half of a blink just as something
	 * happens where it is.
	 */
	void hold_cursor_on();

	/* Where the cursor cell is drawn, in widget coordinates. */
	QRect cursor_cell_rect() const;

	/* The drawer on the right edge, inside the scrollbar: a handle closed,
	 * the height of the terminal open. */
	void place_multiplexer_drawer();
	void sync_multiplexer_drawer_shown();
	bssh_multiplexer_drawer *m_mux_drawer = nullptr;

	/* The corner over the grid's top-right (sec 8.7a). */
	void place_corner();
	void sync_corner();
	bssh_pane_corner *m_corner = nullptr;
	bool m_corner_active = false;
	QString m_corner_tabs;
	bool m_corner_menu = false;
	bool m_column_available = false;

	/*
	 * Repaints screen rows first_row..last_row, or the whole view when
	 * the rows are not where the screen has them -- scrolled back into
	 * history -- or are most of it anyway.
	 */
	void update_rows(int first_row, int last_row);

	QColor m_background;
	QColor m_foreground;
	bool m_reverse_video;
	quint64 m_content_revision;
	bool m_raise_contrast;
	int m_text_brightness;
	bool m_focus_follows_mouse;
	int m_cell_width;
	int m_cell_height;
	int m_baseline;
	int m_cols;
	int m_rows;
};

/*
 * Apply a host's resolved settings (sec 10.1) to a view.
 *
 * A free function rather than a method, because it is the one place that
 * knows the resolver's field NAMES, and a widget that knows those is a
 * widget coupled to the preset schema. Everything it touches is
 * appearance: what the grid is drawn in, never what the session does.
 *
 * Unset fields are left at the view's defaults rather than being forced to
 * anything -- a preset with no font chosen means "no opinion", which is
 * not the same as an opinion that happens to match the default.
 */
void bssh_apply_appearance(bssh_terminal_view *view, const QJsonObject &settings);

/*
 * Round a cell dimension so the grid lands on whole DEVICE pixels, not
 * merely whole logical ones (sec 9.5).
 *
 * A free function because it is arithmetic with no widget in it, and
 * because the case it exists for -- fractional display scaling -- is one a
 * test cannot reach by constructing a view: the test binary runs at 100%.
 */
int bssh_snap_to_device_pixels(int pixels, qreal device_ratio);

#endif /* BSSH_TERMINAL_VIEW_H */
