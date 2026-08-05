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

class QMouseEvent;
class QPainter;
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
	bool raise_contrast() const { return m_raise_contrast; }

	int cell_width() const { return m_cell_width; }
	int cell_height() const { return m_cell_height; }

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
	 * Colour resolution, exposed for the tests that check a scheme
	 * repaints the sixteen it owns and leaves the 256-colour cube alone.
	 * The renderer's own path is private and stays that way.
	 */
	QColor colour_for_test(const bssh_color_t &colour) const { return resolve(colour, m_foreground); }

	/* block | underline | bar, and whether it blinks (sec 10.2). */
	void set_cursor_style(const QString &style);
	void set_cursor_blink(bool blink);
	QString cursor_style() const { return m_cursor_style; }
	bool cursor_blinks() const { return m_blink_timer != nullptr; }

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
	void attach_emulator(bssh_emulator *emulator) { m_emulator = emulator; }

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
	void primary_paste_requested();

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
	void focusOutEvent(QFocusEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;

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
	qint64 line_for_row(int row) const;
	bool cell_at(const QPoint &point, qint64 &line, int &col) const;
	const bssh_cell_t &cell_for_line(qint64 line, int col) const;
	bool is_selected(qint64 line, int col) const;
	void normalised_selection(qint64 &first_line, int &first_col,
	                           qint64 &last_line, int &last_col) const;

	const bssh_screen *m_screen;
	const bssh_image_store *m_images;
	const bssh_scrollback *m_scrollback;
	int m_scroll_offset;
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
	QTimer *m_blink_timer;

	QColor m_background;
	QColor m_foreground;
	quint64 m_content_revision;
	bool m_raise_contrast;
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
