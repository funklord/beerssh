#include "ui/soft_keyboard_layout.h"

#include "term/emulator.h"

namespace {

bssh_key_t text_key(const QString &label, char32_t ch, int span = 2) {
	bssh_key_t key;
	key.kind = bssh_key_t::TEXT;
	key.label = label;
	key.value = int(ch);
	key.span = span;
	return key;
}

/* A character whose label IS what it sends. */
bssh_key_t literal(char ch) {
	return text_key(QString(QLatin1Char(ch)), char32_t(ch));
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
}

bssh_soft_keyboard_catalog &bssh_soft_keyboard_catalog::instance() {
	static bssh_soft_keyboard_catalog catalog;
	return catalog;
}

int bssh_soft_keyboard_catalog::next_index(int index) const {
	if (m_groups.isEmpty()) return 0;
	return (index + 1) % m_groups.size();
}

int bssh_soft_keyboard_catalog::index_of(const QString &id) const {
	for (int i = 0; i < m_groups.size(); ++i) {
		if (m_groups.at(i).id == id) return i;
	}
	return 0;
}
