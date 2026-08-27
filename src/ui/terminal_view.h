#ifndef BSSH_TERMINAL_VIEW_H
#define BSSH_TERMINAL_VIEW_H

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QJsonObject>
#include <QWidget>

#include "term/colour_scheme.h"
#include "term/image_store.h"
#include "term/screen.h"
#include "term/scrollback.h"

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
	 * Everything the view can reach: the whole scrollback and the screen
	 * below it. One coordinate space covers both (see line_for_row), so
	 * this is line zero to the last line rather than anything special.
	 */
	void select_all();

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
	void on_screen_damaged();

signals:
	/*
	 * Alt+drag: this pane wants to be picked up and dropped somewhere
	 * else -- another window, the tab bar, or the desktop (sec 8.4).
	 * The view does not know which session it shows, so it says only
	 * that the gesture happened and lets the container name it.
	 */
	void pane_drag_requested();

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
	void session_dropped(const QMimeData *mime);
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
	 * A hyperlink was activated (sec 7.1). The view does not open it: what
	 * to do with a URL a remote host chose is the window's decision, and
	 * a terminal widget opening browsers on its own is a component with
	 * more authority than it needs.
	 */
	void link_activated(const QString &uri);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

	void keyPressEvent(QKeyEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void focusInEvent(QFocusEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void focusOutEvent(QFocusEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;

	/* sec 6.4's drops: a file's path, or the text itself. */
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
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
	bssh_emulator *m_emulator;
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

	QString m_cursor_style;
	bool m_cursor_on;
	bool m_cursor_blink_wanted;

	/* Alt+drag picks the pane up (sec 8.4); see mousePressEvent. */
	bool m_maybe_pane_drag;
	QPoint m_pane_drag_from;
	QTimer *m_blink_timer;

	/*
	 * Starts or stops the blink for what is true now -- the setting AND
	 * where focus is. Both inputs change independently, so one place
	 * decides rather than two setters each doing half of it.
	 */
	void sync_blink_timer();

	QColor m_background;
	QColor m_foreground;
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
