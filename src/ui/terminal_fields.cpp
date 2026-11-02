#include "ui/terminal_fields.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

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
		emit_changed();
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
		m_form->addRow(new QLabel(QStringLiteral("<b>%1</b>").arg(text), this));
	};
	heading(QStringLiteral("Terminal"));
	m_form->addRow(QStringLiteral("TERM"), m_term);
	m_form->addRow(QStringLiteral("Scrollback"), m_scrollback);
	m_form->addRow(QString(), m_bell);
	m_form->addRow(QStringLiteral("Cursor"), m_cursor_style);
	m_form->addRow(QString(), m_cursor_blink);
	m_form->addRow(QStringLiteral("Features"), m_features);
	m_form->addRow(QStringLiteral("Key row"), m_key_row);
	heading(QStringLiteral("Connection"));
	m_form->addRow(QStringLiteral("Keepalive"), m_keepalive);
	m_form->addRow(QString(), m_compression);
	m_form->addRow(QString(), m_clipboard);
	m_form->addRow(QStringLiteral("On connect"), m_on_connect);

	connect(m_term, &QLineEdit::textEdited, this, &bssh_terminal_fields::emit_changed);
	connect(m_scrollback, &QSpinBox::valueChanged, this, &bssh_terminal_fields::emit_changed);
	connect(m_bell, &QCheckBox::toggled, this, &bssh_terminal_fields::emit_changed);
	connect(m_cursor_style, &QComboBox::currentIndexChanged, this,
	         &bssh_terminal_fields::emit_changed);
	connect(m_cursor_blink, &QCheckBox::toggled, this, &bssh_terminal_fields::emit_changed);
	connect(m_key_row, &QComboBox::currentIndexChanged, this,
	         &bssh_terminal_fields::emit_changed);
	connect(m_keepalive, &QSpinBox::valueChanged, this, &bssh_terminal_fields::emit_changed);
	connect(m_compression, &QCheckBox::toggled, this, &bssh_terminal_fields::emit_changed);
	connect(m_clipboard, &QCheckBox::toggled, this, &bssh_terminal_fields::emit_changed);
	connect(m_on_connect, &QLineEdit::textEdited, this, &bssh_terminal_fields::emit_changed);

	set_values(bssh_preset_t().to_json());
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

void bssh_terminal_fields::set_values(const QJsonObject &settings) {
	m_quiet = true;
	const bssh_preset_t built_in;
	m_term->setText(settings.value(QStringLiteral("term")).toString(built_in.term));
	m_scrollback->setValue(settings.value(QStringLiteral("scrollback_lines"))
	                               .toInt(built_in.scrollback_lines));
	m_bell->setChecked(settings.value(QStringLiteral("bell_enabled")).toBool(built_in.bell_enabled));
	const int style = m_cursor_style->findData(
	        settings.value(QStringLiteral("cursor_style")).toString(built_in.cursor_style));
	m_cursor_style->setCurrentIndex(style < 0 ? 0 : style);
	m_cursor_blink->setChecked(
	        settings.value(QStringLiteral("cursor_blink")).toBool(built_in.cursor_blink));
	m_features_spec = settings.value(QStringLiteral("term_features")).toString();
	m_features->setText(features_button_text(m_features_spec));
	const int row = m_key_row->findData(normalised(QStringLiteral("key_row_layout"),
	        settings.value(QStringLiteral("key_row_layout"))).toString());
	m_key_row->setCurrentIndex(row < 0 ? 0 : row);
	m_keepalive->setValue(settings.value(QStringLiteral("keepalive_seconds"))
	                              .toInt(built_in.keepalive_seconds));
	m_compression->setChecked(
	        settings.value(QStringLiteral("compression")).toBool(built_in.compression));
	m_clipboard->setChecked(settings.value(QStringLiteral("allow_clipboard_write"))
	                                .toBool(built_in.allow_clipboard_write));
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

QJsonObject bssh_terminal_fields::differing_from(const QJsonObject &inherited) const {
	return differing(values(), inherited);
}

QJsonObject bssh_terminal_fields::differing(const QJsonObject &shown, const QJsonObject &inherited) {
	QJsonObject out;
	for (const QString &key : keys()) {
		const QJsonValue mine = normalised(key, shown.value(key));
		if (mine == normalised(key, inherited.value(key))) continue;
		out.insert(key, mine);
	}
	return out;
}
