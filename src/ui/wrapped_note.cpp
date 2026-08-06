#include "ui/wrapped_note.h"

#include <QLabel>
#include <QSizePolicy>

void bssh_wrap_note(QLabel *note) {
	if (!note) return;

	note->setWordWrap(true);

	/*
	 * The whole point of this function. QLabel implements
	 * heightForWidth() once it wraps, but a layout only asks when the
	 * widget's size policy says the height depends on the width -- and
	 * QLabel does not set that flag for you.
	 */
	QSizePolicy policy = note->sizePolicy();
	policy.setHeightForWidth(true);

	/*
	 * Vertically Minimum rather than Fixed: the height it asks for is a
	 * floor, and a note on a page with room to spare should keep it
	 * rather than be squeezed back to one line by a layout distributing
	 * space.
	 */
	policy.setVerticalPolicy(QSizePolicy::Minimum);
	note->setSizePolicy(policy);
}

QLabel *bssh_wrapped_note(const QString &text, QWidget *parent) {
	QLabel *note = new QLabel(text, parent);
	bssh_wrap_note(note);
	return note;
}
