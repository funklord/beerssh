#ifndef BSSH_FONT_PICKER_H
#define BSSH_FONT_PICKER_H

#include <QWidget>

class QComboBox;
class QLabel;
class QSpinBox;

/*
 * bssh_font_picker -- choosing a terminal font (project.md sec 9.4).
 *
 * One widget for both platforms, because sec 9.4 requires one LIST for both
 * platforms: "the picker's contents must not depend on the platform", so
 * that a font chosen on a phone still resolves when the same profile opens
 * on a desktop. Two pickers would be two chances for those lists to drift.
 *
 * What differs between platforms is where it is embedded -- the Window
 * page of the PuTTY-shaped session dialog on desktop, the host page on
 * Android -- and the touch target sizes it asks for, not what it offers.
 *
 * Fonts that are not installed are LISTED, marked, and selectable. That is
 * the whole point of sec 9.4's rule: on Android most of the set arrives in
 * an asset pack, and a picker that hides what has not been downloaded is a
 * picker whose contents depend on the platform after all.
 */
class bssh_font_picker : public QWidget {
	Q_OBJECT

public:
	explicit bssh_font_picker(QWidget *parent = nullptr);

	/* The catalog id, never a display name -- see bssh_font_catalog. */
	QString font_id() const;
	int point_size() const;

	/*
	 * An empty id selects the platform's default rather than nothing,
	 * matching what an unset preset field means everywhere else.
	 */
	void set_font_choice(const QString &id, int point_size);

	/* Bigger controls for touch (sec 6.2's 48dp floor). */
	void set_touch_sized(bool touch);

	/*
	 * The colour scheme lives here too, because it and the font are the
	 * same question asked twice -- "what does this host look like" -- and
	 * splitting them across two widgets would mean two previews, neither
	 * showing what the terminal will actually look like.
	 */
	QString scheme_id() const;
	void set_scheme(const QString &id);

signals:
	void font_chosen(const QString &id, int point_size);
	void scheme_chosen(const QString &id);

private slots:
	void on_choice_changed();

private:
	void refresh_preview();

	QComboBox *m_family;
	QComboBox *m_scheme;
	QSpinBox *m_size;
	QLabel *m_preview;
	QLabel *m_swatches;
	QLabel *m_note;
};

#endif /* BSSH_FONT_PICKER_H */
