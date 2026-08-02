#ifndef BSSH_TERMINAL_VIEW_H
#define BSSH_TERMINAL_VIEW_H

#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QWidget>

#include "term/image_store.h"
#include "term/screen.h"
#include "term/scrollback.h"

class QMouseEvent;
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
	int cell_width() const { return m_cell_width; }
	int cell_height() const { return m_cell_height; }

	void set_terminal_font(const QFont &font);

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
	void clear_selection();

signals:
	/*
	 * Emitted only when the grid size actually changes, not on every
	 * resize event -- at M5 this is what triggers window-change for one
	 * session, and a desktop window drag would otherwise fire a storm of
	 * them (sec 8.1).
	 */
	void grid_resized(int cols, int rows);

	/* A selection was made or cleared -- the window updates the clipboard. */
	void selection_changed();

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;

private:
	void update_metrics();
	void recompute_grid();

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
	qint64 m_selection_anchor_line;
	int m_selection_anchor_col;
	qint64 m_selection_focus_line;
	int m_selection_focus_col;
	bssh_emulator *m_emulator;
	bssh_input_router *m_input_router;
	QFont m_font;
	QColor m_background;
	QColor m_foreground;
	int m_cell_width;
	int m_cell_height;
	int m_baseline;
	int m_cols;
	int m_rows;
};

#endif /* BSSH_TERMINAL_VIEW_H */
