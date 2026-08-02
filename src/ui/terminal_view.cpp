#include "ui/terminal_view.h"

#include <QFontDatabase>
#include <QPen>
#include <QFontMetricsF>

#include <algorithm>
#include <QPainter>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>

#include "term/emulator.h"
#include <QResizeEvent>

#include "input/input_router.h"

/*
 * The placeholder drawn into the empty grid. It exists to prove the cell
 * metrics are right -- it is painted one cell at a time, exactly the way
 * the emulator's row renderer will paint at M2, so a wrong advance width
 * shows up as visibly uneven spacing rather than as a subtle bug later.
 */
static const char *const BSSH_PLACEHOLDER = "BeerSSH -- no session (M0 skeleton)";

bssh_terminal_view::bssh_terminal_view(QWidget *parent)
    : QWidget(parent),
      m_screen(nullptr),
      m_images(nullptr),
      m_scrollback(nullptr),
      m_scroll_offset(0),
      m_focus_border(false),
      m_click_count(0),
      m_last_click_line(-1),
      m_selecting(false),
      m_selection_anchor_line(-1),
      m_selection_anchor_col(0),
      m_selection_focus_line(-1),
      m_selection_focus_col(0),
      m_emulator(nullptr),
      m_input_router(nullptr),
      m_background(QColor(0x1c, 0x1c, 0x1c)),
      m_foreground(QColor(0xd0, 0xd0, 0xd0)),
      m_cell_width(1),
      m_cell_height(1),
      m_baseline(0),
      m_cols(0),
      m_rows(0) {
	/*
	 * The curated bundled set arrives at M9 (sec 9.2); until then the
	 * platform's own fixed-pitch font is the honest placeholder. Asking
	 * for the system fixed font rather than naming a family is what keeps
	 * this working identically on a phone and a workstation.
	 */
	m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	m_font.setStyleHint(QFont::Monospace);
	m_font.setFixedPitch(true);

	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(false);
	/* Without this Qt never routes input-method events here at all. */
	setAttribute(Qt::WA_InputMethodEnabled);
	setAttribute(Qt::WA_OpaquePaintEvent);
	update_metrics();

	/*
	 * Compute the grid up front rather than waiting for the first
	 * resizeEvent. Qt does not deliver one to a widget that has never
	 * been shown, so without this the view reports a 0x0 grid until it is
	 * mapped -- and anything that arrives in the meantime (an SSH banner
	 * beats the first paint comfortably) gets trimmed away against a row
	 * count of zero. The emission is a no-op here: nothing can be
	 * connected to a widget still inside its own constructor.
	 */
	recompute_grid();
}

void bssh_terminal_view::set_terminal_font(const QFont &font) {
	m_font = font;
	update_metrics();
	recompute_grid();
	update();
}

void bssh_terminal_view::attach_screen(const bssh_screen *screen) {
	m_screen = screen;
	update();
}

void bssh_terminal_view::attach_images(const bssh_image_store *images) {
	m_images = images;
	update();
}

void bssh_terminal_view::attach_scrollback(const bssh_scrollback *scrollback) {
	m_scrollback = scrollback;
	update();
}

void bssh_terminal_view::set_scroll_offset(int lines) {
	const int available = m_scrollback ? m_scrollback->count() : 0;
	const int clamped = qBound(0, lines, available);

	if (clamped == m_scroll_offset) return;

	m_scroll_offset = clamped;
	update();
}

void bssh_terminal_view::scroll_by(int lines) {
	set_scroll_offset(m_scroll_offset + lines);
}

void bssh_terminal_view::set_focus_border(bool focused) {
	if (m_focus_border == focused) return;

	m_focus_border = focused;
	update();
}

void bssh_terminal_view::scroll_to_live() {
	set_scroll_offset(0);
}

void bssh_terminal_view::wheelEvent(QWheelEvent *event) {
	if (!m_scrollback) {
		QWidget::wheelEvent(event);
		return;
	}

	/*
	 * Three lines per notch, the usual desktop convention. Touch
	 * (QScroller, sec 6.4) will drive the same set_scroll_offset rather
	 * than growing a second scrolling path.
	 */
	const int notches = event->angleDelta().y() / 120;
	if (notches != 0) scroll_by(notches * 3);

	event->accept();
}

/*
 * The xterm 256-colour palette: 16 system colours, a 6x6x6 cube, then 24
 * greys. Built rather than tabulated -- the cube and the ramp are exact
 * formulas, and a 256-entry literal is 256 chances to fat-finger one.
 *
 * The 16 system colours are the one part that is a choice rather than a
 * formula, and at M10 they become a preset-owned colour scheme (sec 10.2).
 */
QColor bssh_terminal_view::resolve(const bssh_color_t &color, const QColor &fallback) const {
	static const QRgb system[16] = {
		qRgb(0x00, 0x00, 0x00), qRgb(0xcd, 0x00, 0x00),
		qRgb(0x00, 0xcd, 0x00), qRgb(0xcd, 0xcd, 0x00),
		qRgb(0x00, 0x00, 0xee), qRgb(0xcd, 0x00, 0xcd),
		qRgb(0x00, 0xcd, 0xcd), qRgb(0xe5, 0xe5, 0xe5),
		qRgb(0x7f, 0x7f, 0x7f), qRgb(0xff, 0x00, 0x00),
		qRgb(0x00, 0xff, 0x00), qRgb(0xff, 0xff, 0x00),
		qRgb(0x5c, 0x5c, 0xff), qRgb(0xff, 0x00, 0xff),
		qRgb(0x00, 0xff, 0xff), qRgb(0xff, 0xff, 0xff)
	};

	if (color.kind == bssh_color_t::RGB) return QColor(color.rgb);
	if (color.kind != bssh_color_t::INDEXED) return fallback;

	const int index = color.index;
	if (index < 16) return QColor(system[index]);

	if (index < 232) {
		const int offset = index - 16;
		const int steps[6] = {0, 95, 135, 175, 215, 255};
		return QColor(steps[(offset / 36) % 6], steps[(offset / 6) % 6], steps[offset % 6]);
	}

	const int grey = 8 + (index - 232) * 10;
	return QColor(grey, grey, grey);
}

void bssh_terminal_view::update_metrics() {
	const QFontMetricsF metrics(m_font);

	/*
	 * Rounded, never fractional: the grid has to land on whole pixels or
	 * the accumulated error across a wide window blurs the right-hand
	 * columns (sec 9.5). Fractional display scaling (125%, 150%) is
	 * exactly where this matters, which is a desktop case Android never
	 * exercises.
	 *
	 * 'W' is the measured glyph rather than the font's averageCharWidth,
	 * which is an average over a proportional-ish sample and is wrong for
	 * some fonts that ARE monospace. A font whose glyphs disagree about
	 * advance width is not a terminal font at all -- sec 9.6's CI check
	 * exists to keep any such font out of the bundled set.
	 */
	m_cell_width = qMax(1, qRound(metrics.horizontalAdvance(QLatin1Char('W'))));
	m_cell_height = qMax(1, qRound(metrics.height()));
	m_baseline = qRound(metrics.ascent());
}

void bssh_terminal_view::recompute_grid() {
	const int cols = qMax(1, width() / m_cell_width);
	const int rows = qMax(1, height() / m_cell_height);

	if (cols == m_cols && rows == m_rows) return;

	m_cols = cols;
	m_rows = rows;
	emit grid_resized(m_cols, m_rows);
}

void bssh_terminal_view::keyPressEvent(QKeyEvent *event) {
	/*
	 * Both paths matter, and this is only one of them. Android delivers
	 * some keys as IME commit text rather than key events (sec 6.1), so
	 * inputMethodEvent needs the same forwarding before the Android build
	 * is usable with a soft keyboard. Noted rather than stubbed, because
	 * a stub here would look done.
	 */
	/*
	 * Typing returns to the live screen. Anything else means a keystroke
	 * disappears into history the user is reading -- they type, nothing
	 * appears, and the terminal looks broken.
	 */
	if (m_input_router && m_input_router->handle_key(event)) {
		scroll_to_live();
		event->accept();
		return;
	}

	QWidget::keyPressEvent(event);
}

void bssh_terminal_view::inputMethodEvent(QInputMethodEvent *event) {
	/*
	 * The second delivery path sec 6.1 warns about. On Android some keys
	 * arrive only as IME commit text and never as key events; on desktop
	 * this is where dead keys and compose sequences arrive. A terminal
	 * that implements only keyPressEvent silently drops both.
	 *
	 * Pre-edit text is deliberately ignored: an in-progress composition
	 * is not input yet, and echoing it to the remote would send
	 * characters the user has not committed to.
	 */
	if (m_input_router && !event->commitString().isEmpty()) {
		m_input_router->send_text(event->commitString());
		event->accept();
		return;
	}

	QWidget::inputMethodEvent(event);
}

void bssh_terminal_view::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	recompute_grid();
}

void bssh_terminal_view::paintEvent(QPaintEvent *event) {
	Q_UNUSED(event);

	QPainter painter(this);
	painter.fillRect(rect(), m_background);
	painter.setFont(m_font);
	painter.setPen(m_foreground);

	if (!m_screen) {
		/* No session yet: name the app rather than showing a blank void. */
		const QString placeholder = QString::fromLatin1(BSSH_PLACEHOLDER);
		for (int col = 0; col < placeholder.size() && col < m_cols; ++col) {
			painter.drawText(QPointF(col * m_cell_width, m_baseline), QString(placeholder.at(col)));
		}
		return;
	}

	const int rows = qMin(m_rows, m_screen->rows());
	const int cols = qMin(m_cols, m_screen->cols());

	/*
	 * With an offset, the top of the view comes from the scrollback and
	 * the live screen slides down out of sight by the same amount. The
	 * screen itself is untouched: scrolling back is something the VIEW
	 * does, so the session carries on and the grid the remote believes in
	 * never changes.
	 */
	const int history = m_scrollback ? m_scrollback->count() : 0;
	const int offset = qBound(0, m_scroll_offset, history);

	/*
	 * Cell by cell, and per-cell QFont changes for bold/italic. That is
	 * the naive renderer on purpose: sec 9.5's shared glyph atlas and
	 * per-row glyph runs are M9, and building a cache now -- before the
	 * font pipeline exists and before there is anything to profile --
	 * would be a cache written to be thrown away.
	 */
	for (int row = 0; row < rows; ++row) {
		const qreal baseline = m_baseline + row * m_cell_height;

		/*
		 * Rows above the fold come from history; the rest from the live
		 * screen, shifted down by however far back the view is.
		 */
		const int history_index = history - offset + row;
		const bool from_history = offset > 0 && history_index < history;
		const QList<bssh_cell_t> &history_line =
		        from_history ? m_scrollback->line(history_index) : QList<bssh_cell_t>();
		const int screen_row = row - offset;

		for (int col = 0; col < cols; ++col) {
			static const bssh_cell_t blank;
			const bssh_cell_t &cell =
			        from_history ? (col < history_line.size() ? history_line.at(col) : blank)
			                     : (screen_row >= 0 ? m_screen->cell(screen_row, col) : blank);

			/* The far half of a double-width glyph: already drawn. */
			if (cell.width == 0) continue;

			QColor fg = resolve(cell.fg, m_foreground);
			QColor bg = resolve(cell.bg, m_background);
			if (cell.attrs & bssh_cell_t::INVERSE) std::swap(fg, bg);

			/*
			 * Selection inverts rather than tinting: it stays visible
			 * whatever colours the cell already carries, which a fixed
			 * highlight colour cannot promise against 256 possible
			 * backgrounds.
			 */
			if (is_selected(history - offset + row, col)) std::swap(fg, bg);

			const int x = col * m_cell_width;
			const int y = row * m_cell_height;
			const int width = qMax(1, int(cell.width)) * m_cell_width;

			if (bg != m_background) painter.fillRect(x, y, width, m_cell_height, bg);

			if (cell.ch != U'\0' && cell.ch != U' ') {
				if (cell.attrs & (bssh_cell_t::BOLD | bssh_cell_t::ITALIC)) {
					QFont font = m_font;
					font.setBold(cell.attrs & bssh_cell_t::BOLD);
					font.setItalic(cell.attrs & bssh_cell_t::ITALIC);
					painter.setFont(font);
				} else {
					painter.setFont(m_font);
				}

				painter.setPen(fg);
				painter.drawText(QPointF(x, baseline), QString::fromUcs4(&cell.ch, 1));
			}

			if (cell.attrs & bssh_cell_t::UNDERLINE) {
				painter.setPen(fg);
				painter.drawLine(x, y + m_cell_height - 1, x + width - 1, y + m_cell_height - 1);
			}
		}
	}

	/*
	 * Images over the cells they cover, except where the remote asked for
	 * z < 0 -- kitty's way of putting a background behind a TUI. Drawn
	 * after the text for the positive case, which is why the loop runs
	 * twice rather than sorting: two passes over a handful of images is
	 * cheaper to read than a comparator.
	 */
	if (m_images) {
		for (const bssh_image_placement_t &placement : m_images->placements()) {
			const QRect target(placement.col * m_cell_width,
			                    placement.row * m_cell_height,
			                    placement.cols * m_cell_width,
			                    placement.rows * m_cell_height);
			painter.drawImage(target, placement.image);
		}
	}

	if (m_focus_border) {
		/*
		 * Drawn last, over everything, and inset by half a pen width so
		 * the whole line lands inside the widget rather than half of it
		 * being clipped away by the neighbouring pane.
		 */
		QPen border(QColor(0x2b, 0x90, 0xd9));
		border.setWidth(2);
		painter.setPen(border);
		painter.drawRect(rect().adjusted(1, 1, -1, -1));
	}

	/* No cursor while reading history: it belongs to the live screen. */
	if (m_screen->cursor_visible() && offset == 0) {
		/*
		 * A filled block with the character punched out of it. Drawing
		 * the glyph again in the background colour is what keeps the
		 * character under the cursor readable -- a solid block that
		 * simply covers it hides the one cell the user is looking at.
		 */
		const int x = m_screen->cursor_col() * m_cell_width;
		const int y = m_screen->cursor_row() * m_cell_height;
		const bssh_cell_t &cell = m_screen->cell(m_screen->cursor_row(), m_screen->cursor_col());

		painter.fillRect(x, y, m_cell_width, m_cell_height, m_foreground);
		if (cell.ch != U'\0' && cell.ch != U' ') {
			painter.setFont(m_font);
			painter.setPen(m_background);
			painter.drawText(QPointF(x, m_baseline + m_screen->cursor_row() * m_cell_height),
			                  QString::fromUcs4(&cell.ch, 1));
		}
	}
}

/*
 * Absolute line numbering: history lines come first, the live screen
 * continues straight on from them. One coordinate space for both is what
 * lets a selection span the fold and survive scrolling.
 */
qint64 bssh_terminal_view::line_for_row(int row) const {
	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	return history - m_scroll_offset + row;
}

const bssh_cell_t &bssh_terminal_view::cell_for_line(qint64 line, int col) const {
	static const bssh_cell_t blank;

	const qint64 history = m_scrollback ? m_scrollback->count() : 0;
	if (line < 0) return blank;

	if (line < history) {
		const QList<bssh_cell_t> &stored = m_scrollback->line(int(line));
		return col >= 0 && col < stored.size() ? stored.at(col) : blank;
	}

	if (!m_screen) return blank;
	return m_screen->cell(int(line - history), col);
}

bool bssh_terminal_view::cell_at(const QPoint &point, qint64 &line, int &col) const {
	if (m_cell_width <= 0 || m_cell_height <= 0) return false;

	const int row = qBound(0, point.y() / m_cell_height, qMax(0, m_rows - 1));
	col = qBound(0, point.x() / m_cell_width, qMax(0, m_cols - 1));
	line = line_for_row(row);
	return true;
}

void bssh_terminal_view::normalised_selection(qint64 &first_line, int &first_col,
                                               qint64 &last_line, int &last_col) const {
	first_line = m_selection_anchor_line;
	first_col = m_selection_anchor_col;
	last_line = m_selection_focus_line;
	last_col = m_selection_focus_col;

	/* Dragging upwards or leftwards is the same selection, backwards. */
	if (last_line < first_line || (last_line == first_line && last_col < first_col)) {
		qSwap(first_line, last_line);
		qSwap(first_col, last_col);
	}
}

bool bssh_terminal_view::is_selected(qint64 line, int col) const {
	if (!has_selection()) return false;

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	if (line < first_line || line > last_line) return false;
	if (line == first_line && col < first_col) return false;
	if (line == last_line && col > last_col) return false;
	return true;
}

bool bssh_terminal_view::has_selection() const {
	if (m_selection_anchor_line < 0 || m_selection_focus_line < 0) return false;
	return m_selection_anchor_line != m_selection_focus_line
	        || m_selection_anchor_col != m_selection_focus_col;
}

void bssh_terminal_view::clear_selection() {
	if (m_selection_anchor_line < 0) return;

	m_selection_anchor_line = -1;
	m_selection_focus_line = -1;
	update();
	emit selection_changed();
}

QString bssh_terminal_view::selection_text() const {
	if (!has_selection()) return QString();

	qint64 first_line = 0;
	int first_col = 0;
	qint64 last_line = 0;
	int last_col = 0;
	normalised_selection(first_line, first_col, last_line, last_col);

	QStringList lines;
	for (qint64 line = first_line; line <= last_line; ++line) {
		const int from = line == first_line ? first_col : 0;
		const int to = line == last_line ? last_col : m_cols - 1;

		QString text;
		for (int col = from; col <= to; ++col) {
			const bssh_cell_t &cell = cell_for_line(line, col);
			if (cell.width == 0) continue;
			text.append(QString::fromUcs4(&cell.ch, 1));
		}

		/*
		 * Trailing blanks are padding, not content: a terminal line is
		 * always m_cols wide, and pasting the padding back into a shell
		 * is how a copied command arrives with a tail of spaces.
		 */
		while (text.endsWith(QLatin1Char(' '))) text.chop(1);
		lines.append(text);
	}

	return lines.join(QLatin1Char('\n'));
}

void bssh_terminal_view::mousePressEvent(QMouseEvent *event) {
	/*
	 * When the remote has asked for the mouse, clicks are its business --
	 * unless Shift is held, the conventional override that lets a user
	 * select text out of a full-screen program (sec 6.4).
	 */
	const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
	if (m_emulator && m_emulator->mouse_reporting() && !shift) {
		qint64 line = 0;
		int col = 0;
		if (cell_at(event->pos(), line, col)) {
			m_emulator->send_mouse_move(int(line - line_for_row(0)), col, 0);
			m_emulator->send_mouse_button(event->button() == Qt::RightButton ? 3
			                               : event->button() == Qt::MiddleButton ? 2 : 1,
			                               true, 0);
		}
		event->accept();
		return;
	}

	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	qint64 line = 0;
	int col = 0;
	if (!cell_at(event->pos(), line, col)) return;

	/*
	 * Qt delivers a triple-click as press, double-click, press -- there is
	 * no triple-click event -- so the third press is counted here. The
	 * interval is the platform's own double-click time, because a
	 * hardcoded one is wrong for anyone who has changed it, and the row
	 * must match too: three clicks that wandered onto another line are
	 * three clicks, not a line selection.
	 */
	const bool in_time = m_multi_click_timer.isValid()
	        && m_multi_click_timer.elapsed() < QApplication::doubleClickInterval();
	m_click_count = (in_time && line == m_last_click_line) ? m_click_count + 1 : 1;
	m_multi_click_timer.restart();
	m_last_click_line = line;

	if (m_click_count >= 3) {
		/*
		 * The whole line, trailing blanks included in the range but
		 * trimmed by selection_text() -- selecting a line should give
		 * the line, not the line plus the padding a terminal row always
		 * carries.
		 */
		m_selection_anchor_line = line;
		m_selection_anchor_col = 0;
		m_selection_focus_line = line;
		m_selection_focus_col = qMax(0, m_cols - 1);
		m_selecting = false;
		m_click_count = 0;

		update();
		emit selection_changed();
		event->accept();
		return;
	}

	m_selecting = true;
	m_selection_anchor_line = line;
	m_selection_anchor_col = col;
	m_selection_focus_line = line;
	m_selection_focus_col = col;

	update();
	event->accept();
}

void bssh_terminal_view::mouseMoveEvent(QMouseEvent *event) {
	if (!m_selecting) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	qint64 line = 0;
	int col = 0;
	if (cell_at(event->pos(), line, col)) {
		m_selection_focus_line = line;
		m_selection_focus_col = col;
		update();
	}

	event->accept();
}

void bssh_terminal_view::mouseReleaseEvent(QMouseEvent *event) {
	if (m_emulator && m_emulator->mouse_reporting()
	    && !event->modifiers().testFlag(Qt::ShiftModifier)) {
		m_emulator->send_mouse_button(1, false, 0);
		event->accept();
		return;
	}

	if (!m_selecting) {
		QWidget::mouseReleaseEvent(event);
		return;
	}

	m_selecting = false;
	emit selection_changed();
	event->accept();
}

void bssh_terminal_view::mouseDoubleClickEvent(QMouseEvent *event) {
	qint64 line = 0;
	int col = 0;
	if (event->button() != Qt::LeftButton || !cell_at(event->pos(), line, col)) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

	/*
	 * Word selection, using the same word characters a shell does -- a
	 * double-click on a path should take the whole path, which is most of
	 * what anyone double-clicks in a terminal for.
	 */
	const QString word_chars = QStringLiteral("_-./~:@");
	int from = col;
	int to = col;

	while (from > 0) {
		const QChar previous = QChar(cell_for_line(line, from - 1).ch);
		if (!previous.isLetterOrNumber() && !word_chars.contains(previous)) break;
		--from;
	}
	while (to < m_cols - 1) {
		const QChar next = QChar(cell_for_line(line, to + 1).ch);
		if (!next.isLetterOrNumber() && !word_chars.contains(next)) break;
		++to;
	}

	m_selection_anchor_line = line;
	m_selection_anchor_col = from;
	m_selection_focus_line = line;
	m_selection_focus_col = to;
	m_selecting = false;

	/*
	 * The double-click counts itself. Qt delivers press, double-click,
	 * press for a triple-click, and a double-click does NOT arrive as a
	 * press -- so counting only presses leaves the third one at two, and
	 * the line selection never fires.
	 */
	m_click_count = 2;
	m_multi_click_timer.restart();
	m_last_click_line = line;

	update();
	emit selection_changed();
	event->accept();
}
