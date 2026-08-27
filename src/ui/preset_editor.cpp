#include "ui/touch_metrics.h"
#include "ui/preset_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>

#include "ui/scroll_guard.h"
#include "ui/wrapped_note.h"
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QScroller>
#include <QScrollArea>
#include <QVBoxLayout>

#include "model/profile_store.h"
#include "platform/platform.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"
#include "ui/key_row_layout.h"

/*
 * Scrollback is in lines and the range is deliberately wide: 200 is a
 * phone with little memory to spare, 200000 is a workstation watching a
 * build. The cost of the upper end is memory per session (sec 7), which
 * is why it is a preset field rather than a constant.
 */
static const int BSSH_MIN_SCROLLBACK = 0;
static const int BSSH_MAX_SCROLLBACK = 200000;

bssh_preset_editor::bssh_preset_editor(bssh_profile_store *store, layout_t layout,
                                        section_t section, QWidget *parent)
    : QWidget(parent),
      m_store(store),
      m_section(section),
      m_touch(layout == TOUCH) {
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

	QVBoxLayout *right = new QVBoxLayout();

	/*
	 * The fields scroll; the buttons do not.
	 *
	 * Sixteen settings, a font picker and a colour strip need 1063px of
	 * height, and a phone has 640. Without anywhere to scroll, Qt squeezes
	 * the rows past their minimums until they overlap -- which is what a
	 * device showed: the scrollback spin box drawn across the bell
	 * checkbox. Save and Delete stay pinned rather than becoming something
	 * to scroll down to.
	 */
	QWidget *content = new QWidget(this);
	QVBoxLayout *fields = new QVBoxLayout(content);
	fields->setContentsMargins(0, 0, 0, 0);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	/* Sideways scrolling would mean something above is too wide, which is
	 * a bug rather than something to let the user work around. */
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
	right->addWidget(scroll, 1);

	if (m_touch) {
		/*
		 * One column. Side by side, the list gets a fifth of a phone's
		 * width and every preset reads "Default Lin...", which is a
		 * chooser that cannot be chosen from. Above the fields it gets
		 * the whole width and only the height it needs.
		 */
		m_presets->setMaximumWidth(QWIDGETSIZE_MAX);
		m_presets->setMaximumHeight(200);
		QScroller::grabGesture(m_presets->viewport(), QScroller::LeftMouseButtonGesture);

		QVBoxLayout *single = new QVBoxLayout();
		single->addWidget(m_presets, 0);
		single->addLayout(right, 1);
		columns->addLayout(single);
	} else {
		columns->addWidget(m_presets);
		columns->addLayout(right, 1);
	}

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

	/*
	 * Named for what it lets the host DO rather than for the sequence it
	 * enables: "OSC 52" means nothing to the person deciding whether they
	 * trust this machine, and the decision is about trust (sec 7.1).
	 */
	m_clipboard = new QCheckBox(
	        QStringLiteral("Let this host set my clipboard"), this);
	m_clipboard->setToolTip(
	        QStringLiteral("Lets programs on the host copy to your clipboard, so "
	                        "yanking in vim over SSH works. A host that can do this "
	                        "can also replace what you copied, so leave it off for "
	                        "machines you do not control."));

	m_on_connect = new QLineEdit(this);
	m_on_connect->setPlaceholderText(QStringLiteral("tmux attach || tmux new"));

	/*
	 * The on-screen key row for this kind of host (sec 6.2). Shown on both
	 * platforms even though the row itself only appears on touch: a
	 * preset is edited wherever the user happens to be, and a setting that
	 * vanished on the desktop would be one they could not prepare before
	 * picking up the phone.
	 */
	m_key_row = new QComboBox(this);
	for (const bssh_key_row_layout_t &layout : bssh_key_row_catalog::instance().all()) {
		m_key_row->addItem(layout.display_name, layout.id);
		m_key_row->setItemData(m_key_row->count() - 1, layout.description, Qt::ToolTipRole);
	}

	/*
	 * Which rows this view shows (sec 8.5). Every field is BUILT either
	 * way -- load() and save() address them all, and a field that existed
	 * only on one page would be a field the other page silently discarded
	 * on save. Only what is added to the form differs.
	 */
	const bool all = m_section == EVERYTHING;
	const bool terminal = all || m_section == TERMINAL;
	const bool connection = all || m_section == CONNECTION;

	/*
	 * A field this view does not show is HIDDEN, not merely left out of
	 * the form.
	 *
	 * Every one of them was built with this widget as its parent, and a
	 * widget with a parent and no layout is drawn wherever its default
	 * geometry puts it -- which is the top-left corner. Leaving them out
	 * of the form painted the key-row combo and the name field over the
	 * preset list, on top of its first row. Found by looking at the
	 * page; no assertion here would have said a word, because every
	 * widget was present, sized and inside the page.
	 */
	const auto shown = [](QWidget *field, bool visible) {
		if (!visible) field->hide();
		return visible;
	};

	if (shown(m_name, all)) form->addRow(QStringLiteral("Name"), m_name);

	if (shown(m_term, terminal)) form->addRow(QStringLiteral("TERM"), m_term);
	if (shown(m_scrollback, terminal)) form->addRow(QStringLiteral("Scrollback"), m_scrollback);
	if (shown(m_bell, terminal)) form->addRow(QString(), m_bell);
	if (shown(m_cursor_style, terminal)) form->addRow(QStringLiteral("Cursor"), m_cursor_style);
	if (shown(m_cursor_blink, terminal)) form->addRow(QString(), m_cursor_blink);

	if (shown(m_keepalive, connection)) form->addRow(QStringLiteral("Keepalive"), m_keepalive);
	if (shown(m_compression, connection)) form->addRow(QString(), m_compression);
	if (shown(m_clipboard, connection)) form->addRow(QString(), m_clipboard);
	if (shown(m_on_connect, connection)) form->addRow(QStringLiteral("On connect"), m_on_connect);

	if (shown(m_key_row, all)) form->addRow(QStringLiteral("Key row"), m_key_row);
	fields->addLayout(form);

	/*
	 * The font and colours belong to the Appearance category, not to
	 * Terminal or Connection -- so a section view builds the picker (save
	 * writes its values back) and does not show it twice.
	 */
	fields->addWidget(m_font);
	if (m_section != EVERYTHING) m_font->hide();

	/*
	 * The picker brings its own form layout, and two of those size their
	 * label columns independently -- so without this its rows sit 22px
	 * left of every row above them (measured on the shot). Done after
	 * both are populated, because it is the labels' size hints that
	 * decide the width.
	 */
	bssh_font_picker::align_labels(form, m_font);

	/*
	 * The propagation count, always visible rather than only in the
	 * confirmation. Knowing a preset is shared by seven hosts BEFORE
	 * typing into it is what stops the edit from being a surprise at all
	 * (sec 10.1).
	 */
	m_usage = new QLabel(this);
	bssh_wrap_note(m_usage);
	fields->addWidget(m_usage);

	fields->addStretch(1);

	/*
	 * sec 10.5. A preset is the one thing in this app that is safe to send
	 * to someone -- it holds no host, no user and no credential by
	 * construction (sec 10.3) -- so sharing it is a feature rather than a
	 * risk, and it is worth a button rather than an instruction to find
	 * the config directory.
	 */
	QHBoxLayout *transfer = new QHBoxLayout();
	m_import = new QPushButton(QStringLiteral("Import..."), this);
	m_export = new QPushButton(QStringLiteral("Export..."), this);
	transfer->addStretch(1);
	transfer->addWidget(m_import);
	transfer->addWidget(m_export);
	right->addLayout(transfer);

	/*
	 * Import and Export move whole preset FILES between machines, which
	 * is the Presets category's business. On the Terminal and Connection
	 * pages they are two more buttons between a reader and the four
	 * settings they came for -- and this dialog was reported as
	 * confusing, so an operation offered in three places when it belongs
	 * in one is worth removing rather than defending.
	 *
	 * Hidden rather than left unbuilt: save() and the shipped-preset
	 * rules address them, and a pointer that exists on one page and not
	 * another is a null nobody remembers to check.
	 */
	if (m_section != EVERYTHING) {
		m_import->hide();
		m_export->hide();
	}

	QHBoxLayout *buttons = new QHBoxLayout();
	m_save = new QPushButton(QStringLiteral("Save"), this);
	m_clone = new QPushButton(QStringLiteral("Save as new"), this);
	m_delete = new QPushButton(QStringLiteral("Delete"), this);
	buttons->addStretch(1);
	buttons->addWidget(m_clone);
	buttons->addWidget(m_delete);
	buttons->addWidget(m_save);
	right->addLayout(buttons);

	if (m_touch) {
		/*
		 * A page with no way out is the one bug a phone punishes hardest:
		 * the system back gesture is not wired to this stack, so without
		 * this the only exit from the preset editor is killing the app.
		 */
		QPushButton *back = new QPushButton(QStringLiteral("Back to hosts"), this);
		back->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		connect(back, &QPushButton::clicked, this, &bssh_preset_editor::back_requested);
		right->addWidget(back);

		/*
		 * Every control on the page, found rather than listed.
		 *
		 * It WAS a list, and the list is what kept going wrong: Import
		 * and Export were left out of it once and stayed short while
		 * the eleven controls around them grew, and the clipboard
		 * checkbox was left out of it after that and stood 23dp -- half
		 * sec 6.2's floor -- on the one layout built for a thumb. A
		 * page that grows a control must not also have to remember a
		 * second place, and now it does not.
		 */
		bssh_apply_touch_heights(this, true);

	/*
	 * And the wheel scrolls this page rather than changing whatever
	 * control the pointer is over. Last, because it walks the form
	 * and the form has to exist.
	 */
	bssh_dont_steal_the_wheel(this);
		m_font->set_touch_sized(true);
	}

	connect(m_presets, &QListWidget::itemSelectionChanged,
	         this, &bssh_preset_editor::on_selection_changed);
	connect(m_save, &QPushButton::clicked, this, &bssh_preset_editor::on_save);
	connect(m_clone, &QPushButton::clicked, this, &bssh_preset_editor::on_clone);
	connect(m_delete, &QPushButton::clicked, this, &bssh_preset_editor::on_delete);
	connect(m_export, &QPushButton::clicked, this, &bssh_preset_editor::on_export);
	connect(m_import, &QPushButton::clicked, this, &bssh_preset_editor::on_import);

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
	m_clipboard->setChecked(preset.allow_clipboard_write);
	m_on_connect->setText(preset.on_connect_command);
	m_font->set_font_choice(preset.font_family, preset.font_size);
	m_font->set_scheme(preset.colour_scheme);

	const bssh_key_row_catalog &rows = bssh_key_row_catalog::instance();
	const int row_index = m_key_row->findData(rows.effective_id(preset.key_row_layout));
	m_key_row->setCurrentIndex(row_index < 0 ? 0 : row_index);

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
	/*
	 * An untouched font stays UNSET rather than becoming whatever the
	 * picker resolved it to. The picker cannot show "unset" -- an empty
	 * value means "the default for this form factor" (sec 9.2), so it
	 * displays that default -- and writing the resolved value back would
	 * pin it: a preset exported from a desktop would then carry
	 * jetbrains-mono, and importing it on a phone would quietly override
	 * the condensed default that form factor is supposed to get.
	 *
	 * Same rule as a host override: do not record a value that only
	 * agrees with what it would have resolved to anyway.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	const bool compact = platform && platform->is_touch_primary();
	const QString resolved = bssh_font_catalog::instance()
	                                 .effective_id(m_current.font_family, compact);

	if (m_font->font_id() != resolved) preset.font_family = m_font->font_id();
	preset.font_size = m_font->point_size();

	const bssh_key_row_catalog &rows = bssh_key_row_catalog::instance();
	if (m_key_row->currentData().toString() != rows.effective_id(m_current.key_row_layout)) {
		preset.key_row_layout = m_key_row->currentData().toString();
	}

	/* Same rule for the scheme: an untouched default stays unset. */
	const bssh_colour_scheme_catalog &schemes = bssh_colour_scheme_catalog::instance();
	if (m_font->scheme_id() != schemes.effective_id(m_current.colour_scheme)) {
		preset.colour_scheme = m_font->scheme_id();
	}
	preset.keepalive_seconds = m_keepalive->value();
	preset.compression = m_compression->isChecked();
	preset.allow_clipboard_write = m_clipboard->isChecked();
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

void bssh_preset_editor::on_export() {
	const bssh_preset_t preset = edited_preset();

	/*
	 * Exports what is ON SCREEN, not what was last saved. Someone who has
	 * just typed a change and wants to send it to a colleague means the
	 * change; making them save first -- into a shipped preset they cannot
	 * save into -- would be a rule with no reason behind it.
	 */
	const QString suggested = QStringLiteral("%1.ini").arg(
	        preset.name.isEmpty() ? QStringLiteral("preset") : preset.name);

	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	const QString path = QFileDialog::getSaveFileName(
	        this, QStringLiteral("Export preset"), suggested,
	        touch ? QString() : QStringLiteral("Preset files (*.ini)"));

	if (path.isEmpty()) return;

	if (!preset.write_ini(path)) {
		QMessageBox::warning(this, QStringLiteral("Export failed"),
		                      QStringLiteral("Could not write %1.").arg(path));
	}
}

void bssh_preset_editor::on_import() {
	/*
	 * "All files" on touch, rather than the suffix filter. Android's
	 * document picker filters by MIME TYPE, and Qt derives the intent's
	 * type from the name filter: ".ini" is not a type Android knows, so
	 * every preset file was offered as an unselectable "BIN file" and
	 * import was impossible on the platform rather than merely awkward.
	 * Found by tapping the button on a device -- the desktop dialog
	 * filters by suffix and was perfectly fine.
	 *
	 * Nothing is lost by dropping it: read_ini validates the CONTENT and
	 * refuses a file that is not a preset, which is the check that
	 * actually matters -- an extension has never been evidence of what is
	 * inside a file.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	QFileDialog dialog(this, QStringLiteral("Import preset"));
	dialog.setAcceptMode(QFileDialog::AcceptOpen);
	dialog.setFileMode(QFileDialog::ExistingFile);

	if (touch) {
		/*
		 * KNOWN LIMITATION, and deliberately not papered over: on the
		 * Android 11 emulator image this picker lists preset files but
		 * will not let one be chosen. The intent Qt sends is
		 * ACTION_OPEN_DOCUMENT with a wildcard type and category
		 * OPENABLE, which looks correct, and neither a suffix filter,
		 * no filter, an
		 * "All files" filter, nor explicit MIME filters (octet-stream,
		 * text-plain, wildcard) changed it -- a .txt file is refused
		 * just as an .ini is. EXPORT works on the same device, so the
		 * content:// plumbing underneath is fine; it is specific to
		 * choosing an existing document.
		 *
		 * Left as the plain wildcard rather than as a cleverer guess,
		 * because four guesses have already been wrong and the next
		 * person deserves the simple version to debug rather than mine.
		 * sec 10.5 records it as open; the likely route is an
		 * intent-filter so a preset is opened from a file manager INTO
		 * BeerSSH, which is the more Android-shaped answer anyway.
		 */
		dialog.setMimeTypeFilters({ QStringLiteral("*/*") });
	} else {
		dialog.setNameFilter(QStringLiteral("Preset files (*.ini)"));
	}

	if (dialog.exec() != QDialog::Accepted) return;

	const QList<QUrl> chosen = dialog.selectedUrls();
	if (chosen.isEmpty()) return;

	/*
	 * A local path where there is one, the content:// URL otherwise --
	 * read_ini takes either, and on Android there is no path to be had.
	 */
	const QUrl url = chosen.first();
	const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();

	if (path.isEmpty() || !m_store) return;

	bool ok = false;
	bssh_preset_t imported = bssh_preset_t::read_ini(path, &ok);

	if (!ok) {
		QMessageBox::warning(this, QStringLiteral("Import failed"),
		                      QStringLiteral("%1 does not look like a preset file.")
		                              .arg(path));
		return;
	}

	/*
	 * Added, never merged over an existing one of the same name. An
	 * import that silently replaced a preset would change every host
	 * following it -- the propagation this editor otherwise asks about
	 * explicitly, done invisibly by opening a file.
	 */
	imported.name = m_store->unique_preset_name(imported.name);

	const int id = m_store->save_preset(imported);
	if (id == 0) return;

	m_current = imported;
	m_current.id = id;
	reload();
	emit preset_saved(id);
}
