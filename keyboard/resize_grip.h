#ifndef SK_RESIZE_GRIP_H
#define SK_RESIZE_GRIP_H

#include <QWidget>

/*
 * A thin bar on the built-in keyboard's edge that resizes it by dragging
 * (project.md sec 6.2): the top edge of a keyboard under the terminal, the
 * inner edge of each split half.
 *
 * The edge that faces the terminal, never the screen's own: Android's back
 * gesture lives on the screen edges, and a bar there would fight it.
 *
 * It reports the drag and nothing else -- how far the finger has moved
 * along its axis since it came down -- and the keyboard decides what that
 * means, since only it knows which side of the bar it is on.
 */
class sk_resize_grip : public QWidget {
	Q_OBJECT

public:
	/*
	 * `drags` is the direction the bar moves: Qt::Horizontal for a bar
	 * standing upright at a split half's edge, Qt::Vertical for one lying
	 * along the top of the keyboard.
	 */
	explicit sk_resize_grip(Qt::Orientation drags, QWidget *parent = nullptr);

	/*
	 * Thick enough for a finger to find, thin enough to be no more than an
	 * edge: the line drawn is a third of it.
	 */
	static constexpr int THICKNESS_DP = 12;

	QSize sizeHint() const override;

	/*
	 * Drawn as held without being held: the split keyboard lights both
	 * halves' bars while either is dragged, because one drag resizes
	 * both (sec 6.2) and a bar that stays dim while its half moves reads
	 * as a bar that does nothing.
	 */
	void set_lit(bool lit);
	bool lit() const { return m_lit; }

signals:
	/* The finger came down; a drag follows. */
	void pressed();

	/* How far along the axis, in dp, since pressed(). */
	void dragged(int distance);

	/* Let go: the size it was dragged to is the one to keep. */
	void released();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	int along(const QPointF &global) const;

	Qt::Orientation m_drags;
	int m_from = 0;
	bool m_down = false;
	bool m_lit = false;
};

#endif /* SK_RESIZE_GRIP_H */
