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
		 * version and it recursed until the stack ran out.
		 *
		 * THE FIRST EXPLANATION OF THAT WAS WRONG and is worth
		 * recording, because it stood in this comment for a day. It
		 * said Qt remembers which widget a scroll gesture belongs to,
		 * so a wheel handed to the viewport came back to the control
		 * it was taken from. Measured since, on a fixture: it does
		 * not. A wheel sent to the viewport reaches the viewport, the
		 * page scrolls, and the combo sees nothing.
		 *
		 * What actually recursed was this filter, through the SCROLL
		 * BARS. A QAbstractScrollArea's bars are children of an
		 * internal container widget rather than of the area itself, so
		 * the exclusion below -- which tested the parent -- never
		 * fired and the filter went on to the bars as well. Forwarding
		 * a notch to the viewport made QAbstractScrollArea send it to
		 * its vertical bar, which was filtered, which forwarded to the
		 * viewport again. Measured with a depth counter: combo, bar,
		 * bar, bar, until the stack ended.
		 *
		 * So the arithmetic is done here, in QAbstractSlider's own
		 * terms: wheelScrollLines() single steps per notch, capped at
		 * a page, which is what the bar itself would have done with
		 * the event. That is now belt as well as braces -- the
		 * exclusion below is fixed -- but a filter that cannot recurse
		 * by construction is worth more than one that relies on an
		 * exclusion being right.
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
		 * the wheel already means what it says: filtering it would
		 * take the event from a viewport's own bar and hand it back to
		 * the same viewport.
		 *
		 * Tested on the WIDGET, not on its parent. The first version
		 * asked whether the parent was a QAbstractScrollArea, which
		 * looks right and never fires -- Qt parents a scroll area's
		 * bars to an internal container widget, so every bar in the
		 * tree was being filtered. That is what made the forwarding
		 * version above recurse, and it went unnoticed because the
		 * replacement cannot recurse whatever it collects.
		 *
		 * There is no sabotage entry for this line, and the reason is
		 * the same as the XTGETTCAP one in term-features.toml: with
		 * the bar moved directly rather than forwarded, a filtered
		 * scroll bar and an unfiltered one behave identically. This
		 * arithmetic is QScrollBar's own. The guard is here because
		 * the hazard is latent rather than absent -- anything that
		 * makes this filter deliver an event again brings the
		 * recursion straight back.
		 */
		if (qobject_cast<QScrollBar *>(control)) continue;

		control->installEventFilter(filter);
	}
}
