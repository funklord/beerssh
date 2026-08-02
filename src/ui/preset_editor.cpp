#include "ui/preset_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "model/profile_store.h"
#include "ui/font_picker.h"

/*
 * Scrollback is in lines and the range is deliberately wide: 200 is a
 * phone with little memory to spare, 200000 is a workstation watching a
 * build. The cost of the upper end is memory per session (sec 7), which
 * is why it is a preset field rather than a constant.
 */
static const int BSSH_MIN_SCROLLBACK = 0;
static const int BSSH_MAX_SCROLLBACK = 200000;

bssh_preset_editor::bssh_preset_editor(bssh_profile_store *store, QWidget *parent)
    : QWidget(parent),
      m_store(store) {
	QHBoxLayout *columns = new QHBoxLayout(this);

	/*
	 * The list on the left, exactly where the session list is on the
	 * Session page: two lists in one dialog that behave differently
	 * depending on which page you are on is a dialog that has to be
	 * relearned twice.
	 */
	m_presets = new QListWidget(this);
	m_presets->setMaximumWidth(240);

	/*
	 * Elide rather than scroll sideways. "Raspberry Pi / SBC (shipped)"
	 * is longer than the column, and a horizontal scrollbar under a list
	 * of names hides the end of every name until the user thinks to drag
	 * it -- which is a worse way to lose the "(shipped)" marker than
	 * simply showing less of the name.
	 */
	m_presets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_presets->setTextElideMode(Qt::ElideRight);

	columns->addWidget(m_presets);

	QVBoxLayout *right = new QVBoxLayout();
	columns->addLayout(right, 1);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_name = new QLineEdit(this);

	m_term = new QLineEdit(this);
	m_term->setPlaceholderText(QStringLiteral("xterm-256color"));

	m_scrollback = new QSpinBox(this);
	m_scrollback->setRange(BSSH_MIN_SCROLLBACK, BSSH_MAX_SCROLLBACK);
	m_scrollback->setSuffix(QStringLiteral(" lines"));

	m_bell = new QCheckBox(QStringLiteral("Bell enabled"), this);

	m_cursor_style = new QComboBox(this);
	m_cursor_style->addItem(QStringLiteral("Block"), QStringLiteral("block"));
	m_cursor_style->addItem(QStringLiteral("Underline"), QStringLiteral("underline"));
	m_cursor_style->addItem(QStringLiteral("Bar"), QStringLiteral("bar"));

	m_cursor_blink = new QCheckBox(QStringLiteral("Cursor blinks"), this);

	m_font = new bssh_font_picker(this);

	m_keepalive = new QSpinBox(this);
	m_keepalive->setRange(0, 3600);
	m_keepalive->setSuffix(QStringLiteral(" s"));
	/*
	 * Says what 0 costs rather than only what it means. A phone behind a
	 * carrier NAT with keepalive off loses idle sessions for reasons the
	 * user will blame on the app (sec 4).
	 */
	m_keepalive->setSpecialValueText(QStringLiteral("off (idle sessions may drop)"));

	m_compression = new QCheckBox(QStringLiteral("Request compression"), this);

	m_on_connect = new QLineEdit(this);
	m_on_connect->setPlaceholderText(QStringLiteral("tmux attach || tmux new"));

	form->addRow(QStringLiteral("Name"), m_name);
	form->addRow(QStringLiteral("TERM"), m_term);
	form->addRow(QStringLiteral("Scrollback"), m_scrollback);
	form->addRow(QString(), m_bell);
	form->addRow(QStringLiteral("Cursor"), m_cursor_style);
	form->addRow(QString(), m_cursor_blink);
	form->addRow(QStringLiteral("Keepalive"), m_keepalive);
	form->addRow(QString(), m_compression);
	form->addRow(QStringLiteral("On connect"), m_on_connect);
	right->addLayout(form);

	right->addWidget(m_font);

	/*
	 * The propagation count, always visible rather than only in the
	 * confirmation. Knowing a preset is shared by seven hosts BEFORE
	 * typing into it is what stops the edit from being a surprise at all
	 * (sec 10.1).
	 */
	m_usage = new QLabel(this);
	m_usage->setWordWrap(true);
	right->addWidget(m_usage);

	right->addStretch(1);

	QHBoxLayout *buttons = new QHBoxLayout();
	m_save = new QPushButton(QStringLiteral("Save"), this);
	m_clone = new QPushButton(QStringLiteral("Save as new"), this);
	m_delete = new QPushButton(QStringLiteral("Delete"), this);
	buttons->addStretch(1);
	buttons->addWidget(m_clone);
	buttons->addWidget(m_delete);
	buttons->addWidget(m_save);
	right->addLayout(buttons);

	connect(m_presets, &QListWidget::itemSelectionChanged,
	         this, &bssh_preset_editor::on_selection_changed);
	connect(m_save, &QPushButton::clicked, this, &bssh_preset_editor::on_save);
	connect(m_clone, &QPushButton::clicked, this, &bssh_preset_editor::on_clone);
	connect(m_delete, &QPushButton::clicked, this, &bssh_preset_editor::on_delete);

	reload();
}

void bssh_preset_editor::reload() {
	if (!m_store) return;

	const int keep = m_current.id;

	m_presets->clear();
	for (const bssh_preset_t &preset : m_store->presets()) {
		/*
		 * Shipped ones are marked in the list rather than only when
		 * selected, so the reason Save is unavailable is visible before
		 * the button is reached.
		 */
		const QString label = preset.is_builtin
		        ? preset.name + QStringLiteral("  (shipped)")
		        : preset.name;

		QListWidgetItem *item = new QListWidgetItem(label, m_presets);
		item->setData(Qt::UserRole, preset.id);

		if (preset.id == keep) m_presets->setCurrentItem(item);
	}

	if (!m_presets->currentItem() && m_presets->count() > 0) {
		m_presets->setCurrentRow(0);
	}
}

int bssh_preset_editor::current_preset_id() const {
	const QListWidgetItem *item = m_presets->currentItem();
	return item ? item->data(Qt::UserRole).toInt() : 0;
}

void bssh_preset_editor::on_selection_changed() {
	if (!m_store) return;

	const int id = current_preset_id();
	if (id == 0) return;

	show_preset(m_store->preset(id));
}

void bssh_preset_editor::show_preset(const bssh_preset_t &preset) {
	m_current = preset;

	m_name->setText(preset.name);
	m_term->setText(preset.term);
	m_scrollback->setValue(preset.scrollback_lines);
	m_bell->setChecked(preset.bell_enabled);
	m_cursor_blink->setChecked(preset.cursor_blink);
	m_keepalive->setValue(preset.keepalive_seconds);
	m_compression->setChecked(preset.compression);
	m_on_connect->setText(preset.on_connect_command);
	m_font->set_font_choice(preset.font_family, preset.font_size);

	const int style = m_cursor_style->findData(preset.cursor_style);
	m_cursor_style->setCurrentIndex(style < 0 ? 0 : style);

	update_controls();
}

bssh_preset_t bssh_preset_editor::edited_preset() const {
	/*
	 * Starts from the loaded preset, so fields this form does not show --
	 * colour_scheme and key_row_layout, which arrive with their own UI --
	 * are not silently cleared by saving. The session dialog lost host
	 * settings exactly this way.
	 */
	bssh_preset_t preset = m_current;

	preset.name = m_name->text().trimmed();
	preset.term = m_term->text().trimmed();
	preset.scrollback_lines = m_scrollback->value();
	preset.bell_enabled = m_bell->isChecked();
	preset.cursor_style = m_cursor_style->currentData().toString();
	preset.cursor_blink = m_cursor_blink->isChecked();
	preset.font_family = m_font->font_id();
	preset.font_size = m_font->point_size();
	preset.keepalive_seconds = m_keepalive->value();
	preset.compression = m_compression->isChecked();
	preset.on_connect_command = m_on_connect->text();

	if (preset.term.isEmpty()) preset.term = QStringLiteral("xterm-256color");

	return preset;
}

void bssh_preset_editor::update_controls() {
	const int users = m_store ? m_store->hosts_using(m_current.id) : 0;

	if (m_current.is_builtin) {
		m_usage->setText(QStringLiteral(
		        "This is a shipped preset, so it cannot be changed -- everyone's "
		        "clones start from it. Use \"Save as new\" to make an editable "
		        "copy."));
	} else if (users == 0) {
		m_usage->setText(QStringLiteral("Not used by any host yet."));
	} else {
		m_usage->setText(QStringLiteral("Used by %1 host%2. Saving changes "
		                                 "every one of them that has not "
		                                 "overridden the field.")
		                         .arg(users)
		                         .arg(users == 1 ? QString() : QStringLiteral("s")));
	}

	m_save->setEnabled(!m_current.is_builtin);

	/*
	 * Delete is disabled rather than allowed-then-refused. The store
	 * refuses either way (a preset with hosts behind it cannot go), and a
	 * button that reports failure after being pressed teaches people to
	 * expect failure.
	 */
	m_delete->setEnabled(!m_current.is_builtin && users == 0);
}

void bssh_preset_editor::on_save() {
	if (!m_store || m_current.is_builtin) return;

	const bssh_preset_t preset = edited_preset();
	if (preset.name.isEmpty()) {
		m_name->setFocus();
		return;
	}

	/*
	 * The warning sec 10.1 asks for, and it goes BEFORE the write. The
	 * whole value of a preset is that one edit reaches many hosts; the
	 * whole risk is that the person editing it is thinking about one.
	 */
	const int users = m_store->hosts_using(preset.id);
	if (users > 0) {
		QMessageBox box(this);
		box.setIcon(QMessageBox::Question);
		box.setWindowTitle(QStringLiteral("Change this preset?"));
		box.setText(QStringLiteral("\"%1\" is used by %2 host%3.")
		                    .arg(preset.name)
		                    .arg(users)
		                    .arg(users == 1 ? QString() : QStringLiteral("s")));
		box.setInformativeText(QStringLiteral(
		        "Saving applies these settings to every one of them that has "
		        "not overridden the field itself, including any that are "
		        "connected right now."));
		box.setStandardButtons(QMessageBox::Save | QMessageBox::Cancel);
		box.setDefaultButton(QMessageBox::Cancel);

		if (box.exec() != QMessageBox::Save) return;
	}

	const int id = m_store->save_preset(preset);
	if (id == 0) return;

	m_current.id = id;
	reload();
	emit preset_saved(id);
}

void bssh_preset_editor::on_clone() {
	if (!m_store) return;

	bssh_preset_t clone = edited_preset();
	clone.id = 0;
	clone.is_builtin = false;

	/*
	 * Renamed, because two presets called "Default" in a list is a list
	 * that cannot be used. The suffix is only added when the name is
	 * unchanged -- someone who has already typed a new name meant it.
	 */
	if (clone.name == m_current.name) {
		clone.name = m_current.name + QStringLiteral(" copy");
	}

	const int id = m_store->save_preset(clone);
	if (id == 0) return;

	m_current = clone;
	m_current.id = id;
	reload();
	emit preset_saved(id);
}

void bssh_preset_editor::on_delete() {
	if (!m_store || m_current.id == 0) return;

	const int id = m_current.id;
	if (!m_store->remove_preset(id)) return;

	m_current = bssh_preset_t();
	reload();
	emit preset_removed(id);
}
