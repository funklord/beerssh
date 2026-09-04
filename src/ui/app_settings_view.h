#ifndef BSSH_APP_SETTINGS_VIEW_H
#define BSSH_APP_SETTINGS_VIEW_H

#include <QWidget>

class QResizeEvent;

#include <functional>

#include "model/app_settings.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;
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
	explicit bssh_app_settings_view(QWidget *parent = nullptr);

signals:
	/* Something changed; whoever owns the key row should re-read it. */
	void changed();

	/* Touch layout only: the page is done and the caller should go back. */
	void back_requested();

protected:
	void resizeEvent(QResizeEvent *event) override;

private slots:
	void refresh_keyboard_fallback();

private:
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

	QComboBox *m_key_row;
	QComboBox *m_keyboard;
	QLabel *m_keyboard_fallback;
	QComboBox *m_keyboard_layout;
	QComboBox *m_theme;
	QComboBox *m_scrollbar;
	QComboBox *m_status_line;
	QLineEdit *m_window_title;
	QComboBox *m_context_menu;
	QCheckBox *m_caps_as_ctrl;
	QCheckBox *m_warn_multiline_paste;

	/* Null where there is no pointer to hover (sec 8.3). */
	QCheckBox *m_focus_follows_mouse;

	/* Null where the platform cannot hide the window (sec 11). */
	QCheckBox *m_hide_from_screenshots;
	QCheckBox *m_raise_contrast;
	QSlider *m_text_brightness;
	QPushButton *m_custom_colours;
	QLabel *m_text_brightness_value;
};

#endif /* BSSH_APP_SETTINGS_VIEW_H */
