#ifndef BSSH_BOX_DRAWING_H
#define BSSH_BOX_DRAWING_H

#include <QColor>
#include <QRect>

class QPainter;

/*
 * Box drawing (U+2500-257F) and block elements (U+2580-259F) drawn as
 * shapes filling the cell exactly, rather than as the font's glyphs
 * (project.md sec 9). A glyph is drawn to the font's own advance and its
 * antialiased ends overlap or fall short of the cell's, so a run of U+2500
 * showed a dot at every cell boundary -- the rules a TUI draws round
 * its prompt, the holder, 2026-11-13 -- brighter where two glyphs
 * overlapped at 100%, darker where they fell short at 125% and 150%.
 * xterm, Konsole, VTE, kitty, Alacritty, WezTerm and foot all draw
 * these themselves for that reason, and Windows Terminal since 2024.
 *
 * Lines are whole-pixel rectangles from the cell's middle to its edges,
 * so the arms of neighbouring cells meet edge to edge at any font, size
 * and scale. Light lines are a sixteenth of the cell's height, at least a
 * pixel; heavy twice that; a double line is two light ones a light
 * line's width apart. Arcs and diagonals are antialiased paths. The
 * shades are the colour at a quarter, a half and three quarters over
 * what is behind, rather than a dither, which would show its own grid.
 */

/* Whether `ch` is one this draws. */
bool bssh_box_drawn(char32_t ch);

/* Draws `ch` in `cell` in `colour`; false, drawing nothing, for a
 * character it does not draw. */
bool bssh_draw_box(QPainter &painter, char32_t ch, const QRect &cell, const QColor &colour);

/*
 * The table for a line character: its arm in each direction -- left,
 * up, right, down -- as 0 none, 1 light, 2 heavy, 3 double. All zero for
 * the dashed, arced and diagonal ones, which are drawn by shape, and for
 * anything outside U+2500-257F. Exposed so a test can hold it against a
 * font's own glyphs.
 */
struct bssh_box_arms_t {
	int left = 0;
	int up = 0;
	int right = 0;
	int down = 0;
};
bssh_box_arms_t bssh_box_arms(char32_t ch);

#endif
