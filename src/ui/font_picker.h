#ifndef BSSH_FONT_PICKER_H
#define BSSH_FONT_PICKER_H

#include <QList>
#include <QWidget>

#include <memory>

class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class bssh_font_download;
class bssh_range_source;
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
 * the whole point of sec 9.4's rule: on Android most of the set is
 * downloaded, and a picker that hides what has not been downloaded is a
 * picker whose contents depend on the platform after all.
 */
class bssh_font_picker : public QWidget {
	Q_OBJECT

public:
	explicit bssh_font_picker(QWidget *parent = nullptr);
	~bssh_font_picker() override;

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
	 * Line this picker's label column up with the form it is embedded in.
	 *
	 * A form layout sizes its label column from its OWN widest label, and
	 * two form layouts know nothing about each other -- so a picker
	 * dropped below a form starts a second column of its own. Measured on
	 * the preset editor: the rows above put their fields at x=72 and the
	 * picker put its at x=50, which reads as the page losing its
	 * alignment half way down.
	 *
	 * Widening is mutual and takes the larger of the two, so neither side
	 * has to know which one is wider. A picker used on its own -- the
	 * Window page of the desktop dialog -- is left exactly as it was,
	 * since nothing calls this.
	 */
	static void align_labels(QFormLayout *host, bssh_font_picker *picker);

	int label_column_width() const;
	void set_label_column_width(int width);

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

protected:
	/*
	 * The swatch strip is drawn to the width it HAS, so a resize has to
	 * redraw it. See the strip's own comment in the .cpp: it used to be
	 * a fixed 16*18 pixmap, which is exactly the 288px minimum that put
	 * this picker -- and both editors that embed it -- off the edge of a
	 * 320dp screen.
	 */
	void resizeEvent(QResizeEvent *event) override;
	QList<QWidget *> label_widgets() const;

	QFormLayout *m_form;

	QComboBox *m_family;
	QComboBox *m_scheme;
	QSpinBox *m_size;
	QLabel *m_preview;
	QLabel *m_swatches;
	QLabel *m_note;

	/*
	 * Fetching a font of the set that is not on this device (sec 9.4):
	 * offered under the note while one is chosen, and running at most one
	 * download at a time. The source and the download live as long as it
	 * runs.
	 */
	QPushButton *m_download = nullptr;
	QPushButton *m_import = nullptr;

	/* Picks a font file and makes it the custom entry (sec 9.4). */
	void import_font();

	/* The custom entry's row, added or relabelled; its index. */
	int custom_row();
	std::unique_ptr<bssh_range_source> m_download_source;
	bssh_font_download *m_running = nullptr;
	void update_download_offer();
	void start_download();
};

#endif /* BSSH_FONT_PICKER_H */
