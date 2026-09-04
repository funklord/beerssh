#include "ui/soft_keyboard_layout.h"

#include "term/emulator.h"
#include "input/input_router.h"

namespace {

/*
 * ---- slots and legends (sec 6.2) -----------------------------------
 *
 * A keyboard is two things that were one thing in this file until now: a
 * GEOMETRY, which keys exist and how wide they are, and a LAYOUT, which
 * says what each of those positions sends. A call reading
 * full_literal_run("qwertyuiop") declared both at once -- so a second
 * layout could not have been added without forking the geometry it was
 * fused to, and the first thing anyone would want after AltGr is their
 * own letters.
 *
 * Positions are named the way XKB names them, because that is the
 * vocabulary every keyboard layout in the world is already written in:
 * AE01 is the leftmost key of the digit row, AD01 of the QWERTY row,
 * AC01 of the home row, AB01 of the bottom row; TLDE is the key left of
 * AE01 and BKSL the one above Enter. A Swedish layout is then a second
 * table of these same 47 rows and no change to any geometry at all --
 * and it can be transcribed from that layout's own xkb definition rather
 * than recalled, which is how this one was made.
 */
struct bssh_layout_slot_t {
	const char *slot;

	/*
	 * The four levels in UTF-8 -- plain, Shift, AltGr, Shift+AltGr --
	 * and "" where the slot has none.
	 *
	 * Level 2 is empty on every letter, deliberately. Shift on a letter
	 * is a CASE MAPPING and not a second legend, so it is computed
	 * (bssh_shifted_char falls back to toUpper) rather than listed; a
	 * layout in another script then gets its capitals right without
	 * spelling out twenty-six of them, and a layout that genuinely
	 * prints something else on the top half of a letter key can still
	 * say so here.
	 */
	const char *level[4];
};

/*
 * US, with International's third and fourth levels.
 *
 * Generated from this system's own /usr/share/X11/xkb/symbols/us --
 * levels 1 and 2 from variant "basic", levels 3 and 4 from variant
 * "intl" -- rather than written from memory, because a table of accented
 * characters is exactly the kind of thing that is plausible and wrong
 * when recalled and cannot be caught by re-reading it.
 *
 * Deriving it also CHECKED the two hand-written tables it replaces: the
 * generated levels 3 and 4 reproduce all 38 rows of the old AltGr table
 * and the generated level 2 all 21 of the old shifted-twin switch,
 * exactly. Two transcriptions agreeing is worth something only when the
 * second did not come from the first (evidence.md), and this one came
 * from xkb.
 *
 * ONE DELIBERATE DIVERGENCE from us(intl). It also makes the quote keys
 * DEAD at levels 1 and 2 -- AC11 is dead_acute and dead_diaeresis -- so
 * that \' then a gives \u00e1. In a shell that is hostile: \' and " are
 * quoting characters typed constantly, and every one of them would need
 * a trailing space. Levels 1 and 2 therefore come from us(basic) and
 * only the AltGr levels from intl, which is what makes this usable in a
 * terminal and is why the holder chose AltGr over dead keys. Where intl
 * merely repeats at level 3 what basic has at level 1 -- the consequence
 * of that same deadening, on `, \', g, h, v and b -- the level is left
 * empty rather than carrying a character AltGr would send twice.
 */
const bssh_layout_slot_t BSSH_LAYOUT_US_INTL[] = {
	{ "TLDE", { "`", "~", "", "" } },
	{ "AE01", { "1", "!", "\u00a1", "\u00b9" } },
	{ "AE02", { "2", "@", "\u00b2", "" } },
	{ "AE03", { "3", "#", "\u00b3", "" } },
	{ "AE04", { "4", "$", "\u00a4", "\u00a3" } },
	{ "AE05", { "5", "%", "\u20ac", "" } },
	{ "AE06", { "6", "^", "\u00bc", "^" } },
	{ "AE07", { "7", "&", "\u00bd", "" } },
	{ "AE08", { "8", "*", "\u00be", "" } },
	{ "AE09", { "9", "(", "\u2018", "" } },
	{ "AE10", { "0", ")", "\u2019", "" } },
	{ "AE11", { "-", "_", "\u00a5", "" } },
	{ "AE12", { "=", "+", "\u00d7", "\u00f7" } },
	{ "AD01", { "q", "", "\u00e4", "\u00c4" } },
	{ "AD02", { "w", "", "\u00e5", "\u00c5" } },
	{ "AD03", { "e", "", "\u00e9", "\u00c9" } },
	{ "AD04", { "r", "", "\u00ae", "\u2122" } },
	{ "AD05", { "t", "", "\u00fe", "\u00de" } },
	{ "AD06", { "y", "", "\u00fc", "\u00dc" } },
	{ "AD07", { "u", "", "\u00fa", "\u00da" } },
	{ "AD08", { "i", "", "\u00ed", "\u00cd" } },
	{ "AD09", { "o", "", "\u00f3", "\u00d3" } },
	{ "AD10", { "p", "", "\u00f6", "\u00d6" } },
	{ "AD11", { "[", "{", "\u00ab", "\u201c" } },
	{ "AD12", { "]", "}", "\u00bb", "\u201d" } },
	{ "BKSL", { "\\", "|", "\u00ac", "" } },
	{ "AC01", { "a", "", "\u00e1", "\u00c1" } },
	{ "AC02", { "s", "", "\u00df", "\u00a7" } },
	{ "AC03", { "d", "", "\u00f0", "\u00d0" } },
	{ "AC04", { "f", "", "\u00eb", "\u00cb" } },
	{ "AC05", { "g", "", "", "" } },
	{ "AC06", { "h", "", "", "" } },
	{ "AC07", { "j", "", "\u00ef", "\u00cf" } },
	{ "AC08", { "k", "", "\u0153", "\u0152" } },
	{ "AC09", { "l", "", "\u00f8", "\u00d8" } },
	{ "AC10", { ";", ":", "\u00b6", "\u00b0" } },
	{ "AC11", { "'", "\"", "", "" } },
	{ "AB01", { "z", "", "\u00e6", "\u00c6" } },
	{ "AB02", { "x", "", "\u00b7", "" } },
	{ "AB03", { "c", "", "\u00a9", "\u00a2" } },
	{ "AB04", { "v", "", "", "" } },
	{ "AB05", { "b", "", "", "" } },
	{ "AB06", { "n", "", "\u00f1", "\u00d1" } },
	{ "AB07", { "m", "", "\u00b5", "\u00b1" } },
	{ "AB08", { ",", "<", "", "" } },
	{ "AB09", { ".", ">", "", "" } },
	{ "AB10", { "/", "?", "", "" } },
};

/* The slot with this id, or nullptr. */
const bssh_layout_slot_t *layout_slot(const char *id) {
	for (const bssh_layout_slot_t &entry : BSSH_LAYOUT_US_INTL) {
		if (qstrcmp(entry.slot, id) == 0) return &entry;
	}
	return nullptr;
}

/*
 * The slot whose PLAIN level is this character, or nullptr.
 *
 * The symbol pages of the compact keyboard are not a layout: they are a
 * palette, arranged for a thumb rather than in a keyboard's positions,
 * so they name characters and not slots. They still want the right
 * second and third legends, and this is how they get them from the same
 * one table -- a character that appears at no slot's level 1, such as
 * the ? on the compact symbol page, simply has none, which is what it
 * had before.
 */
const bssh_layout_slot_t *layout_slot_for(char32_t plain) {
	for (const bssh_layout_slot_t &entry : BSSH_LAYOUT_US_INTL) {
		const QString level = QString::fromUtf8(entry.level[0]);
		if (!level.isEmpty() && char32_t(level.at(0).unicode()) == plain) return &entry;
	}
	return nullptr;
}

/* Fills levels 2 to 4 from a slot. Level 1 is already the key's value. */
void apply_levels(bssh_key_t &key, const bssh_layout_slot_t *entry) {
	if (!entry) return;

	int *fields[3] = { &key.shifted_value, &key.altgr_value, &key.shift_altgr_value };
	for (int level = 1; level < 4; ++level) {
		const QString text = QString::fromUtf8(entry->level[level]);
		if (!text.isEmpty()) *fields[level - 1] = int(text.at(0).unicode());
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

/* A character whose label IS what it sends, and its other legends. */
bssh_key_t literal(char ch, int span = 2) {
	bssh_key_t key = text_key(QString(QLatin1Char(ch)), char32_t(ch), span);
	apply_levels(key, layout_slot_for(char32_t(ch)));
	return key;
}

/*
 * The key at a layout POSITION: geometry names the slot and the width,
 * the layout supplies every character.
 *
 * An unknown slot produces a key that sends nothing and is labelled with
 * the id that was not found, so a mistyped position is visible on the
 * keyboard and catchable by a test asserting no key sends zero -- rather
 * than silently drawing a blank where a letter should be.
 */
bssh_key_t slot_key(const char *id, int span) {
	const bssh_layout_slot_t *entry = layout_slot(id);
	if (!entry) return text_key(QString::fromUtf8(id), 0, span);

	const QString plain = QString::fromUtf8(entry->level[0]);
	bssh_key_t key = text_key(plain, char32_t(plain.at(0).unicode()), span);
	apply_levels(key, entry);
	return key;
}

/*
 * A run of consecutive slots across one row: slot_run("AD", 1, 10) is
 * AD01 to AD10, which on this layout is q to p.
 *
 * The ids are DERIVED from the row and the count rather than listed,
 * because a list of twelve near-identical strings is a place to make a
 * typo that reads as correct. It also says the thing the geometry
 * actually means -- ten keys along the QWERTY row -- instead of naming
 * the letters that happen to be on them today.
 */
QList<bssh_key_t> slot_run(const char *row, int first, int count, int span) {
	QList<bssh_key_t> keys;
	for (int i = 0; i < count; ++i) {
		const QByteArray id = QByteArray(row)
		        + QByteArray::number(first + i).rightJustified(2, '0');
		keys.append(slot_key(id.constData(), span));
	}
	return keys;
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
	return literal(ch, span);
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
	/*
	 * The RIGHT Alt is AltGr, which is what it is on every keyboard that
	 * has one -- no new key, no change to the row's arithmetic, and the
	 * position a hand already reaches for. Left Alt stays Alt, so Meta
	 * still works, which is the same trade a physical European board
	 * makes.
	 */
	row.append(modifier_key(QStringLiteral("AltGr"), bssh_input_router::MOD_ALTGR, mod));
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
	digits.append(slot_key("TLDE", BSSH_FULL_UNIT));
	append_all(digits, slot_run("AE", 1, 12, BSSH_FULL_UNIT));
	digits.append(named_key(QStringLiteral("Bksp"), bssh_emulator::KEY_BACKSPACE, 8));
	group.rows.append(digits);

	QList<bssh_key_t> tab;
	tab.append(named_key(QStringLiteral("Tab"), bssh_emulator::KEY_TAB, 6));
	append_all(tab, slot_run("AD", 1, 12, BSSH_FULL_UNIT));
	tab.append(slot_key("BKSL", 6));
	group.rows.append(tab);

	/*
	 * Caps Lock keeps its 1.75u and sends nothing: it is a state a
	 * terminal has no notion of, and the shift this keyboard needs is the
	 * sticky modifier on the row below. Reserved rather than drawn, for
	 * the reason SPACER carries.
	 */
	QList<bssh_key_t> home;
	home.append(spacer(7));
	append_all(home, slot_run("AC", 1, 11, BSSH_FULL_UNIT));
	home.append(named_key(QStringLiteral("Enter"), bssh_emulator::KEY_ENTER, 9));
	group.rows.append(home);

	QList<bssh_key_t> shift;
	shift.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 9));
	append_all(shift, slot_run("AB", 1, 10, BSSH_FULL_UNIT));
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
/*
 * A key on the numeric keypad: the character it shows, and nothing else.
 *
 * NOT full_literal(), which is what these were, and which consults the
 * layout. That gave every keypad key the legends belonging to the digit
 * ROW -- Shift on the keypad's 4 sent $, AltGr on its 5 sent a euro
 * sign, Shift on its . sent > -- because the layout is keyed by
 * character and a 4 is a 4 wherever it happens to be drawn.
 *
 * A real keypad does none of that: with NumLock on it sends its digit
 * and nothing else. That is also why the keypad gets no slot -- KP7 is
 * KP7 in every language, and a national layout moves the letters above
 * it rather than the digits on it.
 *
 * The fault was visible from inside the group and nobody was looking:
 * the double-width 0 was built with a different helper and was therefore
 * the ONE key here that behaved correctly. What found it was a test
 * comparing the two keyboards, which collapsed that 0 with the digit
 * row's and failed on the disagreement -- a fault surfacing through a
 * test written for something else, which is the argument for reading a
 * failure before assuming the test is at fault.
 */
bssh_key_t keypad_key(const QString &label, char32_t ch, int span = BSSH_FULL_UNIT) {
	return text_key(label, ch, span);
}

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
	top.append(keypad_key(QStringLiteral("/"), U'/'));
	top.append(keypad_key(QStringLiteral("*"), U'*'));
	top.append(keypad_key(QStringLiteral("-"), U'-'));
	top.append(spacer(trailing));
	group.rows.append(top);

	QList<bssh_key_t> r1;
	r1.append(named_key(QStringLiteral("Ins"), bssh_emulator::KEY_INSERT, BSSH_FULL_UNIT));
	r1.append(named_key(QStringLiteral("Home"), bssh_emulator::KEY_HOME, BSSH_FULL_UNIT));
	r1.append(named_key(QStringLiteral("PgUp"), bssh_emulator::KEY_PAGE_UP, BSSH_FULL_UNIT));
	r1.append(spacer(gap));
	r1.append(keypad_key(QStringLiteral("7"), U'7'));
	r1.append(keypad_key(QStringLiteral("8"), U'8'));
	r1.append(keypad_key(QStringLiteral("9"), U'9'));
	r1.append(keypad_key(QStringLiteral("+"), U'+'));
	r1.append(spacer(trailing));
	group.rows.append(r1);

	QList<bssh_key_t> r2;
	r2.append(named_key(QStringLiteral("Del"), bssh_emulator::KEY_DELETE, BSSH_FULL_UNIT));
	r2.append(named_key(QStringLiteral("End"), bssh_emulator::KEY_END, BSSH_FULL_UNIT));
	r2.append(named_key(QStringLiteral("PgDn"), bssh_emulator::KEY_PAGE_DOWN, BSSH_FULL_UNIT));
	r2.append(spacer(gap));
	r2.append(keypad_key(QStringLiteral("4"), U'4'));
	r2.append(keypad_key(QStringLiteral("5"), U'5'));
	r2.append(keypad_key(QStringLiteral("6"), U'6'));
	r2.append(spacer(BSSH_FULL_UNIT));
	r2.append(spacer(trailing));
	group.rows.append(r2);

	QList<bssh_key_t> r3;
	r3.append(spacer(BSSH_FULL_UNIT));
	r3.append(named_key(QStringLiteral("Up"), bssh_emulator::KEY_UP, BSSH_FULL_UNIT));
	r3.append(spacer(BSSH_FULL_UNIT));
	r3.append(spacer(gap));
	r3.append(keypad_key(QStringLiteral("1"), U'1'));
	r3.append(keypad_key(QStringLiteral("2"), U'2'));
	r3.append(keypad_key(QStringLiteral("3"), U'3'));
	r3.append(named_key(QStringLiteral("Ent"), bssh_emulator::KEY_ENTER, BSSH_FULL_UNIT));
	r3.append(spacer(trailing));
	group.rows.append(r3);

	QList<bssh_key_t> r4;
	r4.append(named_key(QStringLiteral("Left"), bssh_emulator::KEY_LEFT, BSSH_FULL_UNIT));
	r4.append(named_key(QStringLiteral("Down"), bssh_emulator::KEY_DOWN, BSSH_FULL_UNIT));
	r4.append(named_key(QStringLiteral("Right"), bssh_emulator::KEY_RIGHT, BSSH_FULL_UNIT));
	r4.append(spacer(gap));
	r4.append(keypad_key(QStringLiteral("0"), U'0', 2 * BSSH_FULL_UNIT));
	r4.append(keypad_key(QStringLiteral("."), U'.'));
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

	group.rows.append(slot_run("AD", 1, 10, 2));

	/*
	 * Ten, not the nine a physical keyboard has: the semicolon takes the
	 * place the half-key indent would occupy. A grid cannot offset a row
	 * by half a key without a spacer that is not a key, and a terminal
	 * wants a semicolon far more than it wants the indent -- it separates
	 * two commands on one line.
	 */
	group.rows.append(slot_run("AC", 1, 10, 2));

	QList<bssh_key_t> row;
	row.append(modifier_key(QStringLiteral("Shift"), bssh_emulator::MOD_SHIFT, 3));
	row.append(slot_run("AB", 1, 7, 2));
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

	group.rows.append(slot_run("AE", 1, 10, 2));
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

QChar bssh_keyboard_char(const bssh_key_t &key, bool shifted, bool altgr) {
	const char32_t plain = char32_t(key.value);

	if (altgr) {
		if (shifted && key.shift_altgr_value != 0) {
			return QChar(uint(key.shift_altgr_value));
		}
		if (key.altgr_value != 0) return QChar(uint(key.altgr_value));
		/* No third legend: AltGr does nothing, which is a real
		 * keyboard's behaviour too. Fall through to Shift. */
	}
	return bssh_shifted_char(plain, char32_t(key.shifted_value), shifted);
}
