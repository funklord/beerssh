#include "ui/wrapped_note.h"

#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLayout>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {

/* What marks a label as a note, so a form can find its notes again. */
const char *const BSSH_NOTE_PROPERTY = "bssh_note";

/* The gap between a control and the note under it: close enough to read as
 * one thing, apart enough not to touch. */
const int BSSH_NOTE_GAP = 2;

bool is_note(const QWidget *widget) {
	return widget && widget->property(BSSH_NOTE_PROPERTY).toBool();
}

/*
 * A control and its note as one row. Shown while either of them is: a form
 * gives a hidden row no room, and controls here are hidden per platform or
 * until they have something to say, so a pair of two hidden widgets must
 * not leave a blank row and its spacing behind.
 */
class note_pair : public QWidget {
public:
	/* `control` is a widget or, for a row built as a layout -- a field and
	 * its Browse button -- that layout. */
	note_pair(QLayoutItem *control, QWidget *note, QWidget *parent) : QWidget(parent) {
		QVBoxLayout *column = new QVBoxLayout(this);
		column->setContentsMargins(0, 0, 0, 0);
		column->setSpacing(BSSH_NOTE_GAP);
		/*
		 * The control at the width it had: a combo box is its own size
		 * and stays it, a text field that grew across the form still
		 * does. The note takes the whole width under it.
		 */
		const bool grows = control->expandingDirections() & Qt::Horizontal;
		if (QWidget *widget = control->widget()) {
			column->addWidget(widget, 0, grows ? Qt::Alignment() : Qt::AlignLeft);
			m_parts.append(widget);
		} else if (QLayout *layout = control->layout()) {
			layout->setParent(nullptr);
			column->addLayout(layout);
		}
		column->addWidget(note);
		m_parts.append(note);
		for (QWidget *part : m_parts) part->installEventFilter(this);

		/*
		 * Its height depends on its width, through the note's, and a form
		 * asks only a widget whose policy says so -- the rule
		 * bssh_wrap_note() exists for, one level up. Without it the notes
		 * beside a combo box were given one line and clipped.
		 */
		QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Minimum);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
		follow();
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) follow();
		return QWidget::eventFilter(watched, event);
	}

private:
	void follow() {
		bool any = false;
		for (QWidget *part : m_parts) any = any || !part->isHidden();
		/* Never shown with no parent: that would be a window of its own. */
		if (!any) hide();
		else if (isHidden() && parentWidget()) show();
	}

	QList<QWidget *> m_parts;
};

/* A layout item's own deletion: a widget comes wrapped in an item that is
 * the layout's to free, while a layout IS its item and lives on. */
void drop_item(QLayoutItem *item) {
	if (item && !item->layout()) delete item;
}

/* Not onto a note, nor onto a control that already has its own: a second
 * note under a pair would read as the first's. */
bool takes_a_note(QLayoutItem *item) {
	if (!item) return false;
	if (QWidget *widget = item->widget())
		return !is_note(widget) && !dynamic_cast<note_pair *>(widget);
	return item->layout() != nullptr;
}

void attach_in_form(QFormLayout *form) {
	for (int row = 1; row < form->rowCount(); ++row) {
		QLayoutItem *span = form->itemAt(row, QFormLayout::SpanningRole);
		QWidget *note = span ? span->widget() : nullptr;
		if (!is_note(note)) continue;

		/* The control above: a row's field, or a row that spans. */
		QLayoutItem *above = form->itemAt(row - 1, QFormLayout::FieldRole);
		if (!above) above = form->itemAt(row - 1, QFormLayout::SpanningRole);
		if (!takes_a_note(above)) continue;

		QWidget *parent = form->parentWidget();
		const QFormLayout::TakeRowResult taken_note = form->takeRow(row);
		const QFormLayout::TakeRowResult taken_control = form->takeRow(row - 1);
		QWidget *label = taken_control.labelItem ? taken_control.labelItem->widget() : nullptr;

		QWidget *control = taken_control.fieldItem->widget();
		QWidget *pair = new note_pair(taken_control.fieldItem, note, parent);
		drop_item(taken_note.fieldItem);
		drop_item(taken_note.labelItem);
		drop_item(taken_control.fieldItem);
		drop_item(taken_control.labelItem);

		if (label) form->insertRow(row - 1, label, pair);
		else form->insertRow(row - 1, pair);

		/*
		 * The label names the CONTROL, as it did. Qt reads a field's name
		 * off a label beside it -- a sibling whose buddy it is -- and
		 * inside the pair the control is no longer the label's sibling,
		 * so the spin box and the field with a note under them became
		 * unnamed to a screen reader. Named outright, from the label.
		 */
		if (QLabel *names = qobject_cast<QLabel *>(label)) {
			if (control) {
				names->setBuddy(control);
				if (control->accessibleName().isEmpty())
					control->setAccessibleName(names->text().remove(QLatin1Char('&')));
			}
		}
		--row;
	}
}

void attach_in_column(QBoxLayout *column) {
	for (int at = 1; at < column->count(); ++at) {
		QWidget *note = column->itemAt(at)->widget();
		if (!is_note(note) || !takes_a_note(column->itemAt(at - 1))) continue;

		QWidget *parent = column->parentWidget();
		QLayoutItem *taken_note = column->takeAt(at);
		QLayoutItem *taken_control = column->takeAt(at - 1);
		QWidget *pair = new note_pair(taken_control, note, parent);
		drop_item(taken_note);
		drop_item(taken_control);
		column->insertWidget(at - 1, pair);
		--at;
	}
}

} /* namespace */

void bssh_wrap_note(QLabel *note) {
	if (!note) return;

	note->setWordWrap(true);
	note->setProperty(BSSH_NOTE_PROPERTY, true);

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

void bssh_attach_notes(QLayout *layout) {
	if (QFormLayout *form = qobject_cast<QFormLayout *>(layout)) {
		attach_in_form(form);
		return;
	}
	QBoxLayout *column = qobject_cast<QBoxLayout *>(layout);
	if (column && (column->direction() == QBoxLayout::TopToBottom
	               || column->direction() == QBoxLayout::BottomToTop))
		attach_in_column(column);
}

QLabel *bssh_wrapped_note(const QString &text, QWidget *parent) {
	QLabel *note = new QLabel(text, parent);
	bssh_wrap_note(note);
	return note;
}
