#include "term/term_features.h"

namespace {

struct entry_t {
	const char *name;
	const char *description;
	bool switchable;
};

/*
 * Indexed by feature_t, and the order must match the enum. A table
 * rather than a switch so that every_name() and the help text cannot
 * disagree with from_name(): one list, three readers.
 */
const entry_t g_features[bssh_term_features_t::FEATURE_COUNT] = {
	{ "bracketed-paste",
		"DEC 2004. Wraps pasted text so a shell knows it was pasted.", false },
	{ "sgr-mouse",
		"DEC 1006 and the mouse modes that feed it.", false },
	{ "focus-reporting",
		"DEC 1004. Tells the remote when a pane gains or loses focus.", false },
	{ "kitty-keyboard",
		"CSI > flags u. Progressive keyboard disambiguation.", true },
	{ "modify-other-keys",
		"CSI > 4 ; n m. xterm's keyboard disambiguation, level 2.", true },
	{ "synchronised-output",
		"DEC 2026. The remote brackets an update so it paints in one frame.", true },
	{ "hyperlinks",
		"OSC 8. Marks a run of cells as a clickable URL.", true },
	{ "clipboard-write",
		"OSC 52 write. Also needs the host's own permission.", true },
	{ "xtgettcap",
		"DCS + q. Lets a remote ask what this terminal supports.", true },
	{ "colour-query",
		"OSC 10 and OSC 11. The terminal's own foreground and background.", true },
	{ "window-size",
		"CSI 14 t and CSI 16 t. The text area and one cell, in pixels.", true },
	{ "sixel",
		"DCS q. Images as a bitmap the terminal decodes.", true },
	{ "kitty-graphics",
		"APC G. Images as base64 with key=value controls.", true },
	{ "iterm2-images",
		"OSC 1337. Images as base64 with iTerm2's own arguments.", true },
};

bool g_override_set = false;
bssh_term_features_t g_override;

} /* namespace */

void bssh_term_features_t::set_process_override(const bssh_term_features_t &features) {
	g_override = features;
	g_override_set = true;
}

bool bssh_term_features_t::process_override(bssh_term_features_t &features) {
	if (!g_override_set) return false;

	features = g_override;
	return true;
}

const char *bssh_term_features_t::name(feature_t feature) {
	if (feature < 0 || feature >= FEATURE_COUNT) return "";
	return g_features[feature].name;
}

const char *bssh_term_features_t::description(feature_t feature) {
	if (feature < 0 || feature >= FEATURE_COUNT) return "";
	return g_features[feature].description;
}

bool bssh_term_features_t::from_name(const QString &name, feature_t &feature) {
	for (int i = 0; i < FEATURE_COUNT; ++i) {
		if (name != QString::fromLatin1(g_features[i].name)) continue;

		feature = feature_t(i);
		return true;
	}
	return false;
}

bool bssh_term_features_t::switchable(feature_t feature) {
	if (feature < 0 || feature >= FEATURE_COUNT) return false;
	return g_features[feature].switchable;
}

bool bssh_term_features_t::operator==(const bssh_term_features_t &other) const {
	for (int i = 0; i < FEATURE_COUNT; ++i) {
		if (m_on[i] != other.m_on[i]) return false;
	}
	return true;
}

QStringList bssh_term_features_t::every_name() {
	QStringList names;
	for (int i = 0; i < FEATURE_COUNT; ++i) {
		names.append(QString::fromLatin1(g_features[i].name));
	}
	return names;
}

bssh_term_features_t bssh_term_features_t::from_spec(const QString &spec, QString &error) {
	bssh_term_features_t features;
	error.clear();

	const QString trimmed = spec.trimmed();
	if (trimmed.isEmpty()) return features;

	const QStringList words = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);

	for (const QString &raw : words) {
		const QString word = raw.trimmed();
		if (word.isEmpty()) continue;

		if (word == QLatin1String("all") || word == QLatin1String("none")) {
			const bool on = word == QLatin1String("all");
			for (int i = 0; i < FEATURE_COUNT; ++i) features.m_on[i] = on;
			continue;
		}

		bool on = true;
		QString bare = word;
		if (bare.startsWith(QLatin1Char('-'))) {
			on = false;
			bare = bare.mid(1);
		} else if (bare.startsWith(QLatin1Char('+'))) {
			bare = bare.mid(1);
		}

		feature_t feature = BRACKETED_PASTE;
		if (!from_name(bare, feature)) {
			/*
			 * Named rather than ignored, and the message lists what
			 * would have been accepted. A harness that misspells a
			 * feature would otherwise run its whole suite against a
			 * terminal that still had it, and report a pass for a
			 * configuration nobody tested.
			 */
			error = QStringLiteral("unknown terminal feature \"%1\"; known ones are %2")
			                .arg(bare, every_name().join(QStringLiteral(", ")));
			return bssh_term_features_t();
		}

		if (!on && !switchable(feature)) {
			/*
			 * Refused rather than accepted-and-ignored. A harness
			 * asking for a terminal without bracketed paste and
			 * silently getting one WITH it would report a pass for a
			 * configuration nobody tested -- the same green-result-
			 * about-the-wrong-thing an unknown name would produce.
			 */
			error = QStringLiteral(
			                "\"%1\" cannot be switched off yet: libvterm owns the "
			                "mode and answers DECRQM about it, so refusing to act "
			                "on it would advertise a feature this terminal was not "
			                "honouring")
			                .arg(bare);
			return bssh_term_features_t();
		}

		features.m_on[feature] = on;
	}

	return features;
}

QString bssh_term_features_t::to_spec() const {
	QStringList off;
	for (int i = 0; i < FEATURE_COUNT; ++i) {
		if (!m_on[i]) off.append(QLatin1Char('-') + QString::fromLatin1(g_features[i].name));
	}
	return off.join(QLatin1Char(','));
}
