#include "ui/touch_metrics.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QStyledItemDelegate>
#include <QWidget>

namespace {

/*
 * Gives every row of a list the floor.
 *
 * A delegate rather than a `QListView::item { min-height }` stylesheet,
 * because a list may already carry a stylesheet of its own -- the host
 * list draws its row separators with one -- and setting a second would
 * take the first away.
 *
 * QStyledItemDelegate::sizeHint returns the item's own Qt::SizeHintRole
 * where it has one, so this raises the rows that never asked for a height
 * and leaves sec 8.5's deliberately taller 64dp host rows alone.
 */
class touch_row_delegate : public QStyledItemDelegate {
public:
	explicit touch_row_delegate(QObject *parent) : QStyledItemDelegate(parent) {}

	QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override {
		QSize size = QStyledItemDelegate::sizeHint(o, i);
		size.setHeight(qMax(size.height(), BSSH_TOUCH_TARGET_DP));

		return size;
	}
};

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

	/*
	 * The lists first, including the root itself when it IS one: their
	 * rows are tapped and are not widgets, so the walk below cannot see
	 * them.
	 */
	QList<QAbstractItemView *> views = root->findChildren<QAbstractItemView *>();
	if (QAbstractItemView *self = qobject_cast<QAbstractItemView *>(root)) {
		views.append(self);
	}

	/*
	 * And the popup a combo box drops down, which is a list whose rows
	 * are tapped exactly like any other -- they stood 25dp.
	 *
	 * Asked for by name rather than found by the walk, because
	 * QComboBox builds its view lazily: at the moment a form is
	 * constructed the popup does not exist yet, so findChildren returns
	 * nothing and the rows keep the style's default. view() creates it.
	 */
	for (QComboBox *combo : root->findChildren<QComboBox *>()) {
		if (combo->view()) views.append(combo->view());
	}

	for (QAbstractItemView *view : views) {
		view->setItemDelegate(new touch_row_delegate(view));
	}

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
