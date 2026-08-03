#include "ui/terminal_accessible.h"

#include "term/screen.h"
#include "ui/terminal_view.h"

/* Matches the inset the painter uses; see terminal_view.cpp. */
static const int BSSH_GRID_INSET_PX = 1;

bssh_terminal_accessible::bssh_terminal_accessible(bssh_terminal_view *view)
    : QAccessibleWidget(view, QAccessible::Terminal) {
}

bssh_terminal_view *bssh_terminal_accessible::view() const {
	return qobject_cast<bssh_terminal_view *>(object());
}

QAccessibleInterface *bssh_terminal_accessible::factory(const QString &class_name,
                                                         QObject *object) {
	if (class_name == QLatin1String("bssh_terminal_view") && object && object->isWidgetType()) {
		return new bssh_terminal_accessible(static_cast<bssh_terminal_view *>(object));
	}
	return nullptr;
}

void bssh_terminal_accessible::install() {
	static bool installed = false;
	if (installed) return;

	installed = true;
	QAccessible::installFactory(&bssh_terminal_accessible::factory);
}

void *bssh_terminal_accessible::interface_cast(QAccessible::InterfaceType type) {
	if (type == QAccessible::TextInterface) return static_cast<QAccessibleTextInterface *>(this);
	return QAccessibleWidget::interface_cast(type);
}

QAccessible::Role bssh_terminal_accessible::role() const {
	return QAccessible::Terminal;
}

QAccessible::State bssh_terminal_accessible::state() const {
	QAccessible::State s = QAccessibleWidget::state();

	/*
	 * Read-only, and it says so. A reader that believes it can type into
	 * this offers an editing gesture that silently does nothing -- keys
	 * reach the remote through the input router, not through here.
	 */
	s.readOnly = true;
	s.multiLine = true;
	return s;
}

QString bssh_terminal_accessible::text(QAccessible::Text kind) const {
	switch (kind) {
	case QAccessible::Name: {
		const QString named = QAccessibleWidget::text(kind);
		return named.isEmpty() ? QStringLiteral("Terminal") : named;
	}
	case QAccessible::Value:
		return flatten().text;
	default:
		return QAccessibleWidget::text(kind);
	}
}

bssh_terminal_accessible::flat_t bssh_terminal_accessible::flatten() const {
	flat_t out;

	const bssh_terminal_view *terminal = view();
	const bssh_screen *screen = terminal ? terminal->screen() : nullptr;
	if (!screen) return out;

	for (int row = 0; row < screen->rows(); ++row) {
		QString line;
		line.reserve(screen->cols());

		for (int col = 0; col < screen->cols(); ++col) {
			const bssh_cell_t &cell = screen->cell(row, col);
			/* The far half of a wide glyph is not a character (sec 9.3). */
			if (cell.width == 0) continue;
			line.append(QString::fromUcs4(&cell.ch, 1));
		}
		while (line.endsWith(QLatin1Char(' '))) line.chop(1);

		if (row > 0) out.text.append(QLatin1Char('\n'));
		out.row_start.append(out.text.size());
		out.row_length.append(line.size());
		out.text.append(line);
	}

	return out;
}

QString bssh_terminal_accessible::text(int start_offset, int end_offset) const {
	const QString all = flatten().text;

	const int start = qBound(0, start_offset, all.size());
	const int end = qBound(start, end_offset, all.size());
	return all.mid(start, end - start);
}

int bssh_terminal_accessible::characterCount() const {
	return flatten().text.size();
}

int bssh_terminal_accessible::cursorPosition() const {
	const bssh_terminal_view *terminal = view();
	const bssh_screen *screen = terminal ? terminal->screen() : nullptr;
	if (!screen) return 0;

	const flat_t flat = flatten();
	const int row = qBound(0, screen->cursor_row(), flat.row_start.size() - 1);
	if (flat.row_start.isEmpty()) return 0;

	/*
	 * Clamped to the row's own length, because trailing blanks are not in
	 * the text: a cursor sitting past the last character of its line
	 * belongs at the end of that line, not inside the next one.
	 */
	const int col = qBound(0, screen->cursor_col(), flat.row_length.at(row));
	return flat.row_start.at(row) + col;
}

void bssh_terminal_accessible::setCursorPosition(int position) {
	/*
	 * Deliberately nothing. The cursor is the remote program's, and moving
	 * it locally would put the reader's idea of the position somewhere the
	 * terminal is not.
	 */
	Q_UNUSED(position);
}

QRect bssh_terminal_accessible::characterRect(int offset) const {
	const bssh_terminal_view *terminal = view();
	if (!terminal) return QRect();

	const flat_t flat = flatten();
	for (int row = flat.row_start.size() - 1; row >= 0; --row) {
		if (offset < flat.row_start.at(row)) continue;

		const int col = offset - flat.row_start.at(row);
		const QPoint origin = terminal->mapToGlobal(QPoint(0, 0));
		return QRect(origin.x() + BSSH_GRID_INSET_PX + col * terminal->cell_width(),
		              origin.y() + BSSH_GRID_INSET_PX + row * terminal->cell_height(),
		              terminal->cell_width(), terminal->cell_height());
	}
	return QRect();
}

int bssh_terminal_accessible::offsetAtPoint(const QPoint &point) const {
	const bssh_terminal_view *terminal = view();
	if (!terminal || terminal->cell_width() <= 0 || terminal->cell_height() <= 0) return -1;

	const QPoint local = terminal->mapFromGlobal(point);
	const flat_t flat = flatten();
	if (flat.row_start.isEmpty()) return -1;

	const int row = qBound(0, (local.y() - BSSH_GRID_INSET_PX) / terminal->cell_height(),
	                        flat.row_start.size() - 1);
	const int col = qBound(0, (local.x() - BSSH_GRID_INSET_PX) / terminal->cell_width(),
	                        flat.row_length.at(row));
	return flat.row_start.at(row) + col;
}

/*
 * No selection reported. The view has one, but it lives in scrollback
 * coordinates that span history this interface does not expose yet, and a
 * selection reported against the wrong offsets is worse than none.
 */
int bssh_terminal_accessible::selectionCount() const {
	return 0;
}

void bssh_terminal_accessible::selection(int index, int *start_offset, int *end_offset) const {
	Q_UNUSED(index);
	if (start_offset) *start_offset = 0;
	if (end_offset) *end_offset = 0;
}

void bssh_terminal_accessible::addSelection(int start_offset, int end_offset) {
	Q_UNUSED(start_offset);
	Q_UNUSED(end_offset);
}

void bssh_terminal_accessible::removeSelection(int index) {
	Q_UNUSED(index);
}

void bssh_terminal_accessible::setSelection(int index, int start_offset, int end_offset) {
	Q_UNUSED(index);
	Q_UNUSED(start_offset);
	Q_UNUSED(end_offset);
}

void bssh_terminal_accessible::scrollToSubstring(int start_index, int end_index) {
	Q_UNUSED(start_index);
	Q_UNUSED(end_index);
}

QString bssh_terminal_accessible::attributes(int offset, int *start_offset,
                                              int *end_offset) const {
	/*
	 * No text attributes reported. Colour is what this terminal has most
	 * of, and it is decoration rather than structure -- announcing it per
	 * run would bury the text a reader came for.
	 */
	Q_UNUSED(offset);
	if (start_offset) *start_offset = 0;
	if (end_offset) *end_offset = characterCount();
	return QString();
}
