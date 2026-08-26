#ifndef BSSH_TERMINAL_ACCESSIBLE_H
#define BSSH_TERMINAL_ACCESSIBLE_H

#include <QAccessibleWidget>
#include <QList>
#include <QString>

class bssh_terminal_view;

/*
 * bssh_terminal_accessible -- the terminal, for assistive technology
 * (project.md sec 14, M11).
 *
 * The terminal view is a widget that paints characters, so to a screen
 * reader it was an anonymous blank rectangle: role Client, no name, no
 * value, no text interface. Every other control in the app can be fixed
 * by naming it; this one is different, because the thing that needs
 * reading is not the control's label but its CONTENTS -- which are the
 * whole application. A terminal a screen reader cannot read is not a
 * partly accessible terminal.
 *
 * Read-only on purpose. The cursor belongs to the remote program and
 * moves when it says so, so setCursorPosition and the selection setters
 * do nothing rather than lie about having moved something. What this
 * exposes is what is on screen, where the cursor is, and where each
 * character sits -- which is what a reader needs to navigate.
 */
class bssh_terminal_accessible : public QAccessibleWidget,
                                  public QAccessibleTextInterface {
public:
	explicit bssh_terminal_accessible(bssh_terminal_view *view);

	/* The factory registered with QAccessible; see install(). */
	static QAccessibleInterface *factory(const QString &class_name, QObject *object);

	/*
	 * Registers the factory once. Called by the view's constructor rather
	 * than from main(), so a terminal is accessible wherever one is
	 * built -- including in a test, which is the only place this gets
	 * exercised without a screen reader attached.
	 */
	static void install();

	void *interface_cast(QAccessible::InterfaceType type) override;
	QAccessible::Role role() const override;
	QAccessible::State state() const override;
	QString text(QAccessible::Text kind) const override;

	/* QAccessibleTextInterface. */
	QString text(int start_offset, int end_offset) const override;
	int characterCount() const override;
	QRect characterRect(int offset) const override;
	int offsetAtPoint(const QPoint &point) const override;

	int cursorPosition() const override;
	void setCursorPosition(int position) override;

	void selection(int index, int *start_offset, int *end_offset) const override;
	int selectionCount() const override;
	void addSelection(int start_offset, int end_offset) override;
	void removeSelection(int index) override;
	void setSelection(int index, int start_offset, int end_offset) override;

	void scrollToSubstring(int start_index, int end_index) override;
	QString attributes(int offset, int *start_offset, int *end_offset) const override;

private:
	/*
	 * The screen as one string, plus where each row starts in it.
	 *
	 * Rows are joined with newlines and their trailing blanks dropped,
	 * because Qt's boundary navigation looks for '\n' and a reader that
	 * announces eighty spaces before every line break is a reader nobody
	 * will use. The row table is what keeps an offset convertible back to
	 * a cell afterwards, which trimming would otherwise make impossible.
	 */
	struct flat_t {
		QString text;
		QList<int> row_start;
		QList<int> row_length;

		/*
		 * The CELL column of each character of a row -- but only for the
		 * rows that need it, and an empty list means "column equals
		 * index". flatten() drops the far half of a wide glyph, because
		 * it is not a character, so from the first wide glyph on a row
		 * the character index and the cell column run apart by one per
		 * glyph. Every row of ordinary text therefore costs nothing.
		 */
		QList<QList<int> > row_cells;

		/* How many of the rows above came from the scrollback, so an
		 * offset can be turned back into an absolute line. */
		int history_rows = 0;

		/* The revision this was built from; see bssh_terminal_view. */
		quint64 revision = 0;
		bool valid = false;
	};

	/*
	 * Cached against the view's content revision. A full scrollback is
	 * hundreds of kilobytes and a screen reader asks these questions
	 * constantly -- rebuilding the string per query would put the cost of
	 * the entire history on every keystroke.
	 */
	const flat_t &flatten() const;

	mutable flat_t m_flat;
	bssh_terminal_view *view() const;
};

#endif /* BSSH_TERMINAL_ACCESSIBLE_H */
