#ifndef BSSH_APP_SETTINGS_VIEW_H
#define BSSH_APP_SETTINGS_VIEW_H

#include <QWidget>

class QCheckBox;
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

private:
	void commit();

	QComboBox *m_key_row;
	QComboBox *m_theme;
	QComboBox *m_context_menu;
	QCheckBox *m_raise_contrast;
};

#endif /* BSSH_APP_SETTINGS_VIEW_H */
