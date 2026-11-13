#include "term/box_drawing.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace {

/*
 * U+2500-257F as left, up, right, down: 0 none, 1 light, 2 heavy,
 * 3 double; "x" for the ones drawn by shape (dashes, arcs, diagonals).
 * Written from the Unicode names, row by row of sixteen, and held
 * against a font's glyphs by box_drawing_matches_a_font in the suite.
 */
const char *const arms_table[128] = {
	/* 2500 */ "1010", "2020", "0101", "0202", "x", "x", "x", "x",
	/* 2508 */ "x", "x", "x", "x", "0011", "0021", "0012", "0022",
	/* 2510 */ "1001", "2001", "1002", "2002", "0110", "0120", "0210", "0220",
	/* 2518 */ "1100", "2100", "1200", "2200", "0111", "0121", "0211", "0112",
	/* 2520 */ "0212", "0221", "0122", "0222", "1101", "2101", "1201", "1102",
	/* 2528 */ "1202", "2201", "2102", "2202", "1011", "2011", "1021", "2021",
	/* 2530 */ "1012", "2012", "1022", "2022", "1110", "2110", "1120", "2120",
	/* 2538 */ "1210", "2210", "1220", "2220", "1111", "2111", "1121", "2121",
	/* 2540 */ "1211", "1112", "1212", "2211", "1221", "2112", "1122", "2221",
	/* 2548 */ "2122", "2212", "1222", "2222", "x", "x", "x", "x",
	/* 2550 */ "3030", "0303", "0031", "0013", "0033", "3001", "1003", "3003",
	/* 2558 */ "0130", "0310", "0330", "3100", "1300", "3300", "0131", "0313",
	/* 2560 */ "0333", "3101", "1303", "3303", "3031", "1013", "3033", "3130",
	/* 2568 */ "1310", "3330", "3131", "1313", "3333", "x", "x", "x",
	/* 2570 */ "x", "x", "x", "x", "1000", "0100", "0010", "0001",
	/* 2578 */ "2000", "0200", "0020", "0002", "1020", "0102", "2010", "0201",
};

struct span_t {
	int a = 0;
	int b = 0;
};

/* A line of `weight` across `centre`: light lw, heavy twice it. */
span_t line_span(int weight, int centre, int lw) {
	const int t = weight == 2 ? 2 * lw : lw;
	return { centre - t / 2, centre - t / 2 + t };
}

/* Fills [x0,x1) x [y0,y1), whole pixels, nothing when empty. */
void fill(QPainter &painter, int x0, int x1, int y0, int y1, const QColor &colour) {
	if (x1 > x0 && y1 > y0) painter.fillRect(x0, y0, x1 - x0, y1 - y0, colour);
}

void draw_lines(QPainter &painter, const bssh_box_arms_t &arms, const QRect &cell, const QColor &colour) {
	const int x0 = cell.left();
	const int y0 = cell.top();
	const int x1 = x0 + cell.width();
	const int y1 = y0 + cell.height();
	const int cx = x0 + cell.width() / 2;
	const int cy = y0 + cell.height() / 2;
	const int lw = std::max(1, qRound(cell.height() / 16.0));
	/* The double strokes either side of the centre, a light line apart. */
	const int c0x = cx - lw / 2;
	const int c0y = cy - lw / 2;
	const span_t left_stroke{ c0x - lw, c0x };
	const span_t right_stroke{ c0x + lw, c0x + 2 * lw };
	const span_t top_stroke{ c0y - lw, c0y };
	const span_t bottom_stroke{ c0y + lw, c0y + 2 * lw };
	const int L = arms.left;
	const int U = arms.up;
	const int R = arms.right;
	const int D = arms.down;
	const auto single = [](int weight) { return weight == 1 || weight == 2; };

	/*
	 * Where a double stroke meets the arms beside it: the inner corner
	 * where the arm on its side is double, the single line where that
	 * one is single, straight through where the opposite arm is there,
	 * the outer corner where only the far side's double is.
	 */

	/* Right. */
	if (single(R)) {
		const span_t y = line_span(R, cy, lw);
		int start;
		if (L) start = cx;
		else if (U == 3 && D == 3) start = right_stroke.a;
		else if (U == 3 || D == 3) start = left_stroke.a;
		else if (single(U) || single(D))
			start = std::min(single(U) ? line_span(U, cx, lw).a : x1, single(D) ? line_span(D, cx, lw).a : x1);
		else start = cx - lw / 2;
		fill(painter, start, x1, y.a, y.b, colour);
	} else if (R == 3) {
		const auto start = [&](int side, int other) {
			if (side == 3) return right_stroke.a;
			if (single(side)) return line_span(side, cx, lw).a;
			if (L) return cx;
			if (other == 3) return left_stroke.a;
			if (single(other)) return line_span(other, cx, lw).a;
			return cx;
		};
		fill(painter, start(U, D), x1, top_stroke.a, top_stroke.b, colour);
		fill(painter, start(D, U), x1, bottom_stroke.a, bottom_stroke.b, colour);
	}

	/* Left. */
	if (single(L)) {
		const span_t y = line_span(L, cy, lw);
		int end;
		if (R) end = cx;
		else if (U == 3 && D == 3) end = left_stroke.b;
		else if (U == 3 || D == 3) end = right_stroke.b;
		else if (single(U) || single(D))
			end = std::max(single(U) ? line_span(U, cx, lw).b : x0, single(D) ? line_span(D, cx, lw).b : x0);
		else end = cx - lw / 2 + lw;
		fill(painter, x0, end, y.a, y.b, colour);
	} else if (L == 3) {
		const auto end = [&](int side, int other) {
			if (side == 3) return left_stroke.b;
			if (single(side)) return line_span(side, cx, lw).b;
			if (R) return cx;
			if (other == 3) return right_stroke.b;
			if (single(other)) return line_span(other, cx, lw).b;
			return cx;
		};
		fill(painter, x0, end(U, D), top_stroke.a, top_stroke.b, colour);
		fill(painter, x0, end(D, U), bottom_stroke.a, bottom_stroke.b, colour);
	}

	/* Up. */
	if (single(U)) {
		const span_t x = line_span(U, cx, lw);
		int end;
		if (D) end = cy;
		else if (L == 3 && R == 3) end = top_stroke.b;
		else if (L == 3 || R == 3) end = bottom_stroke.b;
		else if (single(L) || single(R))
			end = std::max(single(L) ? line_span(L, cy, lw).b : y0, single(R) ? line_span(R, cy, lw).b : y0);
		else end = cy - lw / 2 + lw;
		fill(painter, x.a, x.b, y0, end, colour);
	} else if (U == 3) {
		const auto end = [&](int side, int other) {
			if (side == 3) return top_stroke.b;
			if (single(side)) return line_span(side, cy, lw).b;
			if (D) return cy;
			if (other == 3) return bottom_stroke.b;
			if (single(other)) return line_span(other, cy, lw).b;
			return cy;
		};
		fill(painter, left_stroke.a, left_stroke.b, y0, end(L, R), colour);
		fill(painter, right_stroke.a, right_stroke.b, y0, end(R, L), colour);
	}

	/* Down. */
	if (single(D)) {
		const span_t x = line_span(D, cx, lw);
		int start;
		if (U) start = cy;
		else if (L == 3 && R == 3) start = bottom_stroke.a;
		else if (L == 3 || R == 3) start = top_stroke.a;
		else if (single(L) || single(R))
			start = std::min(single(L) ? line_span(L, cy, lw).a : y1, single(R) ? line_span(R, cy, lw).a : y1);
		else start = cy - lw / 2;
		fill(painter, x.a, x.b, start, y1, colour);
	} else if (D == 3) {
		const auto start = [&](int side, int other) {
			if (side == 3) return bottom_stroke.a;
			if (single(side)) return line_span(side, cy, lw).a;
			if (U) return cy;
			if (other == 3) return top_stroke.a;
			if (single(other)) return line_span(other, cy, lw).a;
			return cy;
		};
		fill(painter, left_stroke.a, left_stroke.b, start(L, R), y1, colour);
		fill(painter, right_stroke.a, right_stroke.b, start(R, L), y1, colour);
	}
}

/* Dashed lines: `n` dashes across the cell, so a run of them repeats. */
void draw_dashes(QPainter &painter, const QRect &cell, const QColor &colour, bool vertical, int weight, int n) {
	const int lw = std::max(1, qRound(cell.height() / 16.0));
	const int length = vertical ? cell.height() : cell.width();
	const int gap = std::max(1, qRound(length / (3.0 * n)));
	const int origin = vertical ? cell.top() : cell.left();
	const span_t across = line_span(weight, vertical ? cell.left() + cell.width() / 2 : cell.top() + cell.height() / 2, lw);
	for (int k = 0; k < n; ++k) {
		const int a = origin + (k * length) / n + gap / 2;
		const int b = origin + ((k + 1) * length) / n - (gap - gap / 2);
		if (vertical) fill(painter, across.a, across.b, a, b, colour);
		else fill(painter, a, b, across.a, across.b, colour);
	}
}

/* The rounded corners: straight in from the two edges, turning through
 * a quarter circle at the centre, on the light line's centre. */
void draw_arc(QPainter &painter, char32_t ch, const QRect &cell, const QColor &colour) {
	const int lw = std::max(1, qRound(cell.height() / 16.0));
	const qreal cx = cell.left() + cell.width() / 2 - lw / 2 + lw / 2.0;
	const qreal cy = cell.top() + cell.height() / 2 - lw / 2 + lw / 2.0;
	const qreal r = std::min(cell.width(), cell.height()) / 2.0;
	const qreal right = cell.left() + cell.width();
	const qreal bottom = cell.top() + cell.height();
	QPainterPath path;
	switch (ch) {
	case U'╭': /* down and right */
		path.moveTo(right, cy); path.lineTo(cx + r, cy); path.quadTo(cx, cy, cx, cy + r); path.lineTo(cx, bottom);
		break;
	case U'╮': /* down and left */
		path.moveTo(cell.left(), cy); path.lineTo(cx - r, cy); path.quadTo(cx, cy, cx, cy + r); path.lineTo(cx, bottom);
		break;
	case U'╯': /* up and left */
		path.moveTo(cell.left(), cy); path.lineTo(cx - r, cy); path.quadTo(cx, cy, cx, cy - r); path.lineTo(cx, cell.top());
		break;
	default: /* up and right */
		path.moveTo(right, cy); path.lineTo(cx + r, cy); path.quadTo(cx, cy, cx, cy - r); path.lineTo(cx, cell.top());
		break;
	}
	painter.setRenderHint(QPainter::Antialiasing, true);
	QPen pen(colour, lw);
	pen.setCapStyle(Qt::FlatCap);
	painter.strokePath(path, pen);
}

void draw_diagonals(QPainter &painter, char32_t ch, const QRect &cell, const QColor &colour) {
	const int lw = std::max(1, qRound(cell.height() / 16.0));
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setClipRect(cell);
	painter.setPen(QPen(colour, lw));
	const QPointF tl(cell.left(), cell.top());
	const QPointF tr(cell.left() + cell.width(), cell.top());
	const QPointF bl(cell.left(), cell.top() + cell.height());
	const QPointF br(cell.left() + cell.width(), cell.top() + cell.height());
	if (ch == U'╱' || ch == U'╳') painter.drawLine(tr, bl);
	if (ch == U'╲' || ch == U'╳') painter.drawLine(tl, br);
}

/* U+2580-259F: halves, eighths, shades and quadrants. */
void draw_block(QPainter &painter, char32_t ch, const QRect &cell, const QColor &colour) {
	const int x0 = cell.left();
	const int y0 = cell.top();
	const int w = cell.width();
	const int h = cell.height();
	const int x1 = x0 + w;
	const int y1 = y0 + h;
	const int mx = x0 + w / 2;
	const int my = y0 + h / 2;
	const auto lower = [&](int eighths) { fill(painter, x0, x1, y1 - qRound(h * eighths / 8.0), y1, colour); };
	const auto left = [&](int eighths) { fill(painter, x0, x0 + qRound(w * eighths / 8.0), y0, y1, colour); };
	const auto shade = [&](int alpha) {
		QColor c = colour;
		c.setAlpha(alpha);
		painter.fillRect(cell, c);
	};
	const auto quadrants = [&](bool ul, bool ur, bool ll, bool lr) {
		if (ul) fill(painter, x0, mx, y0, my, colour);
		if (ur) fill(painter, mx, x1, y0, my, colour);
		if (ll) fill(painter, x0, mx, my, y1, colour);
		if (lr) fill(painter, mx, x1, my, y1, colour);
	};
	switch (ch) {
	case 0x2580: fill(painter, x0, x1, y0, my, colour); break;
	case 0x2581: lower(1); break;
	case 0x2582: lower(2); break;
	case 0x2583: lower(3); break;
	case 0x2584: fill(painter, x0, x1, my, y1, colour); break;
	case 0x2585: lower(5); break;
	case 0x2586: lower(6); break;
	case 0x2587: lower(7); break;
	case 0x2588: fill(painter, x0, x1, y0, y1, colour); break;
	case 0x2589: left(7); break;
	case 0x258A: left(6); break;
	case 0x258B: left(5); break;
	case 0x258C: fill(painter, x0, mx, y0, y1, colour); break;
	case 0x258D: left(3); break;
	case 0x258E: left(2); break;
	case 0x258F: left(1); break;
	case 0x2590: fill(painter, mx, x1, y0, y1, colour); break;
	case 0x2591: shade(64); break;
	case 0x2592: shade(128); break;
	case 0x2593: shade(191); break;
	case 0x2594: fill(painter, x0, x1, y0, y0 + qRound(h / 8.0), colour); break;
	case 0x2595: fill(painter, x1 - qRound(w / 8.0), x1, y0, y1, colour); break;
	case 0x2596: quadrants(false, false, true, false); break;
	case 0x2597: quadrants(false, false, false, true); break;
	case 0x2598: quadrants(true, false, false, false); break;
	case 0x2599: quadrants(true, false, true, true); break;
	case 0x259A: quadrants(true, false, false, true); break;
	case 0x259B: quadrants(true, true, true, false); break;
	case 0x259C: quadrants(true, true, false, true); break;
	case 0x259D: quadrants(false, true, false, false); break;
	case 0x259E: quadrants(false, true, true, false); break;
	default: quadrants(false, true, true, true); break; /* 259F */
	}
}

} /* namespace */

bool bssh_box_drawn(char32_t ch) {
	return ch >= 0x2500 && ch <= 0x259F;
}

bssh_box_arms_t bssh_box_arms(char32_t ch) {
	bssh_box_arms_t arms;
	if (ch < 0x2500 || ch > 0x257F) return arms;
	const char *entry = arms_table[ch - 0x2500];
	if (entry[0] == 'x') return arms;
	arms.left = entry[0] - '0';
	arms.up = entry[1] - '0';
	arms.right = entry[2] - '0';
	arms.down = entry[3] - '0';
	return arms;
}

bool bssh_draw_box(QPainter &painter, char32_t ch, const QRect &cell, const QColor &colour) {
	if (!bssh_box_drawn(ch) || cell.isEmpty()) return false;
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, false);
	if (ch >= 0x2580) {
		draw_block(painter, ch, cell, colour);
	} else if (ch >= 0x2504 && ch <= 0x250B) {
		/* Triple dashes then quadruple: light/heavy, horizontal/vertical. */
		const int i = int(ch - 0x2504);
		draw_dashes(painter, cell, colour, (i / 2) % 2 == 1, i % 2 == 0 ? 1 : 2, i < 4 ? 3 : 4);
	} else if (ch >= 0x254C && ch <= 0x254F) {
		const int i = int(ch - 0x254C);
		draw_dashes(painter, cell, colour, i >= 2, i % 2 == 0 ? 1 : 2, 2);
	} else if (ch >= 0x256D && ch <= 0x2570) {
		draw_arc(painter, ch, cell, colour);
	} else if (ch >= 0x2571 && ch <= 0x2573) {
		draw_diagonals(painter, ch, cell, colour);
	} else {
		draw_lines(painter, bssh_box_arms(ch), cell, colour);
	}
	painter.restore();
	return true;
}
