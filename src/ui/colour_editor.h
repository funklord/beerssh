#ifndef BSSH_COLOUR_EDITOR_H
#define BSSH_COLOUR_EDITOR_H

#include <QDialog>

#include "term/colour_scheme.h"

class QAbstractButton;
class QToolButton;

/*
 * bssh_colour_editor -- the custom scheme's nineteen numbers
 * (project.md sec 9.3).
 *
 * The shipped schemes deliberately do not let anyone repaint the sixteen:
 * a program asking for red means red, and a palette that reassigns it
 * makes every other program's colour choices mean something else. This is
 * the escape hatch for the person who wants to anyway, and it is the ONLY
 * place in the app where those sixteen can be changed -- which is the
 * point. A choice that takes a deliberate trip to a dialog is a choice
 * somebody made; one offered in a dropdown beside the font is one they
 * made by accident.
 *
 * Shaped after PuTTY's Colours panel, which is the interface this problem
 * has already been solved in: every colour visible at once, named rather
 * than numbered, and the system colour picker for the actual choosing.
 * PuTTY spells them out as a list with RGB spinners; a grid of swatches
 * says the same thing in a quarter of the height and reads as a palette,
 * which is what it is.
 */
class bssh_colour_editor : public QDialog {
	Q_OBJECT

public:
	explicit bssh_colour_editor(const bssh_colour_scheme_t &scheme,
	                             QWidget *parent = nullptr);

	/* What the user settled on. Only meaningful after exec() accepted. */
	bssh_colour_scheme_t scheme() const { return m_scheme; }

private:
	QToolButton *add_swatch(QRgb *target, const QString &label, const QString &tip);
	void paint_swatch(QToolButton *swatch, QRgb colour);
	void pick(QRgb *target, QToolButton *swatch, const QString &label);
	void reset_to_classic();

	bssh_colour_scheme_t m_scheme;

	/*
	 * Kept so a reset can repaint them without rebuilding the dialog.
	 * Parallel to the colours they edit, in the order they were added.
	 */
	QList<QToolButton *> m_swatches;
	QList<QRgb *> m_targets;
};

#endif /* BSSH_COLOUR_EDITOR_H */
