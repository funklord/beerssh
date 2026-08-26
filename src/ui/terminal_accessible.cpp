#include "ui/terminal_accessible.h"

#include "term/screen.h"
#include "term/scrollback.h"
#include "ui/terminal_view.h"

/*
 * A row's CHARACTER index as a cell COLUMN, and back. An empty map means
 * the row has no wide glyph and the two are the same number; see flat_t.
 *
 * These exist because a reader asks in characters and the grid answers in
 * cells, and every place that confused the two got a line of CJK wrong by
 * one cell per preceding glyph -- silently, on the one surface where the
 * person who wrote it cannot see the answer.
 */
static int cell_for_index(const QList<int> &map, int index) {
	if (map.isEmpty()) return index;
	if (index < map.size()) return map.at(index);

	/* Past the last character: keep counting columns from where it ended. */
	return map.last() + 1 + (index - map.size());
}

static int index_for_cell(const QList<int> &map, int cell) {
	if (map.isEmpty()) return cell;

	for (int i = 0; i < map.size(); ++i) {
		if (map.at(i) >= cell) return i;
	}
	return map.size();
}

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

/*
 * The scrollback and then the visible screen, as one stream.
 *
 * History is most of what a terminal has to say, and leaving it out made
 * the reader's view stop at whatever happened to be on screen -- a
 * build log a sighted user can scroll back through was simply gone. The
 * two are one text because that is what they are to somebody reading:
 * the boundary between them is a scroll position, not a document break.
 */
const bssh_terminal_accessible::flat_t &bssh_terminal_accessible::flatten() const {
	const bssh_terminal_view *terminal = view();
	const quint64 revision = terminal ? terminal->content_revision() : 0;

	if (m_flat.valid && m_flat.revision == revision) return m_flat;

	m_flat = flat_t();
	m_flat.revision = revision;
	m_flat.valid = true;

	if (!terminal) return m_flat;

	const auto append_row = [this](const QString &line, QList<int> cells) {
		if (!m_flat.row_start.isEmpty()) m_flat.text.append(QLatin1Char('\n'));
		m_flat.row_start.append(m_flat.text.size());
		m_flat.row_length.append(line.size());
		m_flat.text.append(line);

		/*
		 * Trailing blanks are trimmed off the text, so the map goes with
		 * them; and a row whose columns are its indices is stored as
		 * nothing at all, which is every row of ordinary text.
		 */
		cells.resize(qMin(cells.size(), line.size()));

		bool identity = true;
		for (int i = 0; i < cells.size(); ++i) {
			if (cells.at(i) != i) { identity = false; break; }
		}

		m_flat.row_cells.append(identity ? QList<int>() : cells);
	};

	const auto trimmed = [](QString line) {
		while (line.endsWith(QLatin1Char(' '))) line.chop(1);
		return line;
	};

	/*
	 * Appended as code units rather than through QString::fromUcs4, which
	 * builds and destroys a whole QString per character. On a full 5000
	 * line buffer that was the difference between rebuilding in 16.7 ms
	 * and rebuilding in a fraction of it -- and this runs whenever the
	 * screen changes while a reader is attached.
	 */
	const auto append_char = [](QString &line, char32_t ch) {
		if (ch >= 0x10000 && ch <= 0x10FFFF) {
			line.append(QChar::highSurrogate(ch));
			line.append(QChar::lowSurrogate(ch));
			return;
		}
		line.append(QChar(char16_t(ch)));
	};

	const bssh_scrollback *history = terminal->scrollback();
	if (history) {
		for (int i = 0; i < history->count(); ++i) {
			QString line;
			QList<int> columns;
			const QList<bssh_cell_t> &cells = history->line(i);
			line.reserve(cells.size());
			for (int col = 0; col < cells.size(); ++col) {
				if (cells.at(col).width == 0) continue;

				const int before = line.size();
				append_char(line, cells.at(col).ch);

				/* One entry per CODE UNIT: offsets into the text are
				 * UTF-16, so a surrogate pair maps twice to one cell. */
				for (int unit = before; unit < line.size(); ++unit) columns.append(col);
			}
			append_row(trimmed(line), columns);
		}
		m_flat.history_rows = history->count();
	}

	const bssh_screen *screen = terminal->screen();
	if (!screen) return m_flat;

	for (int row = 0; row < screen->rows(); ++row) {
		QString line;
		QList<int> columns;
		line.reserve(screen->cols());
		for (int col = 0; col < screen->cols(); ++col) {
			const bssh_cell_t &cell = screen->cell(row, col);
			/* The far half of a wide glyph is not a character (sec 9.3). */
			if (cell.width == 0) continue;

			const int before = line.size();
			append_char(line, cell.ch);
			for (int unit = before; unit < line.size(); ++unit) columns.append(col);
		}
		append_row(trimmed(line), columns);
	}

	return m_flat;
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

	const flat_t &flat = flatten();
	if (flat.row_start.isEmpty()) return 0;

	/* The cursor is on the SCREEN, which starts after the history. */
	const int row = qBound(0, flat.history_rows + screen->cursor_row(),
	                        flat.row_start.size() - 1);

	/*
	 * Clamped to the row's own length, because trailing blanks are not in
	 * the text: a cursor sitting past the last character of its line
	 * belongs at the end of that line, not inside the next one.
	 */
	const int index = index_for_cell(flat.row_cells.at(row), screen->cursor_col());
	return flat.row_start.at(row) + qBound(0, index, flat.row_length.at(row));
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

	const flat_t &flat = flatten();
	for (int row = flat.row_start.size() - 1; row >= 0; --row) {
		if (offset < flat.row_start.at(row)) continue;

		/*
		 * Only the visible screen has a rectangle. A line that has
		 * scrolled into history is not on screen at all, and inventing
		 * coordinates for it would send a magnifier somewhere blank.
		 */
		/*
		 * Asked of the VIEW, which owns the mapping and accounts for the
		 * grid origin, the inset and the scroll offset. This used to
		 * compute its own, and got all three wrong: the bare inset
		 * rather than grid_origin_x(), so a left-hand scrollbar put
		 * every rectangle two columns out; a screen row rather than a
		 * view row, so everything moved while scrolled back and history
		 * rows plainly on screen reported nothing; and a character index
		 * used as a cell column.
		 *
		 * A null rectangle is the honest answer for a line that is not
		 * on screen -- inventing coordinates sends a magnifier somewhere
		 * blank.
		 */
		const int cell = cell_for_index(flat.row_cells.at(row),
		                                 offset - flat.row_start.at(row));
		const QRect local = terminal->cell_rect(row, cell);
		if (local.isNull()) return QRect();

		return local.translated(terminal->mapToGlobal(QPoint(0, 0)));
	}
	return QRect();
}

int bssh_terminal_accessible::offsetAtPoint(const QPoint &point) const {
	const bssh_terminal_view *terminal = view();
	if (!terminal || terminal->cell_width() <= 0 || terminal->cell_height() <= 0) return -1;

	const flat_t &flat = flatten();
	if (flat.row_start.isEmpty()) return -1;

	/*
	 * The view's own hit test, for the reason characterRect uses its own
	 * rectangle: it already knows about the grid origin and the scroll
	 * offset, and it answers in ABSOLUTE lines, which is how the rows
	 * below are numbered. The copy that used to live here knew about
	 * neither, so touch exploration announced the wrong line whenever the
	 * view was scrolled back and could never reach the history at all.
	 */
	qint64 line = 0;
	int cell = 0;
	if (!terminal->cell_at(terminal->mapFromGlobal(point), line, cell)) return -1;

	const int row = int(qBound<qint64>(0, line, flat.row_start.size() - 1));
	const int index = index_for_cell(flat.row_cells.at(row), cell);
	return flat.row_start.at(row) + qBound(0, index, flat.row_length.at(row));
}

/*
 * The view's selection, in this interface's offsets.
 *
 * It could not be reported while only the visible screen was exposed: a
 * selection that starts in the scrollback had no offset to start AT, and
 * a range reported against the wrong coordinates is worse than none.
 * Flattening history and screen together is what makes it expressible,
 * so the two arrived in one piece.
 */
int bssh_terminal_accessible::selectionCount() const {
	const bssh_terminal_view *terminal = view();
	return terminal && terminal->has_selection() ? 1 : 0;
}

void bssh_terminal_accessible::selection(int index, int *start_offset, int *end_offset) const {
	if (start_offset) *start_offset = 0;
	if (end_offset) *end_offset = 0;
	if (index != 0) return;

	const bssh_terminal_view *terminal = view();
	if (!terminal) return;

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	if (!terminal->selection_range(first_line, first_col, last_line, last_col)) return;

	/*
	 * The view counts absolute lines from the top of the scrollback,
	 * which is exactly how the rows below are ordered -- so the two agree
	 * without a conversion, which is the point of exposing both.
	 */
	const flat_t &flat = flatten();
	if (flat.row_start.isEmpty()) return;

	const auto offset_of = [&flat](qint64 line, int col) {
		const int row = int(qBound<qint64>(0, line, flat.row_start.size() - 1));

		/* A cell column from the view, an offset for the reader. */
		const int index = index_for_cell(flat.row_cells.at(row), col);
		return flat.row_start.at(row) + qBound(0, index, flat.row_length.at(row));
	};

	if (start_offset) *start_offset = offset_of(first_line, first_col);
	if (end_offset) *end_offset = offset_of(last_line, last_col);
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
