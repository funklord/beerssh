#include "ui/scroll_guard.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QWidget>

namespace {

/*
 * One filter object for a whole page, parented to the root, so the
 * lifetime question answers itself: the controls cannot outlive the form
 * that holds them.
 */
class wheel_to_the_page : public QObject {
public:
	explicit wheel_to_the_page(QWidget *root) : QObject(root) {}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() != QEvent::Wheel) {
			return QObject::eventFilter(watched, event);
		}

		QWidget *control = qobject_cast<QWidget *>(watched);
		if (!control) return QObject::eventFilter(watched, event);

		/*
		 * The page is scrolled by MOVING ITS BAR, not by re-sending the
		 * event somewhere better. Forwarding was the obvious first
		 * version and it recursed until the stack ran out: Qt
		 * remembers which widget a scroll gesture belongs to, so a
		 * wheel event handed to the viewport comes straight back to
		 * the control it was taken from, into this filter again.
		 * Measured on a fixture -- combo box, scroll area, forty rows
		 * -- and it segfaulted on the first notch.
		 *
		 * So the arithmetic is done here, in QAbstractSlider's own
		 * terms: wheelScrollLines() single steps per notch, capped at
		 * a page, which is what the bar itself would have done with
		 * the event.
		 */
		QWheelEvent *wheel = static_cast<QWheelEvent *>(event);

		for (QWidget *up = control->parentWidget(); up; up = up->parentWidget()) {
			QAbstractScrollArea *area = qobject_cast<QAbstractScrollArea *>(up);
			if (!area) continue;

			const bool sideways = wheel->angleDelta().y() == 0;
			QScrollBar *bar = sideways ? area->horizontalScrollBar()
			                            : area->verticalScrollBar();
			if (!bar) return true;

			const int delta = sideways ? wheel->angleDelta().x()
			                            : wheel->angleDelta().y();

			int step = QApplication::wheelScrollLines() * bar->singleStep();
			if (bar->pageStep() > 0) step = qMin(step, bar->pageStep());

			bar->setValue(bar->value() - delta * step / 120);
			return true;
		}

		return QObject::eventFilter(watched, event);
	}
};

bool steals_the_wheel(const QWidget *widget) {
	return qobject_cast<const QComboBox *>(widget)
	        || qobject_cast<const QAbstractSpinBox *>(widget)
	        || qobject_cast<const QAbstractSlider *>(widget);
}

} /* namespace */

void bssh_dont_steal_the_wheel(QWidget *root) {
	if (!root) return;

	wheel_to_the_page *filter = new wheel_to_the_page(root);

	for (QWidget *control : root->findChildren<QWidget *>()) {
		if (!steals_the_wheel(control)) continue;

		/*
		 * A scroll BAR is a QAbstractSlider, and it is the one place
		 * the wheel already means what it says. Filtering it would
		 * take the event from a viewport's own bar and hand it back to
		 * the same viewport.
		 */
		if (control->parentWidget()
		    && qobject_cast<QAbstractScrollArea *>(control->parentWidget())) {
			continue;
		}

		control->installEventFilter(filter);
	}
}
