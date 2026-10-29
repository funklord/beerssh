#ifndef BSSH_SCREEN_H
#define BSSH_SCREEN_H

#include <QList>
#include <QRgb>
#include <QString>

/*
 * bssh_screen -- the character-cell grid an emulator writes and a
 * terminal view renders (project.md sec 7).
 *
 * This is deliberately the seam between the two, and it is the same
 * whichever parser ends up filling it: a vendored libvterm writing
 * through its damage callbacks, or a parser of our own. Neither the
 * renderer nor the tests should have to care, which is why this type
 * carries no parsing logic at all -- it is a grid, a cursor, and nothing
 * else.
 *
 * Scrollback is NOT here. The ring buffer belongs with the emulator that
 * decides what scrolls off the top; this is the visible screen only.
 */

struct bssh_color_t {
	/*
	 * Terminals have three kinds of colour and they are not
	 * interchangeable: "whatever the theme says" is a real value,
	 * distinct from an explicit palette entry that happens to look the
	 * same, and both differ from a truecolor triple. Flattening DEFAULT
	 * into an index is how a colour scheme change stops repainting old
	 * output correctly.
	 */
	enum kind_t : quint8 {
		DEFAULT,
		INDEXED,
		RGB
	};

	kind_t kind;
	quint8 index;
	QRgb rgb;

	bssh_color_t() : kind(DEFAULT), index(0), rgb(0) {}

	bool operator==(const bssh_color_t &other) const {
		if (kind != other.kind) return false;
		if (kind == INDEXED) return index == other.index;
		if (kind == RGB) return rgb == other.rgb;
		return true;
	}
};

struct bssh_cell_t {
	/* SGR attributes, as a bitmask so a cell stays small. */
	enum attr_t : quint8 {
		NONE      = 0x00,
		BOLD      = 0x01,
		ITALIC    = 0x02,
		UNDERLINE = 0x04,
		INVERSE   = 0x08,
		BLINK     = 0x10,
		STRIKE    = 0x20,
		DIM       = 0x40   /* SGR 2, faint (vterm-patch/0011) */
	};

	char32_t ch;
	bssh_color_t fg;
	bssh_color_t bg;
	quint8 attrs;

	/*
	 * 1 for a normal cell, 2 for the first half of a double-width glyph,
	 * 0 for the continuation cell it occupies (sec 9.3). Getting this
	 * wrong misaligns every TUI on a CJK locale, so it is a property of
	 * the cell rather than something the renderer re-derives.
	 */
	quint8 width;

	/*
	 * OSC 8 hyperlink id, 0 for none (sec 7.1, tier 3). An id rather than
	 * the URI itself: a long URL repeated across every cell of a wrapped
	 * link would dwarf the text it decorates, and the scrollback stores
	 * cells.
	 */
	quint16 link;

	bssh_cell_t() : ch(U' '), attrs(NONE), width(1), link(0) {}
};

class bssh_screen {
public:
	bssh_screen(int cols = 80, int rows = 24);

	int cols() const { return m_cols; }
	int rows() const { return m_rows; }

	/*
	 * Content is preserved from the top-left corner. Real terminals
	 * differ on whether a narrower window reflows wrapped lines or
	 * truncates them; that decision belongs to the emulator, which is the
	 * only thing that knows which rows were soft-wrapped.
	 */
	void resize(int cols, int rows);

	const bssh_cell_t &cell(int row, int col) const;
	void set_cell(int row, int col, const bssh_cell_t &cell);

	/*
	 * A whole row, cols() cells long, or nullptr outside the grid. The
	 * ring rotates whole rows, so a row's cells are always contiguous --
	 * which is what lets the scrollback take a departing row in one copy
	 * rather than a bounds-checked cell() per column.
	 */
	const bssh_cell_t *row_data(int row) const;

	void clear();
	void clear_row(int row);

	/*
	 * Move a block of cells within the grid, which is what a scroll is.
	 *
	 * Here rather than in the emulator because the grid owns its own
	 * storage and can move rows without looking at them: the emulator's
	 * only other option is to re-read every moved cell from the parser
	 * and convert it back, which is how a one-line scroll came to cost a
	 * full-screen copy (sec 9.5). Overlapping source and destination are
	 * expected -- that is the normal case -- and handled.
	 */
	void move_rect(int dest_row, int dest_col, int src_row, int src_col,
	                int rows, int cols);

	int cursor_row() const { return m_cursor_row; }
	int cursor_col() const { return m_cursor_col; }
	bool cursor_visible() const { return m_cursor_visible; }
	void set_cursor(int row, int col);
	void set_cursor_visible(bool visible) { m_cursor_visible = visible; }

	/*
	 * Plain-text dump, one line per row with trailing blanks removed.
	 * This is what the golden-file tests compare (sec 12.3): byte stream
	 * in, expected screen out. Attributes are deliberately not included
	 * -- a colour test that also asserts on layout fails for two reasons
	 * at once and tells you neither.
	 */
	QString to_text() const;

private:
	bool in_bounds(int row, int col) const;

	int m_cols;
	int m_rows;
	int m_cursor_row;
	int m_cursor_col;
	bool m_cursor_visible;
	QList<bssh_cell_t> m_cells;

	/*
	 * Which physical row logical row 0 lives in.
	 *
	 * The grid is a ring. Scrolling the whole screen up by one used to
	 * memmove every row -- ten thousand cells on a 200x50 grid, per
	 * scrolled line -- which is why throughput tracked the grid's AREA
	 * rather than its width. Rotating this instead is one addition, and
	 * the exposed row is filled by the damage libvterm already sends.
	 *
	 * Every access goes through physical(); nothing outside this class
	 * touches m_cells, which is what made the change containable.
	 */
	int m_first_row;

	int physical(int row) const {
		const int mapped = row + m_first_row;
		return mapped >= m_rows ? mapped - m_rows : mapped;
	}
};

#endif /* BSSH_SCREEN_H */
