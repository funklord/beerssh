#include "ui/touch_metrics.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QWidget>

namespace {

bool aimed_at(const QWidget *widget) {
	if (qobject_cast<const QAbstractButton *>(widget)) return true;
	if (qobject_cast<const QLineEdit *>(widget)) return true;
	if (qobject_cast<const QComboBox *>(widget)) return true;
	if (qobject_cast<const QAbstractSpinBox *>(widget)) return true;

	return qobject_cast<const QKeySequenceEdit *>(widget) != nullptr;
}

} /* namespace */

void bssh_apply_touch_height(QWidget *widget, bool touch) {
	if (!widget || !touch) return;

	widget->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
}

void bssh_apply_touch_heights(QWidget *root, bool touch) {
	if (!root || !touch) return;

	for (QWidget *control : root->findChildren<QWidget *>()) {
		if (!aimed_at(control)) continue;

		/*
		 * Skip what lives inside another target. Growing a spin box's
		 * own line edit to the floor would grow the spin box past it,
		 * and the reader would be left wondering why one row on the
		 * form is taller than the rest.
		 */
		bool nested = false;
		for (QWidget *up = control->parentWidget(); up && up != root;
		      up = up->parentWidget()) {
			if (aimed_at(up)) {
				nested = true;
				break;
			}
		}

		if (!nested) control->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	}
}
