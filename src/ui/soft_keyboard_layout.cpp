#include "ui/soft_keyboard_layout.h"

#include "term/emulator.h"

namespace {

/*
 * The other legend on each key, US ANSI. A table rather than a rule,
 * because there is no rule: Shift picks the character printed on the top
 * half of the keycap, and which character that is was decided by a layout
 * standard, not by arithmetic.
 */
char32_t shifted_twin(char32_t plain) {
	switch (plain) {
	case U'`': return U'~';
	case U'1': return U'!';
	case U'2': return U'@';
	case U'3': return U'#';
	case U'4': return U'$';
	case U'5': return U'%';
	case U'6': return U'^';
	case U'7': return U'&';
	case U'8': return U'*';
	case U'9': return U'(';
	case U'0': return U')';
	case U'-': return U'_';
	case U'=': return U'+';
	case U'[': return U'{';
	case U']': return U'}';
	case U'\\': return U'|';
	case U';': return U':';
	case U'\'': return U'"';
	case U',': return U'<';
	case U'.': return U'>';
	case U'/': return U'?';
	default:  return 0;
	}
}

bssh_key_t text_key(const QString &label, char32_t ch, int span = 2) {
	bssh_key_t key;
	key.kind = bssh_key_t::TEXT;
	key.label = label;
	key.value = int(ch);
	key.span = span;
	return key;
}

/* A character whose label IS what it sends, and its second legend. */
bssh_key_t literal(char ch) {
	bssh_key_t key = text_key(QString(QLatin1Char(ch)), char32_t(ch));
	key.shifted_value = int(shifted_twin(char32_t(ch)));
	return key;
}

bssh_key_t named_key(const QString &label, int code, int span = 2) {
	bssh_key_t key;
	key.kind = bssh_key_t::KEY;
	key.label = label;
	key.value = code;
	key.span = span;
	return key;
}

bssh_key_t modifier_key(const QString &label, quint8 modifier, int span = 2) {
	bssh_key_t key;
	key.kind = bssh_key_t::MODIFIER;
	key.label = label;
	key.modifier = modifier;
	key.span = span;
	return key;
}

/*
 * The bottom row, which every group carries unchanged.
 *
 * Repeated on purpose rather than drawn once outside the groups. Space,
 * Enter and the modifiers must not move under a thumb when the side
 * button is pressed: the value of that button is that it changes the part
 * you went looking for and nothing else. Building it in one function is
 * what keeps the three copies identical.
 */
QList<bssh_key_t> common_bottom_row() {
	QList<bssh_key_t> row;
	row.append(modifier_key(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL, 3));
	row.append(modifier_key(QStringLiteral("Alt"), bssh_emulator::MOD_ALT, 3));
	row.append(named_key(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE, 3));
	row.append(text_key(QStringLiteral("space"), U' ', 5));
	row.append(named_key(QStringLiteral("Tab"), bssh_emulator::KEY_TAB, 3));
	row.append(named_key(QStringLiteral("Enter"), bssh_emulator::KEY_ENTER, 3));
	return row;
}

QList<bssh_key_t> literal_row(const char *characters) {
	QList<bssh_key_t> row;
	for (const char *c = characters; *c; ++c) row.append(literal(*c));
	return row;
}

/* ---- the full-size keyboard (sec 6.2) -------------------------------
 *
 * A real keyboard's geometry, in QUARTER-key columns: 1u is 4, so 1.25u
 * is 5, 1.5u is 6, 1.75u is 7, 2u is 8, 2.25u is 9, 2.75u is 11 and the
 * spacebar's 6.25u is 25. Every row is 60 -- the 15u of an ANSI main
 * block -- and the catalog asserts it, because a row that does not add
 * up puts every key after the mistake in the wrong place.
 *
 * The proportions are the requirement, not decoration. A keyboard whose
 * Tab is a whole key wide, or whose Shift is the size of a letter, is one
 * that has to be read rather than touched, and the point of drawing a
 * real keyboard is that hands already know where its keys are.
 */


const int BSSH_FULL_UNIT = 4;                /* one key, in quarter columns */
const int BSSH_FULL_COLUMNS = 15 * BSSH_FULL_UNIT;

bssh_key_t spacer(int span) {
	bssh_key_t key;
	key.kind = bssh_key_t::SPACER;
	key.span = span;
	return key;
}

bssh_key_t full_text(const QString &label, char32_t ch, int span = 4) {
	return text_key(label, ch, span);
}

bssh_key_t full_literal(char ch, int span = 4) {
	bssh_key_t key = text_key(QString(QLatin1Char(ch)), char32_t(ch), span);
	key.shifted_value = int(shifted_twin(char32_t(ch)));
	return key;
}

QList<bssh_key_t> full_literal_run(const char *characters) {
	QList<bssh_key_t> row;
	for (const char *c = characters; *c; ++c) row.append(full_literal(*c));
	return row;
}

void append_all(QList<bssh_key_t> &row, const QList<bssh_key_t> &more) {
	for (const bssh_key_t &key : more) row.append(key);
}

/*
 * Page one: the main block, which is the whole of a keyboard for anyone
 * who is not entering numbers. Six rows, exactly as a 104-key board has
 * above its navigation cluster.
 */
/*
 * The bottom row, in ANSI's own widths: 1.25u for every modifier and
 * 6.25u for the wide key in the middle. Super and Menu are spacers --
 * libvterm has no encoding for either, so a button would do nothing.
 *
 * Built once for both pages, for the reason the compact keyboard's own
 * bottom row gives: Ctrl, Alt and space must not move under a thumb when
 * the page button is pressed. The wide key differs because the pages do
 * -- space where you are typing, Esc where you are navigating -- and
 * that is the one deliberate difference between them.
 */
QList<bssh_key_t> full_bottom_row(bool keypad_page) {
	const int mod = 5;   /* 1.25u */
	QList<bssh_key_t> row;
	row.append(modifier_key(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL, mod));
	row.append(spacer(mod));
	row.append(modifier_key(QStringLiteral("Alt"), bssh_emulator::MOD_ALT, mod));
	if (keypad_page) {
		row.append(named_key(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE, 25));
	} else {
		row.append(text_key(QStringLiteral("space"), U' ', 25));
	}
	row.append(modifier_key(QStringLiteral("Alt"), bssh_emulator::MOD_ALT, mod));
	row.append(spacer(mod));
	row.append(spacer(mod));
	row.append(modifier_key(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL, mod));
	return row;
}

bssh_soft_key_group_t full_main_group() {
	bssh_soft_key_group_t group;
	group.id = QStringLiteral("full-main");
	group.button_label = QStringLiteral("123");
	group.display_name = QStringLiteral("Main block");
	group.columns = BSSH_FULL_COLUMNS;

	/*
	 * The function row, with the gaps a real board has between its banks
	 * -- F4 to F5 and F8 to F9. They are spacers rather than nothing,
	 * because the eye finds F7 by counting from a gap and not from the
	 * left edge.
	 */
	QList<bssh_key_t> fn;
	fn.append(named_key(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE, 4));
	fn.append(spacer(2));
	for (int i = 1; i <= 12; ++i) {
		fn.append(named_key(QStringLiteral("F%1").arg(i),
		                     bssh_emulator::KEY_FUNCTION_BASE + i, 4));
		if (i == 4 || i == 8) fn.append(spacer(1));
	}
	fn.append(spacer(4));
	group.rows.append(fn);

	QList<bssh_key_t> digits;
	digits.append(full_literal('`'));
	append_all(digits, full_literal_run("1234567890"));
	digits.append(full_literal('-'));
	digits.append(full_literal('='));
	digits.append(named_key(QStringLiteral("Bksp"), bssh_emulator::KEY_BACKSPACE, 8));
	group.rows.append(digits);

	QList<bssh_key_t> tab;
	tab.append(named_key(QStringLiteral("Tab"), bssh_emulator::KEY_TAB, 6));
	append_all(tab, full_literal_run("qwertyuiop"));
	tab.append(full_literal('['));
	tab.append(full_literal(']'));
	tab.append(full_literal('\\', 6));
	group.rows.append(tab);

	/*
	 * Caps Lock keeps its 1.75u and sends nothing: it is a state a
	 * terminal has no notion of, and the shift this keyboard needs is the
	 * sticky modifier on the row below. Reserved rather than drawn, for
	 * the reason SPACER carries.
	 */
	QList<bssh_key_t> home;
	home.append(spacer(7));
	append_all(home, full_literal_run("asdfghjkl"));
	home.append(full_literal(';'));
	home.append(full_literal('\''));
	home.append(named_key(QStringLiteral("Enter"), bssh_emulator::KEY_ENTER, 9));
	group.rows.append(home);

	QList<bssh_key_t> shift;
	shift.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 9));
	append_all(shift, full_literal_run("zxcvbnm"));
	shift.append(full_literal(','));
	shift.append(full_literal('.'));
	shift.append(full_literal('/'));
	shift.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 11));
	group.rows.append(shift);

	group.rows.append(full_bottom_row(false));

	return group;
}

/*
 * Page two: everything to the right of the main block on a real board --
 * the navigation cluster, the arrows and the numeric keypad.
 *
 * A second page rather than a wider first one, and this is the trade the
 * holder named: a phone cannot show 22u of keyboard at a width a finger
 * can hit, so the clusters that a terminal user reaches for least often
 * move one tap away while the main block keeps a real board's shape.
 *
 * Their columns are the SAME 60, so a key here is exactly as wide as a
 * letter on page one. Giving this page a narrower grid would have made
 * its keys bigger than the letters', which is the one thing a keyboard
 * shaped like a keyboard must not do.
 */
bssh_soft_key_group_t full_keypad_group() {
	bssh_soft_key_group_t group;
	group.id = QStringLiteral("full-keypad");
	group.button_label = QStringLiteral("abc");
	group.display_name = QStringLiteral("Navigation and keypad");
	group.columns = BSSH_FULL_COLUMNS;

	/*
	 * The geometry, named once and then built from, because the first
	 * version of this function had its widths typed in and one row came
	 * to 52 instead of 60 -- which the catalog's shape test caught and a
	 * reader never would.
	 *
	 * A real board puts the navigation cluster three keys wide, a gap,
	 * then the keypad four keys wide. Those are the numbers; the rest of
	 * the row is the space to the right of the keypad.
	 */
	const int nav = 3 * BSSH_FULL_UNIT;
	const int pad = 4 * BSSH_FULL_UNIT;
	const int gap = 5 * BSSH_FULL_UNIT;
	const int trailing = BSSH_FULL_COLUMNS - nav - gap - pad;

	/* PrtSc, ScrLk, Pause and NumLock: states and host keys, not wire
	 * keys, so they hold their places and send nothing. */
	QList<bssh_key_t> top;
	top.append(spacer(nav));
	top.append(spacer(gap));
	top.append(spacer(BSSH_FULL_UNIT));
	top.append(full_literal('/'));
	top.append(full_literal('*'));
	top.append(full_literal('-'));
	top.append(spacer(trailing));
	group.rows.append(top);

	QList<bssh_key_t> r1;
	r1.append(named_key(QStringLiteral("Ins"), bssh_emulator::KEY_INSERT, BSSH_FULL_UNIT));
	r1.append(named_key(QStringLiteral("Home"), bssh_emulator::KEY_HOME, BSSH_FULL_UNIT));
	r1.append(named_key(QStringLiteral("PgUp"), bssh_emulator::KEY_PAGE_UP, BSSH_FULL_UNIT));
	r1.append(spacer(gap));
	r1.append(full_literal('7'));
	r1.append(full_literal('8'));
	r1.append(full_literal('9'));
	r1.append(full_literal('+'));
	r1.append(spacer(trailing));
	group.rows.append(r1);

	QList<bssh_key_t> r2;
	r2.append(named_key(QStringLiteral("Del"), bssh_emulator::KEY_DELETE, BSSH_FULL_UNIT));
	r2.append(named_key(QStringLiteral("End"), bssh_emulator::KEY_END, BSSH_FULL_UNIT));
	r2.append(named_key(QStringLiteral("PgDn"), bssh_emulator::KEY_PAGE_DOWN, BSSH_FULL_UNIT));
	r2.append(spacer(gap));
	r2.append(full_literal('4'));
	r2.append(full_literal('5'));
	r2.append(full_literal('6'));
	r2.append(spacer(BSSH_FULL_UNIT));
	r2.append(spacer(trailing));
	group.rows.append(r2);

	QList<bssh_key_t> r3;
	r3.append(spacer(BSSH_FULL_UNIT));
	r3.append(named_key(QStringLiteral("Up"), bssh_emulator::KEY_UP, BSSH_FULL_UNIT));
	r3.append(spacer(BSSH_FULL_UNIT));
	r3.append(spacer(gap));
	r3.append(full_literal('1'));
	r3.append(full_literal('2'));
	r3.append(full_literal('3'));
	r3.append(named_key(QStringLiteral("Ent"), bssh_emulator::KEY_ENTER, BSSH_FULL_UNIT));
	r3.append(spacer(trailing));
	group.rows.append(r3);

	QList<bssh_key_t> r4;
	r4.append(named_key(QStringLiteral("Left"), bssh_emulator::KEY_LEFT, BSSH_FULL_UNIT));
	r4.append(named_key(QStringLiteral("Down"), bssh_emulator::KEY_DOWN, BSSH_FULL_UNIT));
	r4.append(named_key(QStringLiteral("Right"), bssh_emulator::KEY_RIGHT, BSSH_FULL_UNIT));
	r4.append(spacer(gap));
	r4.append(full_text(QStringLiteral("0"), U'0', 2 * BSSH_FULL_UNIT));
	r4.append(full_literal('.'));
	r4.append(spacer(BSSH_FULL_UNIT));
	r4.append(spacer(trailing));
	group.rows.append(r4);

	/*
	 * The row that keeps the two pages the same height, and it is not
	 * padding: Ctrl, Alt and Esc are what a terminal user reaches for
	 * while they are over here, and sending them back to page one for
	 * Ctrl-C would make this page useless.
	 */
	group.rows.append(full_bottom_row(true));

	return group;
}

bssh_soft_key_group_t letters_group() {
	bssh_soft_key_group_t group;
	group.id = QStringLiteral("letters");
	group.button_label = QStringLiteral("abc");
	group.display_name = QStringLiteral("Letters");

	group.rows.append(literal_row("qwertyuiop"));

	/*
	 * Ten, not the nine a physical keyboard has: the semicolon takes the
	 * place the half-key indent would occupy. A grid cannot offset a row
	 * by half a key without a spacer that is not a key, and a terminal
	 * wants a semicolon far more than it wants the indent -- it separates
	 * two commands on one line.
	 */
	group.rows.append(literal_row("asdfghjkl;"));

	QList<bssh_key_t> row;
	row.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 3));
	row.append(literal_row("zxcvbnm"));
	row.append(named_key(QStringLiteral("Bksp"), bssh_emulator::KEY_BACKSPACE, 3));
	group.rows.append(row);

	group.rows.append(common_bottom_row());
	return group;
}

/*
 * Digits, and the punctuation a command line is written in.
 *
 * The digits run 1 to 0 across the top, where a keyboard puts them and
 * where the letters they replace already are, rather than as a 3x4
 * telephone numpad. Somebody switching groups to type a port number is
 * looking at the same ten positions their thumb just used; a numpad block
 * would be better for entering long numbers and worse for everything else
 * a terminal does.
 */
bssh_soft_key_group_t numbers_group() {
	bssh_soft_key_group_t group;
	group.id = QStringLiteral("numbers");
	group.button_label = QStringLiteral("123");
	group.display_name = QStringLiteral("Numbers and symbols");

	group.rows.append(literal_row("1234567890"));
	/*
	 * A comma rather than a second semicolon: `;` is already on the
	 * letters group's home row, and this row has exactly ten places. The
	 * comma has nowhere else on the keyboard to be, so spending one of
	 * them on a character that is one group away would leave brace
	 * expansion and every comma-separated list untypeable.
	 */
	group.rows.append(literal_row("-/:,()$&@\""));

	/*
	 * The pipe, the tilde, the backtick and the star. Sec 6.2 names
	 * exactly these as characters a system keyboard buries two layers
	 * down, and gives that as a reason the key row exists -- so a
	 * keyboard of our own that buried them again would have missed the
	 * point of being written.
	 */
	QList<bssh_key_t> row;
	row.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 3));
	row.append(literal_row(".?!'|~*"));
	row.append(named_key(QStringLiteral("Bksp"), bssh_emulator::KEY_BACKSPACE, 3));
	group.rows.append(row);

	group.rows.append(common_bottom_row());
	return group;
}

/*
 * The keys a terminal needs and no system keyboard has.
 *
 * This is the key row's content (sec 6.2) with room to breathe. Both
 * surfaces exist for the same absence, so a user who learnt the row and
 * then switches the built-in keyboard on must not find the arrows gone.
 * The arrow glyphs are the row's own labels, for the same reason.
 */
bssh_soft_key_group_t terminal_group() {
	bssh_soft_key_group_t group;
	group.id = QStringLiteral("terminal");
	group.button_label = QStringLiteral("Fn");
	group.display_name = QStringLiteral("Terminal and function keys");

	QList<bssh_key_t> row;
	for (int n = 1; n <= 10; ++n) {
		row.append(named_key(QStringLiteral("F%1").arg(n),
		                      bssh_emulator::KEY_FUNCTION_BASE + n));
	}
	group.rows.append(row);

	/*
	 * Three-letter labels rather than "PageUp". The key row was drawn
	 * with elided labels for a long time -- Ctrl read as "Ctr" -- and
	 * nobody could tell a cramped row from a broken one (sec 6.2). A
	 * keyboard cannot scroll sideways out of that, so the labels are
	 * chosen to fit instead.
	 */
	row.clear();
	row.append(named_key(QStringLiteral("F11"), bssh_emulator::KEY_FUNCTION_BASE + 11));
	row.append(named_key(QStringLiteral("F12"), bssh_emulator::KEY_FUNCTION_BASE + 12));
	row.append(named_key(QStringLiteral("Ins"), bssh_emulator::KEY_INSERT));
	row.append(named_key(QStringLiteral("Del"), bssh_emulator::KEY_DELETE));
	row.append(named_key(QStringLiteral("Hom"), bssh_emulator::KEY_HOME));
	row.append(named_key(QStringLiteral("End"), bssh_emulator::KEY_END));
	row.append(named_key(QStringLiteral("PgU"), bssh_emulator::KEY_PAGE_UP));
	row.append(named_key(QStringLiteral("PgD"), bssh_emulator::KEY_PAGE_DOWN));
	row.append(named_key(QStringLiteral("↑"), bssh_emulator::KEY_UP));
	row.append(named_key(QStringLiteral("↓"), bssh_emulator::KEY_DOWN));
	group.rows.append(row);

	row.clear();
	row.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 3));
	row.append(named_key(QStringLiteral("←"), bssh_emulator::KEY_LEFT));
	row.append(named_key(QStringLiteral("→"), bssh_emulator::KEY_RIGHT));
	row.append(literal('-'));
	row.append(literal('/'));
	row.append(literal('|'));
	row.append(literal('~'));
	row.append(literal('`'));
	row.append(named_key(QStringLiteral("Bksp"), bssh_emulator::KEY_BACKSPACE, 3));
	group.rows.append(row);

	group.rows.append(common_bottom_row());
	return group;
}

} /* namespace */

bssh_soft_keyboard_catalog::bssh_soft_keyboard_catalog() {
	m_groups.append(letters_group());
	m_groups.append(numbers_group());
	m_groups.append(terminal_group());

	m_full.append(full_main_group());
	m_full.append(full_keypad_group());
}

bssh_soft_keyboard_catalog &bssh_soft_keyboard_catalog::instance() {
	static bssh_soft_keyboard_catalog catalog;
	return catalog;
}

int bssh_soft_keyboard_catalog::next_index(int index, style_t style) const {
	const QList<bssh_soft_key_group_t> &groups = all(style);
	if (groups.isEmpty()) return 0;
	return (index + 1) % groups.size();
}

int bssh_soft_keyboard_catalog::index_of(const QString &id, style_t style) const {
	const QList<bssh_soft_key_group_t> &groups = all(style);
	for (int i = 0; i < groups.size(); ++i) {
		if (groups.at(i).id == id) return i;
	}
	return 0;
}

bssh_soft_keyboard_catalog::style_t bssh_keyboard_style_for(
        bssh_soft_keyboard_catalog::style_t wanted, int width_dp) {
	if (wanted != bssh_soft_keyboard_catalog::STYLE_FULL) return wanted;
	if (width_dp < BSSH_FULL_KEYBOARD_MIN_DP) {
		return bssh_soft_keyboard_catalog::STYLE_COMPACT;
	}
	return wanted;
}

QChar bssh_shifted_char(char32_t plain, char32_t twin, bool shifted) {
	if (!shifted) return QChar(uint(plain));
	if (twin != 0) return QChar(uint(twin));
	return QChar(uint(plain)).toUpper();
}
