#ifndef BSSH_PANE_CORNER_H
#define BSSH_PANE_CORNER_H

#include <QWidget>

class QToolButton;
class bssh_input_selector;

/*
 * bssh_pane_corner -- everything that covers a pane's text, in one place
 * (project.md sec 8.7a): the drag grip, the input mode drum, and the tmux
 * and screen button, stacked top to bottom over the grid's top-right
 * corner. The holder: "we keep all of the annoying things that can cover
 * the terminal in one place, and hidden while that window is inactive."
 *
 * An overlay, never a column: it takes no space from the grid, so showing
 * it never resizes the terminal. The view places it (place_corner) and
 * shows it only on the focused pane.
 */
class bssh_pane_corner : public QWidget {
	Q_OBJECT

public:
	explicit bssh_pane_corner(QWidget *parent = nullptr);

	bssh_input_selector *selector() const { return m_selector; }

	/* The drag grip: the desktop's, and only on a pane (sec 8.4). */
	void set_grip_shown(bool shown);
	bool grip_shown() const;

	/* The drum's rows, three or one (bssh_input_selector::set_rows). */
	void set_drum_rows(int rows);

	/* Its side, in proportion to the text it sits over. */
	void set_grip_side(int side);

	/* The grip's rect in this widget, empty while it is hidden. */
	QRect grip_rect() const;

	/* What the multiplexer button reads; empty hides it (sec 10.2a). */
	void set_multiplexer_text(const QString &text);
	QToolButton *multiplexer_button() const { return m_multiplexer; }

	/*
	 * The bars that are hidden, reached from here instead (sec 8.7a): the
	 * pane's tabs while the tab bar is hidden ("tabs 3"; empty hides the
	 * button), and the menu bar's menus while it is.
	 */
	void set_tabs_text(const QString &text);
	QToolButton *tabs_button() const { return m_tabs; }
	void set_menu_shown(bool shown);
	QToolButton *menu_button() const { return m_menu; }

	/*
	 * Faint until the pointer is over it -- the desktop's, where the
	 * holder asked for it "transparent until the mouse is over it". Touch
	 * has nothing that hovers, so there it is always solid.
	 */
	void set_faint_when_idle(bool faint);
	bool is_faint() const;

	QSize sizeHint() const override;

signals:
	/* The grip was dragged far enough to mean it (sec 8.4). */
	void drag_requested();

	void multiplexer_clicked();

	/* Where to put the popup: under the button, in global coordinates. */
	void tabs_clicked(const QPoint &global);
	void menu_clicked(const QPoint &global);

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	void relayout();
	void sync_opacity();

	QWidget *m_grip;
	bssh_input_selector *m_selector;
	QToolButton *m_multiplexer;
	QToolButton *m_tabs;
	QToolButton *m_menu;
	int m_grip_side = 18;
	bool m_faint_when_idle = false;
	bool m_hovered = false;
	class QGraphicsOpacityEffect *m_opacity;
};

#endif /* BSSH_PANE_CORNER_H */
