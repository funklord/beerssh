#include "ui/terminal_fields.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QToolButton>

#include "model/preset.h"
#include "softkeys/key_row_layout.h"
#include "ui/term_features_editor.h"

namespace {

QString features_button_text(const QString &spec) {
	return bssh_term_features_editor::summary(spec) + QStringLiteral("...");
}

/*
 * A value as it is MEANT: an empty TERM is the built-in one, and an unset
 * key row is the default row. Without this a form showing what it was
 * given would differ from it, and leaving a field alone would record it.
 */
QJsonValue normalised(const QString &key, const QJsonValue &value) {
	if (key == QLatin1String("term")) {
		const QString term = value.toString().trimmed();
		return term.isEmpty() ? bssh_preset_t().term : term;
	}
	if (key == QLatin1String("key_row_layout"))
		return sk_key_row_catalog::instance().effective_id(value.toString());
	return value;
}

} /* namespace */

bssh_terminal_fields::bssh_terminal_fields(QWidget *parent) : QWidget(parent) {
	m_form = new QFormLayout(this);
	m_form->setContentsMargins(0, 0, 0, 0);

	/*
	 * As every other form here: with rows that cannot wrap, a label and
	 * its field set the width of whatever embeds this, and in a wider
	 * font that was a phone's settings page scrolling sideways.
	 */
	m_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	m_form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_term = new QLineEdit(this);
	m_term->setObjectName(QStringLiteral("fields-term"));
	m_term->setPlaceholderText(bssh_preset_t().term);

	m_scrollback = new QSpinBox(this);
	m_scrollback->setObjectName(QStringLiteral("fields-scrollback"));
	m_scrollback->setRange(BSSH_PRESET_MIN_SCROLLBACK, BSSH_PRESET_MAX_SCROLLBACK);
	m_scrollback->setSuffix(QStringLiteral(" lines"));

	m_bell = new QCheckBox(QStringLiteral("Bell enabled"), this);
	m_bell->setObjectName(QStringLiteral("fields-bell"));

	m_cursor_style = new QComboBox(this);
	m_cursor_style->setObjectName(QStringLiteral("fields-cursor"));
	m_cursor_style->addItem(QStringLiteral("Block"), QStringLiteral("block"));
	m_cursor_style->addItem(QStringLiteral("Underline"), QStringLiteral("underline"));
	m_cursor_style->addItem(QStringLiteral("Bar"), QStringLiteral("bar"));

	m_cursor_blink = new QCheckBox(QStringLiteral("Cursor blinks"), this);
	m_cursor_blink->setObjectName(QStringLiteral("fields-cursor-blink"));

	/*
	 * Which extensions the terminal speaks (sec 7.1). A button to a dialog
	 * rather than fourteen checkboxes, the colour editor's shape: sec 7.1
	 * calls it "not a preference knob: it is a test instrument", so it
	 * belongs a deliberate trip away. The label carries the state, which
	 * is the question somebody scanning the page is asking.
	 */
	m_features = new QPushButton(this);
	m_features->setObjectName(QStringLiteral("preset-term-features"));
	m_features->setToolTip(
	        QStringLiteral("Turn terminal extensions off, to see how a program behaves "
	                        "against a terminal that lacks them."));
	connect(m_features, &QPushButton::clicked, this, [this] {
		bssh_term_features_editor editor(m_features_spec, this);
		if (editor.exec() != QDialog::Accepted) return;
		m_features_spec = editor.spec();
		m_features->setText(features_button_text(m_features_spec));
		choose(QStringLiteral("term_features"));
	});

	/*
	 * The on-screen key row (sec 6.2). Shown on both platforms even though
	 * the row only appears on touch: settings are edited wherever the user
	 * happens to be, and one that vanished on the desktop could not be
	 * prepared before picking up the phone.
	 */
	m_key_row = new QComboBox(this);
	m_key_row->setObjectName(QStringLiteral("preset-key-row"));
	for (const sk_key_row_layout_t &layout : sk_key_row_catalog::instance().all()) {
		m_key_row->addItem(layout.display_name, layout.id);
		m_key_row->setItemData(m_key_row->count() - 1, layout.description, Qt::ToolTipRole);
	}

	m_keepalive = new QSpinBox(this);
	m_keepalive->setObjectName(QStringLiteral("fields-keepalive"));
	m_keepalive->setRange(BSSH_PRESET_MIN_KEEPALIVE, BSSH_PRESET_MAX_KEEPALIVE);
	m_keepalive->setSuffix(QStringLiteral(" s"));
	/*
	 * Says what 0 costs rather than only what it means. A phone behind a
	 * carrier NAT with keepalive off loses idle sessions for reasons the
	 * user will blame on the app (sec 4).
	 */
	m_keepalive->setSpecialValueText(QStringLiteral("off (idle sessions may drop)"));

	m_compression = new QCheckBox(QStringLiteral("Request compression"), this);
	m_compression->setObjectName(QStringLiteral("fields-compression"));

	/*
	 * Named for what it lets the host DO rather than for the sequence it
	 * enables: "OSC 52" means nothing to the person deciding whether they
	 * trust this machine, and the decision is about trust (sec 7.1).
	 */
	m_clipboard = new QCheckBox(QStringLiteral("Let the host set my clipboard"), this);
	m_clipboard->setObjectName(QStringLiteral("fields-clipboard"));
	m_clipboard->setToolTip(
	        QStringLiteral("Lets programs on the host copy to your clipboard, so "
	                        "yanking in vim over SSH works. A host that can do this "
	                        "can also replace what you copied, so leave it off for "
	                        "machines you do not control."));

	m_on_connect = new QLineEdit(this);
	m_on_connect->setObjectName(QStringLiteral("fields-on-connect"));
	m_on_connect->setPlaceholderText(QStringLiteral("tmux attach || tmux new"));

	const auto heading = [this](const QString &text) {
		QLabel *label = new QLabel(QStringLiteral("<b>%1</b>").arg(text), this);
		m_form->addRow(label);
		return label;
	};
	m_first_heading = heading(QStringLiteral("Terminal"));
	m_form->addRow(QStringLiteral("TERM"), with_default(QStringLiteral("term"), m_term, QStringLiteral("TERM")));
	m_form->addRow(QStringLiteral("Scrollback"),
	               with_default(QStringLiteral("scrollback_lines"), m_scrollback, QStringLiteral("Scrollback")));
	m_form->addRow(QString(), with_default(QStringLiteral("bell_enabled"), m_bell));
	m_form->addRow(QStringLiteral("Cursor"), with_default(QStringLiteral("cursor_style"), m_cursor_style, QStringLiteral("Cursor")));
	m_form->addRow(QString(), with_default(QStringLiteral("cursor_blink"), m_cursor_blink));
	m_form->addRow(QStringLiteral("Features"), with_default(QStringLiteral("term_features"), m_features, QStringLiteral("Features")));
	m_form->addRow(QStringLiteral("Key row"), with_default(QStringLiteral("key_row_layout"), m_key_row, QStringLiteral("Key row")));
	heading(QStringLiteral("Connection"));
	m_form->addRow(QStringLiteral("Keepalive"),
	               with_default(QStringLiteral("keepalive_seconds"), m_keepalive, QStringLiteral("Keepalive")));
	m_form->addRow(QString(), with_default(QStringLiteral("compression"), m_compression));
	m_form->addRow(QString(), with_default(QStringLiteral("allow_clipboard_write"), m_clipboard));
	m_form->addRow(QStringLiteral("On connect"),
	               with_default(QStringLiteral("on_connect_command"), m_on_connect, QStringLiteral("On connect")));

	/* A change somebody makes is a choice; one this form makes is not (m_quiet). */
	const auto chooses = [this](const char *key) {
		return [this, key] { choose(QString::fromLatin1(key)); };
	};
	connect(m_term, &QLineEdit::textEdited, this, chooses("term"));
	connect(m_scrollback, &QSpinBox::valueChanged, this, chooses("scrollback_lines"));
	connect(m_bell, &QCheckBox::toggled, this, chooses("bell_enabled"));
	connect(m_cursor_style, &QComboBox::currentIndexChanged, this, chooses("cursor_style"));
	connect(m_cursor_blink, &QCheckBox::toggled, this, chooses("cursor_blink"));
	connect(m_key_row, &QComboBox::currentIndexChanged, this, chooses("key_row_layout"));
	connect(m_keepalive, &QSpinBox::valueChanged, this, chooses("keepalive_seconds"));
	connect(m_compression, &QCheckBox::toggled, this, chooses("compression"));
	connect(m_clipboard, &QCheckBox::toggled, this, chooses("allow_clipboard_write"));
	connect(m_on_connect, &QLineEdit::textEdited, this, chooses("on_connect_command"));

	show_settings(bssh_preset_t().to_json(), QJsonObject());
}

/*
 * The field with its way back: a Default button, shown while the field is
 * chosen, that puts it back to following. Qt Designer's property editor is
 * the shape -- a set property in bold with a reset beside it -- after the
 * "Default (x)" combo was weighed and refused: it would turn four check
 * boxes into combos and has no form at all for a number or a line of text.
 */
QWidget *bssh_terminal_fields::with_default(const QString &key, QWidget *field,
                                            const QString &label) {
	/*
	 * Named for its row: the form's label is the wrapper's buddy now, not
	 * the field's, and a screen reader would otherwise say "empty".
	 */
	if (!label.isEmpty() && field->accessibleName().isEmpty()) field->setAccessibleName(label);

	QWidget *row = new QWidget(this);
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(field, 1);

	QToolButton *back = new QToolButton(row);
	back->setObjectName(QStringLiteral("fields-default-") + key);
	back->setText(QStringLiteral("Default"));
	back->setAccessibleName(QStringLiteral("Use the default"));
	back->setVisible(false);
	connect(back, &QToolButton::clicked, this, [this, key] { follow(key); });
	layout->addWidget(back);

	m_defaults.insert(key, back);
	m_fields_by_key.insert(key, field);
	return row;
}

void bssh_terminal_fields::choose(const QString &key) {
	if (m_quiet) return;
	m_chosen.insert(key);
	mark(key);
	emit changed();
}

void bssh_terminal_fields::follow(const QString &key) {
	m_chosen.remove(key);
	fill(m_inherited, { key });
	mark(key);
	emit changed();
}

void bssh_terminal_fields::mark(const QString &key) {
	const bool own = m_chosen.contains(key);
	QToolButton *back = m_defaults.value(key);
	QWidget *field = m_fields_by_key.value(key);
	if (!back || !field) return;
	back->setVisible(own);
	back->setToolTip(QStringLiteral("Follow %1 again").arg(m_source_name));

	/* Bold where chosen: the row's label, or a check box's own text. */
	QLabel *label = qobject_cast<QLabel *>(m_form->labelForField(field->parentWidget()));
	QWidget *shown = label && !label->text().isEmpty() ? static_cast<QWidget *>(label) : field;
	QFont font = shown->font();
	font.setBold(own);
	shown->setFont(font);
}

void bssh_terminal_fields::set_first_heading_shown(bool shown) {
	m_first_heading->setVisible(shown);
}

QStringList bssh_terminal_fields::keys() {
	return { QStringLiteral("term"),           QStringLiteral("scrollback_lines"),
	         QStringLiteral("bell_enabled"),   QStringLiteral("cursor_style"),
	         QStringLiteral("cursor_blink"),   QStringLiteral("term_features"),
	         QStringLiteral("key_row_layout"), QStringLiteral("keepalive_seconds"),
	         QStringLiteral("compression"),    QStringLiteral("allow_clipboard_write"),
	         QStringLiteral("on_connect_command") };
}

void bssh_terminal_fields::emit_changed() {
	if (!m_quiet) emit changed();
}

void bssh_terminal_fields::show_settings(const QJsonObject &inherited, const QJsonObject &own) {
	m_inherited = inherited;
	m_chosen.clear();
	QJsonObject shown = inherited;
	for (const QString &key : keys()) {
		if (!own.contains(key)) continue;
		m_chosen.insert(key);
		shown.insert(key, own.value(key));
	}
	const QStringList all = keys();
	fill(shown, QSet<QString>(all.begin(), all.end()));
	for (const QString &key : all) mark(key);
}

void bssh_terminal_fields::set_inherited(const QJsonObject &inherited) {
	m_inherited = inherited;
	QSet<QString> following;
	for (const QString &key : keys())
		if (!m_chosen.contains(key)) following.insert(key);
	fill(inherited, following);
}

void bssh_terminal_fields::fill(const QJsonObject &settings, const QSet<QString> &keys) {
	m_quiet = true;
	const bssh_preset_t built_in;
	const auto has = [&keys](const char *key) { return keys.contains(QString::fromLatin1(key)); };
	if (has("term")) m_term->setText(settings.value(QStringLiteral("term")).toString(built_in.term));
	if (has("scrollback_lines"))
		m_scrollback->setValue(settings.value(QStringLiteral("scrollback_lines"))
		                               .toInt(built_in.scrollback_lines));
	if (has("bell_enabled"))
		m_bell->setChecked(settings.value(QStringLiteral("bell_enabled")).toBool(built_in.bell_enabled));
	if (has("cursor_style")) {
		const int style = m_cursor_style->findData(
		        settings.value(QStringLiteral("cursor_style")).toString(built_in.cursor_style));
		m_cursor_style->setCurrentIndex(style < 0 ? 0 : style);
	}
	if (has("cursor_blink"))
		m_cursor_blink->setChecked(
		        settings.value(QStringLiteral("cursor_blink")).toBool(built_in.cursor_blink));
	if (has("term_features")) {
		m_features_spec = settings.value(QStringLiteral("term_features")).toString();
		m_features->setText(features_button_text(m_features_spec));
	}
	if (has("key_row_layout")) {
		const int row = m_key_row->findData(normalised(QStringLiteral("key_row_layout"),
		        settings.value(QStringLiteral("key_row_layout"))).toString());
		m_key_row->setCurrentIndex(row < 0 ? 0 : row);
	}
	if (has("keepalive_seconds"))
		m_keepalive->setValue(settings.value(QStringLiteral("keepalive_seconds"))
		                              .toInt(built_in.keepalive_seconds));
	if (has("compression"))
		m_compression->setChecked(
		        settings.value(QStringLiteral("compression")).toBool(built_in.compression));
	if (has("allow_clipboard_write"))
		m_clipboard->setChecked(settings.value(QStringLiteral("allow_clipboard_write"))
		                                .toBool(built_in.allow_clipboard_write));
	if (has("on_connect_command"))
		m_on_connect->setText(settings.value(QStringLiteral("on_connect_command")).toString());
	m_quiet = false;
}

QJsonObject bssh_terminal_fields::values() const {
	QJsonObject out;
	out.insert(QStringLiteral("term"), m_term->text().trimmed());
	out.insert(QStringLiteral("scrollback_lines"), m_scrollback->value());
	out.insert(QStringLiteral("bell_enabled"), m_bell->isChecked());
	out.insert(QStringLiteral("cursor_style"), m_cursor_style->currentData().toString());
	out.insert(QStringLiteral("cursor_blink"), m_cursor_blink->isChecked());
	out.insert(QStringLiteral("term_features"), m_features_spec);
	out.insert(QStringLiteral("key_row_layout"), m_key_row->currentData().toString());
	out.insert(QStringLiteral("keepalive_seconds"), m_keepalive->value());
	out.insert(QStringLiteral("compression"), m_compression->isChecked());
	out.insert(QStringLiteral("allow_clipboard_write"), m_clipboard->isChecked());
	out.insert(QStringLiteral("on_connect_command"), m_on_connect->text());
	return out;
}

QJsonObject bssh_terminal_fields::chosen_values() const {
	const QJsonObject all = values();
	QJsonObject out;
	for (const QString &key : m_chosen) out.insert(key, normalised(key, all.value(key)));
	return out;
}
