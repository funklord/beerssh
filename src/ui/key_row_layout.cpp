#include "ui/key_row_layout.h"

#include "term/emulator.h"

namespace {

bssh_key_t named(const QString &label, int key) {
	bssh_key_t entry;
	entry.label = label;
	entry.kind = bssh_key_t::KEY;
	entry.value = key;
	return entry;
}

bssh_key_t modifier(const QString &label, quint8 mask) {
	bssh_key_t entry;
	entry.label = label;
	entry.kind = bssh_key_t::MODIFIER;
	entry.modifier = mask;
	return entry;
}

bssh_key_t text(const QString &label, char32_t ch) {
	bssh_key_t entry;
	entry.label = label;
	entry.kind = bssh_key_t::TEXT;
	entry.value = int(ch);
	return entry;
}

bssh_key_t chord(const QString &label, char32_t ch, quint8 mask) {
	bssh_key_t entry;
	entry.label = label;
	entry.kind = bssh_key_t::CHORD;
	entry.value = int(ch);
	entry.modifier = mask;
	return entry;
}

/* The arrows, which every layout wants and none wants to spell out. */
bssh_key_t function_toggle() {
	bssh_key_t key;
	key.label = QStringLiteral("Fn");
	key.kind = bssh_key_t::FUNCTION_TOGGLE;
	return key;
}

QList<bssh_key_t> arrows() {
	return {
		named(QStringLiteral("←"), bssh_emulator::KEY_LEFT),
		named(QStringLiteral("↓"), bssh_emulator::KEY_DOWN),
		named(QStringLiteral("↑"), bssh_emulator::KEY_UP),
		named(QStringLiteral("→"), bssh_emulator::KEY_RIGHT)
	};
}

} /* namespace */

QList<bssh_key_t> bssh_function_keys() {
	QList<bssh_key_t> keys;
	for (int n = 1; n <= 12; ++n) {
		keys.append(named(QStringLiteral("F%1").arg(n),
		                   bssh_emulator::KEY_FUNCTION_BASE + n));
	}
	return keys;
}

bssh_key_row_catalog &bssh_key_row_catalog::instance() {
	static bssh_key_row_catalog catalog;
	return catalog;
}

bssh_key_row_catalog::bssh_key_row_catalog() {
	/*
	 * The default: what the row held before layouts existed, so nobody's
	 * key row changed the day this landed.
	 */
	{
		bssh_key_row_layout_t layout;
		layout.id = QStringLiteral("default");
		layout.display_name = QStringLiteral("Default");
		layout.description = QStringLiteral(
		        "Esc, Tab, the modifiers, arrows, and the punctuation a soft "
		        "keyboard buries.");

		layout.keys = {
			named(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE),
			named(QStringLiteral("Tab"), bssh_emulator::KEY_TAB),
			modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL),
			modifier(QStringLiteral("Alt"), bssh_emulator::MOD_ALT)
		};
		layout.keys += arrows();
		layout.keys += QList<bssh_key_t>{
			function_toggle(),
			text(QStringLiteral("-"), U'-'),
			text(QStringLiteral("/"), U'/'),
			text(QStringLiteral("|"), U'|'),
			text(QStringLiteral("~"), U'~')
		};

		m_layouts.append(layout);
	}

	/*
	 * A Cisco-ish console is driven by `?` and Tab -- completion and
	 * context help are how anyone navigates a CLI they do not have
	 * memorised -- and almost never by Alt. `|` earns its place because
	 * every such box pipes into `include` or `begin` to make output
	 * readable.
	 */
	{
		bssh_key_row_layout_t layout;
		layout.id = QStringLiteral("network-switch");
		layout.display_name = QStringLiteral("Network switch");
		layout.description = QStringLiteral(
		        "Context help and completion first, for a console CLI: ?, Tab "
		        "and | rather than Alt and history keys.");

		layout.keys = {
			named(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE),
			named(QStringLiteral("Tab"), bssh_emulator::KEY_TAB),
			modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL),
			text(QStringLiteral("?"), U'?'),
			text(QStringLiteral("|"), U'|')
		};
		layout.keys += arrows();
		layout.keys += QList<bssh_key_t>{
			text(QStringLiteral("-"), U'-'),
			text(QStringLiteral("."), U'.'),
			text(QStringLiteral("/"), U'/')
		};

		m_layouts.append(layout);
	}

	/*
	 * The prefix key is the whole point. Ctrl-B is a chord no soft
	 * keyboard can produce, and a tmux user presses it dozens of times an
	 * hour -- as two sticky presses that is the difference between a
	 * usable phone client and one that gets abandoned. The keys beside it
	 * are what a prefix is usually followed by.
	 */
	{
		bssh_key_row_layout_t layout;
		layout.id = QStringLiteral("tmux");
		layout.display_name = QStringLiteral("tmux");
		layout.description = QStringLiteral(
		        "The Ctrl-B prefix as one key, plus what usually follows it.");

		layout.keys = {
			chord(QStringLiteral("C-b"), U'b', bssh_emulator::MOD_CTRL),
			named(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE),
			modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL),
			text(QStringLiteral("c"), U'c'),
			text(QStringLiteral("n"), U'n'),
			text(QStringLiteral("p"), U'p'),
			text(QStringLiteral("d"), U'd'),
			text(QStringLiteral("["), U'[')
		};
		layout.keys += arrows();

		m_layouts.append(layout);
	}

	/*
	 * Escape twice the size of anything else, because that is the key vi
	 * is made of, and the punctuation its command line needs. No Alt: vi
	 * does not use it and the space is worth more elsewhere.
	 */
	{
		bssh_key_row_layout_t layout;
		layout.id = QStringLiteral("vi");
		layout.display_name = QStringLiteral("vi / vim");
		layout.description = QStringLiteral(
		        "Esc first and the command-line punctuation: :, /, and the "
		        "registers.");

		layout.keys = {
			named(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE),
			modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL),
			text(QStringLiteral(":"), U':'),
			text(QStringLiteral("/"), U'/'),
			text(QStringLiteral("$"), U'$'),
			text(QStringLiteral("^"), U'^'),
			text(QStringLiteral("*"), U'*'),
			text(QStringLiteral("\""), U'"')
		};
		layout.keys += arrows();

		m_layouts.append(layout);
	}

	/*
	 * For a narrow phone, or a screen where the row is competing with the
	 * terminal for rows: the four keys a session cannot be driven without
	 * and nothing else.
	 */
	{
		bssh_key_row_layout_t layout;
		layout.id = QStringLiteral("minimal");
		layout.display_name = QStringLiteral("Minimal");
		layout.description = QStringLiteral(
		        "Four keys and the arrows, for a narrow screen.");

		layout.keys = {
			named(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE),
			named(QStringLiteral("Tab"), bssh_emulator::KEY_TAB),
			modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL),
			modifier(QStringLiteral("Alt"), bssh_emulator::MOD_ALT)
		};
		layout.keys += arrows();

		m_layouts.append(layout);
	}
}

const bssh_key_row_layout_t *bssh_key_row_catalog::by_id(const QString &id) const {
	for (const bssh_key_row_layout_t &layout : m_layouts) {
		if (layout.id == id) return &layout;
	}
	return nullptr;
}

QString bssh_key_row_catalog::effective_id(const QString &stored) const {
	if (stored.isEmpty()) return default_id();
	if (by_id(stored)) return stored;

	for (const bssh_key_row_layout_t &layout : m_layouts) {
		if (layout.display_name.compare(stored, Qt::CaseInsensitive) == 0) return layout.id;
	}

	return stored;
}

QString bssh_key_row_catalog::default_id() const {
	return QStringLiteral("default");
}
