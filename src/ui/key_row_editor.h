#ifndef BSSH_KEY_ROW_EDITOR_H
#define BSSH_KEY_ROW_EDITOR_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QWidget;

/*
 * bssh_key_row_editor -- composing your own on-screen row (project.md
 * sec 6.2, sec 6.3).
 *
 * sec 6.2 shipped five curated rows and left "letting a user compose their
 * own" open; sec 6.3 says that question is the same one for fonts, colour
 * schemes, key rows and keyboard layouts and "should be answered once, for
 * all four". The colour scheme answered it first -- one reserved entry in
 * the catalogue, written by a deliberate trip to a dialog -- and this is
 * that answer applied to the row.
 *
 * The row is edited AS WORDS rather than by dragging keys around, and that
 * is a decision rather than a shortcut. The spec is what app.ini stores
 * (sec 10.6 wants that file hand-editable), so an editor that produced
 * anything else would be a second representation to keep in step with the
 * first. Typing `esc tab ctrl alt left down up right' is also simply
 * faster than placing eight keys, and the word list below makes the
 * vocabulary discoverable without anybody reading the manual.
 *
 * What it will not do is accept a row that cannot drive a terminal. sec
 * 6.2's floor -- an Escape and a modifier -- is checked as you type and
 * OK stays disabled until it is met, because the alternative is a user
 * who empties their own key row and has no way back to a working one
 * except the settings file this dialog exists to avoid.
 */
class bssh_key_row_editor : public QDialog {
	Q_OBJECT

public:
	explicit bssh_key_row_editor(const QString &spec, QWidget *parent = nullptr);

	/* The spec the user settled on. Only meaningful after exec() accepted. */
	QString spec() const;

	/*
	 * Every word the note on this dialog names.
	 *
	 * Exposed so the suite can assert that each one parses, which is what
	 * keeps the explanation and sk_parse_key_row from drifting apart --
	 * a note promising a key this build does not know is a promise the
	 * row cannot keep, and nothing else would catch it.
	 */
	static QStringList vocabulary();

private:
	void refresh();

	QLineEdit *m_spec;
	QLabel *m_preview;
	QLabel *m_status;
	QDialogButtonBox *m_buttons;
};

#endif /* BSSH_KEY_ROW_EDITOR_H */
