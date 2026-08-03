#ifndef BSSH_LICENCES_VIEW_H
#define BSSH_LICENCES_VIEW_H

#include <QWidget>

class QListWidget;
class QTextBrowser;

/*
 * bssh_licences_view -- who else's work is in here (project.md sec 9.4).
 *
 * sec 9.4 asks for this in as many words: "Keep each font's licence file in
 * the bundle and surface an Open Source Licences screen in settings (also
 * good Play hygiene, and required in spirit by the OFL/Apache terms on
 * every platform)." Bundling the fonts created the obligation; the licence
 * files have been fetched and packaged into the APK since, and until now
 * nothing displayed them.
 *
 * It lists three kinds of thing, because this app carries three:
 *
 *   the bundled fonts (sec 9.2), whose full licence text ships beside them
 *   and is shown verbatim -- the OFL asks for the text, not a summary;
 *
 *   the colour schemes (sec 10.2), which are other people's designs even
 *   though they are only sixteen numbers each;
 *
 *   the libraries, including the one that needs a sentence rather than a
 *   name: libssh is LGPL, which is why the desktop build links it
 *   dynamically (sec 4).
 *
 * One widget for both platforms. A dialog on the desktop and a page on
 * Android, but the same contents -- an attribution that differed by device
 * would be an attribution that is wrong on one of them.
 */
class bssh_licences_view : public QWidget {
	Q_OBJECT

public:
	explicit bssh_licences_view(QWidget *parent = nullptr);

signals:
	/* Touch layout only: the page is done and the caller should go back. */
	void back_requested();

private:
	void add_entry(const QString &title, const QString &subtitle, const QString &body);
	void load_fonts();
	void load_schemes();
	void load_libraries();

	/* The licence text that ships beside the fonts, or empty. */
	QString licence_text(const QString &font_id) const;

	QListWidget *m_list;
	QTextBrowser *m_text;
};

#endif /* BSSH_LICENCES_VIEW_H */
