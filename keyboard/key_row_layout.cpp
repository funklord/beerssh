#include "keyboard/key_row_layout.h"

#include "keyboard/sk_modifiers.h"

#include <QStringList>

namespace {

sk_key_t named(const QString &label, int key) {
	sk_key_t entry;
	entry.label = label;
	entry.kind = sk_key_t::KEY;
	entry.value = key;
	return entry;
}

sk_key_t modifier(const QString &label, quint8 mask) {
	sk_key_t entry;
	entry.label = label;
	entry.kind = sk_key_t::MODIFIER;
	entry.modifier = mask;
	return entry;
}

sk_key_t text(const QString &label, char32_t ch) {
	sk_key_t entry;
	entry.label = label;
	entry.kind = sk_key_t::TEXT;
	entry.value = int(ch);
	return entry;
}

sk_key_t chord(const QString &label, char32_t ch, quint8 mask) {
	sk_key_t entry;
	entry.label = label;
	entry.kind = sk_key_t::CHORD;
	entry.value = int(ch);
	entry.modifier = mask;
	return entry;
}

/* The arrows, which every layout wants and none wants to spell out. */
sk_key_t function_toggle() {
	sk_key_t key;
	key.label = QStringLiteral("Fn");
	key.kind = sk_key_t::FUNCTION_TOGGLE;
	return key;
}

QList<sk_key_t> arrows() {
	return {
		named(QStringLiteral("←"), Qt::Key_Left),
		named(QStringLiteral("↓"), Qt::Key_Down),
		named(QStringLiteral("↑"), Qt::Key_Up),
		named(QStringLiteral("→"), Qt::Key_Right)
	};
}


/*
 * The vocabulary a custom row is written in. One table, read both ways,
 * so a spec and its parse cannot disagree about what `pgup' means -- two
 * tables would be two things to keep in step, which is the shape sec 6.4
 * records for the context menu's hardcoded chords.
 */
struct token_t {
	const char *word;
	sk_key_t::kind_t kind;
	int value;
	quint8 modifier;
	const char *label;
};

const token_t SK_KEY_WORDS[] = {
	{ "esc",   sk_key_t::KEY, Qt::Key_Escape,    0, "Esc" },
	{ "tab",   sk_key_t::KEY, Qt::Key_Tab,       0, "Tab" },
	{ "enter", sk_key_t::KEY, Qt::Key_Return,     0, "Enter" },
	{ "bksp",  sk_key_t::KEY, Qt::Key_Backspace, 0, "Bksp" },
	{ "ins",   sk_key_t::KEY, Qt::Key_Insert,    0, "Ins" },
	{ "del",   sk_key_t::KEY, Qt::Key_Delete,    0, "Del" },
	{ "home",  sk_key_t::KEY, Qt::Key_Home,      0, "Home" },
	{ "end",   sk_key_t::KEY, Qt::Key_End,       0, "End" },
	{ "pgup",  sk_key_t::KEY, Qt::Key_PageUp,   0, "PgUp" },
	{ "pgdn",  sk_key_t::KEY, Qt::Key_PageDown, 0, "PgDn" },
	/* The arrows carry the glyphs the curated rows use, so a custom row
	 * sits beside them without looking like a different widget. */
	{ "left",  sk_key_t::KEY, Qt::Key_Left,  0, "\xe2\x86\x90" },
	{ "down",  sk_key_t::KEY, Qt::Key_Down,  0, "\xe2\x86\x93" },
	{ "up",    sk_key_t::KEY, Qt::Key_Up,    0, "\xe2\x86\x91" },
	{ "right", sk_key_t::KEY, Qt::Key_Right, 0, "\xe2\x86\x92" },

	{ "ctrl",  sk_key_t::MODIFIER, 0, SK_MOD_CTRL,  "Ctrl" },
	{ "alt",   sk_key_t::MODIFIER, 0, SK_MOD_ALT,   "Alt" },
	{ "shift", sk_key_t::MODIFIER, 0, SK_MOD_SHIFT, "Shift" },

	{ "fn",    sk_key_t::FUNCTION_TOGGLE, 0, 0, "Fn" },
	{ "gap",   sk_key_t::SPACER,          0, 0, "" }
};

const token_t *word_for(const QString &word) {
	for (const token_t &entry : SK_KEY_WORDS) {
		if (word.compare(QLatin1String(entry.word), Qt::CaseInsensitive) == 0) {
			return &entry;
		}
	}
	return nullptr;
}

/* `ctrl-b' and `alt-x': the modifier's word, a hyphen, one character. */
bool parse_chord(const QString &word, sk_key_t *out) {
	const int dash = word.indexOf(QLatin1Char('-'));
	if (dash <= 0 || dash + 2 != word.size()) return false;

	const token_t *mod = word_for(word.left(dash));
	if (!mod || mod->kind != sk_key_t::MODIFIER) return false;

	const QChar ch = word.at(dash + 1);
	*out = chord(QStringLiteral("%1-%2")
	                     .arg(QString(QLatin1String(mod->word)).at(0).toUpper())
	                     .arg(ch),
	              char32_t(ch.unicode()), mod->modifier);
	return true;
}

} /* namespace */

QList<sk_key_t> sk_function_keys() {
	QList<sk_key_t> keys;
	for (int n = 1; n <= 12; ++n) {
		keys.append(named(QStringLiteral("F%1").arg(n),
		                   Qt::Key_F1 + n - 1));
	}
	return keys;
}

sk_key_row_catalog &sk_key_row_catalog::instance() {
	static sk_key_row_catalog catalog;
	return catalog;
}

sk_key_row_catalog::sk_key_row_catalog() {
	/*
	 * The default: what the row held before layouts existed, so nobody's
	 * key row changed the day this landed.
	 */
	{
		sk_key_row_layout_t layout;
		layout.id = QStringLiteral("default");
		layout.display_name = QStringLiteral("Default");
		layout.description = QStringLiteral(
		        "Esc, Tab, the modifiers, arrows, and the punctuation a soft "
		        "keyboard buries.");

		layout.keys = {
			named(QStringLiteral("Esc"), Qt::Key_Escape),
			named(QStringLiteral("Tab"), Qt::Key_Tab),
			modifier(QStringLiteral("Ctrl"), SK_MOD_CTRL),
			modifier(QStringLiteral("Alt"), SK_MOD_ALT)
		};
		layout.keys += arrows();
		layout.keys += QList<sk_key_t>{
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
		sk_key_row_layout_t layout;
		layout.id = QStringLiteral("network-switch");
		layout.display_name = QStringLiteral("Network switch");
		layout.description = QStringLiteral(
		        "Context help and completion first, for a console CLI: ?, Tab "
		        "and | rather than Alt and history keys.");

		layout.keys = {
			named(QStringLiteral("Esc"), Qt::Key_Escape),
			named(QStringLiteral("Tab"), Qt::Key_Tab),
			modifier(QStringLiteral("Ctrl"), SK_MOD_CTRL),
			text(QStringLiteral("?"), U'?'),
			text(QStringLiteral("|"), U'|')
		};
		layout.keys += arrows();
		layout.keys += QList<sk_key_t>{
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
		sk_key_row_layout_t layout;
		layout.id = QStringLiteral("tmux");
		layout.display_name = QStringLiteral("tmux");
		layout.description = QStringLiteral(
		        "The Ctrl-B prefix as one key, plus what usually follows it.");

		layout.keys = {
			chord(QStringLiteral("C-b"), U'b', SK_MOD_CTRL),
			named(QStringLiteral("Esc"), Qt::Key_Escape),
			modifier(QStringLiteral("Ctrl"), SK_MOD_CTRL),
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
		sk_key_row_layout_t layout;
		layout.id = QStringLiteral("vi");
		layout.display_name = QStringLiteral("vi / vim");
		layout.description = QStringLiteral(
		        "Esc first and the command-line punctuation: :, /, and the "
		        "registers.");

		layout.keys = {
			named(QStringLiteral("Esc"), Qt::Key_Escape),
			modifier(QStringLiteral("Ctrl"), SK_MOD_CTRL),
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
		sk_key_row_layout_t layout;
		layout.id = QStringLiteral("minimal");
		layout.display_name = QStringLiteral("Minimal");
		layout.description = QStringLiteral(
		        "Four keys and the arrows, for a narrow screen.");

		layout.keys = {
			named(QStringLiteral("Esc"), Qt::Key_Escape),
			named(QStringLiteral("Tab"), Qt::Key_Tab),
			modifier(QStringLiteral("Ctrl"), SK_MOD_CTRL),
			modifier(QStringLiteral("Alt"), SK_MOD_ALT)
		};
		layout.keys += arrows();

		m_layouts.append(layout);
	}

	/*
	 * The user's own, last so it reads as the escape hatch rather than as
	 * a sixth curated row. It starts as a copy of the default -- the same
	 * choice the colour catalogue makes -- so somebody who selects it
	 * before editing anything gets a working row instead of a blank
	 * strip, and so the contract below holds from the first moment it
	 * exists rather than from the first time it is written.
	 */
	{
		sk_key_row_layout_t layout = m_layouts.first();
		layout.id = QStringLiteral("custom");
		layout.display_name = QStringLiteral("Custom");
		layout.description = QStringLiteral(
		        "Your own row, composed in Settings and stored on this device.");
		m_layouts.append(layout);
	}
}

const sk_key_row_layout_t *sk_key_row_catalog::by_id(const QString &id) const {
	for (const sk_key_row_layout_t &layout : m_layouts) {
		if (layout.id == id) return &layout;
	}
	return nullptr;
}

QString sk_key_row_catalog::effective_id(const QString &stored) const {
	if (stored.isEmpty()) return default_id();
	if (by_id(stored)) return stored;

	for (const sk_key_row_layout_t &layout : m_layouts) {
		if (layout.display_name.compare(stored, Qt::CaseInsensitive) == 0) return layout.id;
	}

	return stored;
}

QString sk_key_row_catalog::default_id() const {
	return QStringLiteral("default");
}

bool sk_key_row_usable(const QList<sk_key_t> &keys) {
	bool has_escape = false;
	bool has_modifier = false;

	for (const sk_key_t &key : keys) {
		if (key.kind == sk_key_t::KEY && key.value == Qt::Key_Escape) {
			has_escape = true;
		}
		if (key.kind == sk_key_t::MODIFIER) has_modifier = true;
	}

	return has_escape && has_modifier;
}

QList<sk_key_t> sk_parse_key_row(const QString &spec) {
	QList<sk_key_t> keys;

	const QStringList words = spec.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (const QString &word : words) {
		if (const token_t *entry = word_for(word)) {
			sk_key_t key;
			key.label = QString::fromUtf8(entry->label);
			key.kind = entry->kind;
			key.value = entry->value;
			key.modifier = entry->modifier;
			keys.append(key);
			continue;
		}

		sk_key_t as_chord;
		if (parse_chord(word, &as_chord)) {
			keys.append(as_chord);
			continue;
		}

		/*
		 * One character is itself. Longer than that and it is a word
		 * this build does not know -- a newer spelling, or a typo -- and
		 * it is dropped rather than rendered as a button whose label is
		 * the mistake.
		 */
		if (word.size() == 1) {
			keys.append(text(word, char32_t(word.at(0).unicode())));
		}
	}

	return keys;
}

QString sk_key_row_spec(const QList<sk_key_t> &keys) {
	QStringList words;

	for (const sk_key_t &key : keys) {
		if (key.kind == sk_key_t::TEXT) {
			words.append(QString(QChar(char16_t(key.value))));
			continue;
		}

		if (key.kind == sk_key_t::CHORD) {
			for (const token_t &entry : SK_KEY_WORDS) {
				if (entry.kind != sk_key_t::MODIFIER) continue;
				if (entry.modifier != key.modifier) continue;

				words.append(QStringLiteral("%1-%2")
				                     .arg(QLatin1String(entry.word))
				                     .arg(QChar(char16_t(key.value))));
				break;
			}
			continue;
		}

		for (const token_t &entry : SK_KEY_WORDS) {
			if (entry.kind != key.kind) continue;
			if (entry.kind == sk_key_t::MODIFIER
			    ? entry.modifier != key.modifier
			    : entry.value != key.value) {
				continue;
			}

			words.append(QLatin1String(entry.word));
			break;
		}
	}

	return words.join(QLatin1Char(' '));
}

bool sk_key_row_catalog::set_custom(const QList<sk_key_t> &keys) {
	if (!sk_key_row_usable(keys)) return false;

	for (sk_key_row_layout_t &layout : m_layouts) {
		if (layout.id != QStringLiteral("custom")) continue;

		layout.keys = keys;
		return true;
	}

	return false;
}
