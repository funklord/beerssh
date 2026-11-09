#include "ui/touch_metrics.h"
#include "ui/preset_editor.h"

#include "model/settings_resolver.h"

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
#include "ui/terminal_fields.h"

namespace {

/*
 * The state, and the ellipsis that says it opens something. Every other
 * button on this page that leads to a dialog carries one -- Import...,
 * Export... -- and without it a button reading "All on" is a status
 * label that happens to be clickable.
 *
 * The ellipsis is added HERE rather than inside summary(), which stays a
 * plain statement of the state for anybody else who wants to say it.
 */

/*
 * What did not happen, then the store's reason. Every one of these
 * returned in silence once, so a refused Save, Clone, Delete or Import
 * looked exactly like one that worked -- the host editor's "Not saved"
 * (sec 10.6), met here in the preset editor, which had none.
 */
void warn_not_written(QWidget *parent, const QString &title,
                      const QString &what, const QString &why) {
	QMessageBox::warning(parent, title,
	                      why.isEmpty() ? what : what + QStringLiteral("\n\n") + why);
}

} /* namespace */
#include "ui/form_wrap.h"
#include "softkeys/key_row_layout.h"

/*
 * Scrollback is in lines and the range is deliberately wide: 200 is a
 * phone with little memory to spare, 200000 is a workstation watching a
 * build. The cost of the upper end is memory per session (sec 7), which
 * is why it is a preset field rather than a constant.
 */

bssh_preset_editor::bssh_preset_editor(bssh_profile_store *store, layout_t layout,
                                        QWidget *parent)
    : QWidget(parent),
      m_store(store),
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

	/*
	 * The terminal and connection fields are bssh_terminal_fields, the
	 * one form every place that sets them uses -- the defaults for every
	 * host, a host's own, and here (the holder, 2026-11-02). The name is
	 * the preset's alone and goes first in that form, so the labels line
	 * up in one column.
	 */
	m_name = new QLineEdit(this);
	m_fields = new bssh_terminal_fields(content);
	QFormLayout *form = m_fields->form();
	form->insertRow(0, QStringLiteral("Name"), m_name);
	fields->addWidget(m_fields);

	/*
	 * Where the room is short the label goes ABOVE its field, for the
	 * reason ui/form_wrap.h gives and with the measurement this page
	 * supplied.
	 *
	 * Three of these fields hold text of a length nobody chose -- Name,
	 * TERM and On connect -- and this is the NARROWEST page in the app:
	 * its scroll area leaves 284px on a phone where the settings page and
	 * the host editor get 306. Two columns took 72 of that for the label,
	 * leaving 212, and an entirely ordinary
	 *
	 *     tmux attach -t main || tmux new -s main
	 *
	 * in On connect needs 230. So the field a person is most likely to
	 * paste a long string into was the one with the least room.
	 *
	 * WrapAllRows rather than WrapLongRows: the latter wraps a row whose
	 * MINIMUM will not fit, and a line edit willing to scroll its own
	 * text has a small minimum, so it never fires.
	 *
	 * The width asked about is the PAGE's, not this column's -- the
	 * desktop layout puts a 240px preset list beside these fields, and
	 * measuring the column left would make a dialog that is not narrow
	 * answer as though it were. sec 8.2's classes describe the window.
	 */
	bssh_watch_form_width(form, this);

	m_font = new bssh_font_picker(content);
	fields->addWidget(m_font);

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
		 * A page with no way out is the one bug a phone punishes hardest.
		 * The system Back leaves this page too (bssh_main_window::go_back),
		 * but a gesture is not something a person can see, and this is the
		 * way out that is on the screen.
		 */
		/* "Back", as every other page says: it returns to the page it
		 * came from -- the settings -- and "Back to hosts" said otherwise. */
		QPushButton *back = new QPushButton(QStringLiteral("Back"), this);
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
	m_fields->set_source_name(QStringLiteral("this device's default"));
	m_fields->show_settings(bssh_settings_resolver::app_defaults(), preset.chosen_json());
	m_font->set_font_choice(preset.font_family, preset.font_size);
	m_font->set_scheme(preset.colour_scheme);

	update_controls();
}

bssh_preset_t bssh_preset_editor::edited_preset() const {
	/*
	 * Starts from the loaded preset, so fields this form does not show --
	 * colour_scheme and key_row_layout, which arrive with their own UI --
	 * are not silently cleared by saving. The session dialog lost host
	 * settings exactly this way.
	 */
	/*
	 * The fields that differ from the loaded preset, laid over it: the
	 * same rule as a host's own settings -- a value only agreeing with
	 * what it would have been anyway is not recorded, so an untouched key
	 * row stays unset and follows the default row.
	 */
	QJsonObject json = m_current.to_json();
	const QJsonObject built_in = bssh_preset_t().to_json();
	const QJsonObject own = m_fields->chosen_values();
	for (const QString &key : bssh_terminal_fields::keys())
		json.insert(key, own.contains(key) ? own.value(key) : built_in.value(key));
	bssh_preset_t preset = bssh_preset_t::from_json(json);
	preset.id = m_current.id;
	preset.is_builtin = m_current.is_builtin;

	/* The form's choices, and what it does not show as they were: never every field. */
	preset.chosen = m_current.chosen;
	for (const QString &key : bssh_terminal_fields::keys()) preset.chosen.remove(key);
	preset.chosen.unite(m_fields->chosen());

	preset.name = m_name->text().trimmed();
	/*
	 * An untouched font stays UNSET rather than becoming whatever the
	 * picker resolved it to. The picker cannot show "unset" -- an empty
	 * value means "the default" (sec 9.2), so it displays that default --
	 * and writing the resolved value back would pin it: the preset would
	 * then carry jetbrains-mono as a choice, and stop following the
	 * default if the default ever moved.
	 */
	const QString resolved = bssh_font_catalog::instance().effective_id(m_current.font_family);
	if (m_font->font_id() != resolved) {
		preset.font_family = m_font->font_id();
		preset.chosen.insert(QStringLiteral("font_family"));
	}
	if (m_font->point_size() != m_current.font_size) {
		preset.font_size = m_font->point_size();
		preset.chosen.insert(QStringLiteral("font_size"));
	}

	/* Same rule for the scheme: an untouched default stays unset. */
	const bssh_colour_scheme_catalog &schemes = bssh_colour_scheme_catalog::instance();
	if (m_font->scheme_id() != schemes.effective_id(m_current.colour_scheme)) {
		preset.colour_scheme = m_font->scheme_id();
		preset.chosen.insert(QStringLiteral("colour_scheme"));
	}

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
	if (id == 0) {
		warn_not_written(this, QStringLiteral("Not saved"),
		                 QStringLiteral("This preset was not saved."),
		                 m_store->last_error());
		return;
	}

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
	if (id == 0) {
		warn_not_written(this, QStringLiteral("Not cloned"),
		                 QStringLiteral("The copy was not saved."),
		                 m_store->last_error());
		return;
	}

	m_current = clone;
	m_current.id = id;
	reload();
	emit preset_saved(id);
}

void bssh_preset_editor::on_delete() {
	if (!m_store || m_current.id == 0) return;

	const int id = m_current.id;
	if (!m_store->remove_preset(id)) {
		warn_not_written(this, QStringLiteral("Not deleted"),
		                 QStringLiteral("\"%1\" was not deleted.").arg(m_current.name),
		                 m_store->last_error());
		return;
	}

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
		 * A wildcard, because a preset has no MIME type Android knows --
		 * the picker calls an .ini a "BIN file" -- and read_ini checks the
		 * content anyway. Qt sends ACTION_OPEN_DOCUMENT with the wildcard
		 * type and category OPENABLE.
		 *
		 * On the Android 11 emulator image this listed preset files and
		 * would not let one be chosen, whatever the filter. On the API 33
		 * image with Qt 6.12, 2026-09-29, the same intent lists the file
		 * as choosable and imports it (sec 10.5). Android 11 was not run
		 * again, so which of the two changed is not known.
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
	QString why;
	bssh_preset_t imported = bssh_preset_t::read_ini(path, &ok, &why);

	if (!ok) {
		QMessageBox::warning(this, QStringLiteral("Import failed"),
		                      why.isEmpty()
		                              ? QStringLiteral("%1 does not look like a preset file.")
		                                        .arg(path)
		                              : QStringLiteral("%1 could not be imported.\n\n%2")
		                                        .arg(path, why));
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
	if (id == 0) {
		warn_not_written(this, QStringLiteral("Not imported"),
		                 QStringLiteral("%1 was read, but could not be added to "
		                                 "your presets.").arg(path),
		                 m_store->last_error());
		return;
	}

	m_current = imported;
	m_current.id = id;
	reload();
	emit preset_saved(id);
}
