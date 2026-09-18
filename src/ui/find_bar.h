#ifndef BSSH_FIND_BAR_H
#define BSSH_FIND_BAR_H

#include <QWidget>

class QLabel;
class QLineEdit;

/*
 * bssh_find_bar -- the find-in-scrollback strip (project.md sec 6.4).
 *
 * A thin bar with a text field, a match count and previous/next, shown on
 * the find chord and dismissed with Escape. It knows nothing about the
 * scrollback: it emits what the user typed and which way they want to step,
 * and the window drives the focused pane's search. That keeps the search
 * itself in the view, which owns the buffers and the coordinate space, and
 * leaves this a plain control -- the same split the input selector uses.
 */
class bssh_find_bar : public QWidget {
	Q_OBJECT

public:
	explicit bssh_find_bar(QWidget *parent = nullptr);

	/* Take the keyboard and select the field, for reopening on a query. */
	void take_focus();
	QString query() const;

	/* Show the position in the matches, or that there are none / none typed. */
	void set_status(int current, int total);

signals:
	void search_changed(const QString &query);
	void step_requested(int direction); /* +1 next, -1 previous */
	void dismissed();

protected:
	/* Escape closes; a plain line edit ignores it, so it reaches here. */
	void keyPressEvent(QKeyEvent *event) override;

private:
	QLineEdit *m_field;
	QLabel *m_status;
};

#endif /* BSSH_FIND_BAR_H */
