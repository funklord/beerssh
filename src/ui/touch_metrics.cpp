#include "ui/touch_metrics.h"

#include <QFont>
#include <QMenu>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QSlider>
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

} /* namespace */

bool bssh_is_touch_target(const QWidget *widget) {
	if (qobject_cast<const QAbstractButton *>(widget)) return true;
	if (qobject_cast<const QLineEdit *>(widget)) return true;
	if (qobject_cast<const QComboBox *>(widget)) return true;
	if (qobject_cast<const QAbstractSpinBox *>(widget)) return true;
	if (qobject_cast<const QKeySequenceEdit *>(widget)) return true;

	/*
	 * A slider is DRAGGED, which is a harder thing to do than a tap and
	 * therefore not a smaller target. It was missing, and the brightness
	 * slider that exposed it stood 33dp on a phone -- the same "a widget
	 * left off the list is short" this walk was written to end, arriving
	 * through the list of TYPES rather than the list of widgets.
	 */
	return qobject_cast<const QSlider *>(widget) != nullptr;
}

void bssh_apply_touch_height(QWidget *widget, bool touch) {
	if (!widget || !touch) return;

	widget->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
}

void bssh_apply_touch_menu(QMenu *menu, bool touch) {
	if (!menu || !touch) return;

	/*
	 * A stylesheet rather than a walk, because a menu's items are
	 * QActions and not widgets -- there is nothing for findChildren to
	 * find.
	 *
	 * The ROW is the 48dp target: min-height plus the padding above and
	 * below. It was min-height 48 with the padding on top, 64dp rows
	 * around ordinary text, which the holder saw as tiny text in a lot of
	 * empty space and a menu likely to run off the screen (2026-10-22).
	 * The text is made a little larger with the same change, so the row
	 * reads as full rather than as padding.
	 */
	/* Once per menu: applied again, it would grow the text again. */
	if (!menu->property("bssh_touch_font").toBool()) {
		QFont larger = menu->font();
		larger.setPointSizeF(larger.pointSizeF() * 1.15);
		menu->setFont(larger);
		menu->setProperty("bssh_touch_font", true);
	}
	menu->setStyleSheet(QStringLiteral("QMenu::item { min-height: %1px; padding: 8px 24px; }")
	                            .arg(BSSH_TOUCH_TARGET_DP - 16));
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
		if (!bssh_is_touch_target(control)) continue;

		/*
		 * Skip what lives inside another target. Growing a spin box's
		 * own line edit to the floor would grow the spin box past it,
		 * and the reader would be left wondering why one row on the
		 * form is taller than the rest.
		 */
		bool nested = false;
		for (QWidget *up = control->parentWidget(); up && up != root;
		      up = up->parentWidget()) {
			if (bssh_is_touch_target(up)) {
				nested = true;
				break;
			}
		}

		if (!nested) control->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	}
}
