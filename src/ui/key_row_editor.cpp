#include "ui/key_row_editor.h"

#include "keyboard/key_row_layout.h"
#include "term/emulator.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

struct group_t {
	const char *heading;
	const char *words;
	const char *note;
};

/*
 * The vocabulary, grouped for somebody who has not read sec 6.2.
 *
 * One table builds both the note on screen and vocabulary(), so the words
 * a reader is told about and the words the suite checks are the same
 * list. A second copy would be a second thing to keep in step -- the
 * shape sec 6.4 records for the context menu that spelled its own chords
 * out -- and the test asserts every word here parses, so a word this
 * build does not know cannot sit in the note unnoticed.
 */
const group_t BSSH_ROW_GROUPS[] = {
	{ "Keys",
	  "esc tab enter bksp ins del home end pgup pgdn up down left right",
	  "" },
	{ "Modifiers", "ctrl alt shift",
	  "sticky -- they apply to the next key" },
	{ "Chords", "ctrl-b alt-x",
	  "one press for what would take two" },
	{ "Other", "fn gap",
	  "the F1..F12 bank, and a blank column" }
};

} /* namespace */

bssh_key_row_editor::bssh_key_row_editor(const QString &spec, QWidget *parent)
    : QDialog(parent), m_spec(nullptr), m_preview(nullptr), m_status(nullptr),
      m_buttons(nullptr) {
	setWindowTitle(QStringLiteral("Custom key row"));

	QVBoxLayout *column = new QVBoxLayout(this);

	QFormLayout *form = new QFormLayout;
	m_spec = new QLineEdit(spec, this);
	m_spec->setPlaceholderText(QStringLiteral("esc tab ctrl alt left down up right"));
	form->addRow(QStringLiteral("Row"), m_spec);
	column->addLayout(form);

	QStringList lines;
	for (const group_t &group : BSSH_ROW_GROUPS) {
		const QString note = QString::fromLatin1(group.note);
		lines.append(QStringLiteral("%1: %2%3")
		                     .arg(QLatin1String(group.heading))
		                     .arg(QLatin1String(group.words))
		                     .arg(note.isEmpty() ? QString()
		                                         : QStringLiteral(" -- ") + note));
	}
	lines.append(QStringLiteral(
	        "Anything else one character long is itself: | / ~ - $ ^"));

	QLabel *vocabulary = new QLabel(lines.join(QLatin1Char('\n')), this);
	vocabulary->setWordWrap(true);
	column->addWidget(vocabulary);

	/*
	 * What the words came out as, which is the half a spec cannot show.
	 * A dropped word is invisible in the text the user typed -- it is
	 * still sitting there -- and visible here as a key that is not in the
	 * preview, which is the only way they find out before saving.
	 */
	m_preview = new QLabel(this);
	m_preview->setWordWrap(true);
	m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
	form->addRow(QStringLiteral("Preview"), m_preview);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	column->addWidget(m_status);

	m_buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel
	                | QDialogButtonBox::RestoreDefaults,
	        this);
	connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	/*
	 * Restore puts back the row the catalogue's default holds rather than
	 * clearing the field: an empty box is not a row anybody wants, and
	 * "default" is the thing the user is most likely to want to start
	 * from after making a mess.
	 */
	connect(m_buttons->button(QDialogButtonBox::RestoreDefaults),
	        &QPushButton::clicked, this, [this] {
		const bssh_key_row_catalog catalog;
		if (const bssh_key_row_layout_t *fallback =
		            catalog.by_id(catalog.default_id())) {
			m_spec->setText(bssh_key_row_spec(fallback->keys));
		}
	});

	column->addWidget(m_buttons);

	connect(m_spec, &QLineEdit::textChanged, this, &bssh_key_row_editor::refresh);
	refresh();
}

QString bssh_key_row_editor::spec() const {
	return m_spec->text().simplified();
}

void bssh_key_row_editor::refresh() {
	const QList<bssh_key_t> keys = bssh_parse_key_row(m_spec->text());

	QStringList labels;
	for (const bssh_key_t &key : keys) {
		labels.append(key.kind == bssh_key_t::SPACER ? QStringLiteral("[ ]")
		                                             : key.label);
	}
	m_preview->setText(labels.isEmpty() ? QStringLiteral("nothing yet")
	                                    : labels.join(QStringLiteral("  ")));

	const bool usable = bssh_key_row_usable(keys);

	/*
	 * Say which half is missing, not merely that something is. "Add an
	 * Escape" is actionable where "invalid row" sends somebody back to
	 * the word list to guess which of five rules they broke.
	 */
	if (usable) {
		m_status->setText(QStringLiteral("%1 key(s).").arg(keys.size()));
	} else {
		QStringList missing;
		bool has_escape = false;
		bool has_modifier = false;
		for (const bssh_key_t &key : keys) {
			if (key.kind == bssh_key_t::KEY
			    && key.value == bssh_emulator::KEY_ESCAPE) {
				has_escape = true;
			}
			if (key.kind == bssh_key_t::MODIFIER) has_modifier = true;
		}
		if (!has_escape) missing.append(QStringLiteral("esc"));
		if (!has_modifier) {
			missing.append(QStringLiteral("a modifier (ctrl, alt or shift)"));
		}

		m_status->setText(
		        QStringLiteral("A row needs %1: without them it cannot drive a "
		                        "terminal.")
		                .arg(missing.join(QStringLiteral(" and "))));
	}

	m_buttons->button(QDialogButtonBox::Ok)->setEnabled(usable);
}

QStringList bssh_key_row_editor::vocabulary() {
	QStringList words;
	for (const group_t &group : BSSH_ROW_GROUPS) {
		words += QString::fromLatin1(group.words)
		                 .split(QLatin1Char(' '), Qt::SkipEmptyParts);
	}
	return words;
}
