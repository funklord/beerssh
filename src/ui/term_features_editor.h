#ifndef BSSH_TERM_FEATURES_EDITOR_H
#define BSSH_TERM_FEATURES_EDITOR_H

#include <QDialog>
#include <QList>

#include "term/term_features.h"

class QCheckBox;

/*
 * bssh_term_features_editor -- which extensions a host's terminal speaks
 * (project.md sec 7.1).
 *
 * sec 7.1 says the spec string "is a preset field, so a host that always
 * wants a reduced terminal says it once" -- and until this existed there
 * was nowhere to say it. The field was stored, validated, loaded and
 * applied to the session, and the only way to set one was to edit
 * app.ini by hand. Every other field on a preset has had a control since
 * the editor was written; this was the one that did not.
 *
 * Behind a button rather than on the page, for the reason the colour
 * editor is: fourteen checkboxes are a page of their own, and this is
 * the rarer choice of the two. sec 7.1's own words for what it is --
 * "not a preference knob: it is a test instrument" -- are why it sits a
 * deliberate trip away rather than beside the font.
 *
 * The list is GENERATED from the feature table, so a feature added to
 * sec 7.1 appears here without anybody editing this file. The same
 * argument the manual page and the completion are generated under: a
 * second copy of the list is a copy that goes stale, and here it would
 * go stale as a checkbox that silently governs nothing.
 */
class bssh_term_features_editor : public QDialog {
	Q_OBJECT

public:
	/*
	 * Takes the stored spec rather than a features object, because the
	 * spec is what a preset holds -- and an unreadable one is a real
	 * state (somebody hand-edited it) that has to open at something
	 * rather than refuse.
	 */
	explicit bssh_term_features_editor(const QString &spec, QWidget *parent = nullptr);

	/*
	 * What the user settled on, in the spec grammar, empty when nothing
	 * is turned off. Only meaningful after exec() accepted.
	 */
	QString spec() const;

	/*
	 * How a caller summarises the setting on the button that opens this
	 * -- "All on", or "2 off". Here rather than in the caller because
	 * the caller would have to parse the spec to say it, and a second
	 * reading of the grammar is a second thing to be wrong.
	 */
	static QString summary(const QString &spec);

private:
	QList<QCheckBox *> m_boxes;
};

#endif /* BSSH_TERM_FEATURES_EDITOR_H */
