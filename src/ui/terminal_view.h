#ifndef BSSH_TERMINAL_VIEW_H
#define BSSH_TERMINAL_VIEW_H

#include <QColor>
#include <QFont>
#include <QWidget>

#include "term/screen.h"

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
	const bssh_screen *screen() const { return m_screen; }

	/*
	 * The router that turns this widget's key events into bytes (sec
	 * 6.3). The view does no translation of its own -- it forwards, so
	 * that the on-screen key row and a physical keyboard cannot drift
	 * apart.
	 */
	void attach_input_router(bssh_input_router *router) { m_input_router = router; }

signals:
	/*
	 * Emitted only when the grid size actually changes, not on every
	 * resize event -- at M5 this is what triggers window-change for one
	 * session, and a desktop window drag would otherwise fire a storm of
	 * them (sec 8.1).
	 */
	void grid_resized(int cols, int rows);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void inputMethodEvent(QInputMethodEvent *event) override;

private:
	void update_metrics();
	void recompute_grid();

	QColor resolve(const bssh_color_t &color, const QColor &fallback) const;

	const bssh_screen *m_screen;
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
