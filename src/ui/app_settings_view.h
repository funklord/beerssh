#ifndef BSSH_APP_SETTINGS_VIEW_H
#define BSSH_APP_SETTINGS_VIEW_H

#include <QWidget>

class QResizeEvent;

#include <functional>

#include "model/app_settings.h"

class QCheckBox;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QLineEdit;
class QComboBox;

/*
 * bssh_app_settings_view -- the settings that belong to the device
 * (project.md sec 6.2).
 *
 * Small on purpose. Almost everything configurable in this app belongs to
 * a host's preset, where it can differ per host and travel between
 * machines; what lands here is only what is a property of the device in
 * your hand. Today that is one setting, and a page with one setting on it
 * is the honest shape rather than a reason to invent more.
 */
class bssh_app_settings_view : public QWidget {
	Q_OBJECT

public:
	/*
	 * Which part of the page to build (project.md sec 8.5).
	 *
	 * EVERYTHING is the DESKTOP's, and it is not a legacy mode: there the
	 * view is one category inside PuTTY's tree (session_dialog), so it
	 * already sits behind a level of navigation and nesting it again
	 * would be a tree inside a tree.
	 *
	 * Touch builds one page per section instead, reached from a list and
	 * left with Back. sec 8.5 rules out a category tree beside a form of
	 * small fields -- "a desktop idiom that survives neither a thumb nor
	 * a 5-inch screen" -- and this is the other thing: full-screen pages
	 * in sequence, which is what the platform's own settings do and what
	 * show_page()/go_back() was already built for.
	 *
	 * The page was 2038px tall at 320dp, against a 640px screen. Measured
	 * before it was split: 912px of controls, 532 of labels, 510 of
	 * explanatory notes -- so trimming the prose could never have been
	 * the answer, and sec 12.3b says why it must not be tried.
	 */
	enum section_t {
		EVERYTHING,

		/*
		 * The desktop's "This device" category: everything but the
		 * appearance, which the dialog's Appearance category shows --
		 * brightness was there to find and nobody looked under "This
		 * device" for it (reported 2026-10-10).
		 */
		EVERYTHING_BUT_APPEARANCE,
		SECTION_KEYBOARD,
		SECTION_APPEARANCE,
		SECTION_TERMINAL,
		SECTION_PASSWORDS,
		SECTION_SHORTCUTS,
	};

	/* The heading a section page shows, and the row that opens it. */
	static QString section_name(section_t section);

	/*
	 * Which section this page IS, so a caller can check that the row it
	 * tapped and the page it got are the same thing. The index maps rows
	 * to pages by position, and a list whose two halves are kept in step
	 * by hand is the shape sec 8.7 already records an off-by-one in.
	 */
	section_t section() const { return m_section; }

	/* The page's heading, where the section's own name is the wrong one. */
	void set_heading(const QString &heading);

	explicit bssh_app_settings_view(QWidget *parent = nullptr);
	bssh_app_settings_view(section_t section, QWidget *parent);

signals:
	/* Something changed; whoever owns the key row should re-read it. */
	void changed();

	/* Touch layout only: the page is done and the caller should go back. */
	void back_requested();

protected:
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;

private slots:
	void refresh_keyboard_fallback();

private:
	/*
	 * The stored-passwords rows, filled the first time the page is shown
	 * rather than when it is built: naming the store asks which one there
	 * is, which on a desktop probes for a keyring -- and this view is built
	 * with the main window, so the probe held the first window for as long
	 * as D-Bus took to give up (sec 11).
	 */
	void fill_store_notes();
	QLabel *m_store_name = nullptr;
	QLabel *m_store_detail = nullptr;
	bool m_store_filled = false;

	/*
	 * Writes ONE setting and tells the window.
	 *
	 * One field rather than the whole page, because there is a view per
	 * WINDOW and each snapshots app.ini into its widgets when it is
	 * built. A commit() that wrote every widget back made the older
	 * snapshot win: open two windows, set the theme in the second, then
	 * tick any box in the first, and the first window's key row,
	 * scrollbar, status line and window title -- as they stood when it
	 * opened -- were written over the ones just chosen. Nothing
	 * subscribes to app.ini, so neither page ever learns it happened.
	 *
	 * A write of the control the user actually touched cannot carry a
	 * stale field with it. Re-reading the widgets when the page is shown
	 * would not do: both windows' settings pages can be open at once.
	 */
	void commit(const std::function<void(bssh_app_settings &)> &write);

	/* Reads every remap slot's from/to edits into the stored list. */
	void save_key_remaps();

	/*
	 * Whether this row belongs on this page, hiding the widget when it
	 * does not. preset_editor.cpp's shown() lambda, which exists for the
	 * same reason: a widget left out of a form but still parented here
	 * would otherwise be drawn at the top-left corner.
	 *
	 * Skipping the addRow rather than hiding afterwards is what disposes
	 * of the LABEL too -- addRow(QString, QWidget) builds one internally,
	 * and hiding only the field would leave its caption behind.
	 */
	bool on_this_page(section_t section, QWidget *widget);


	section_t m_section;
	QLabel *m_heading = nullptr;

	/* The whole view rather than one section's page: the desktop's form. */
	bool whole() const { return m_section == EVERYTHING || m_section == EVERYTHING_BUT_APPEARANCE; }

	QComboBox *m_key_row;
	QComboBox *m_keyboard;
	QLabel *m_keyboard_fallback;
	QComboBox *m_keyboard_layout;
	QComboBox *m_theme;
	QComboBox *m_scrollbar;
	QComboBox *m_arrangement;
	QComboBox *m_tab_position = nullptr;
	QSpinBox *m_image_history = nullptr;
	QSlider *m_snap_radius;
	QLabel *m_snap_radius_value;
	QComboBox *m_status_line;
	QLineEdit *m_window_title;
	QComboBox *m_context_menu;
	QCheckBox *m_caps_as_ctrl;
	QCheckBox *m_keyboard_grips;
	QPushButton *m_keyboard_reset;
	QCheckBox *m_warn_multiline_paste;
	QCheckBox *m_keep_sessions_on_close;
	QCheckBox *m_offer_multiplexers;
	QCheckBox *m_honour_font_scale;
	QCheckBox *m_font_ligatures;
	QCheckBox *m_honour_remote_cursor;

	/*
	 * The general key-remap slots (sec 6.1), a from/to QKeySequenceEdit
	 * per fixed slot. Kept so save_key_remaps can read them all together:
	 * a slot is a pair, and one half changing rewrites the whole list.
	 */
	QList<QKeySequenceEdit *> m_remap_from;
	QList<QKeySequenceEdit *> m_remap_to;

	/* Null where there is no pointer to hover (sec 8.3). */
	QCheckBox *m_focus_follows_mouse;

	/* Null where the platform cannot hide the window (sec 11). */
	QCheckBox *m_hide_from_screenshots;
	QCheckBox *m_raise_contrast;
	QSlider *m_text_brightness;
	QPushButton *m_custom_colours;
	QPushButton *m_custom_key_row;
	QLabel *m_text_brightness_value;
};

#endif /* BSSH_APP_SETTINGS_VIEW_H */
