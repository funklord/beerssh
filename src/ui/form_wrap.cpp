#include "ui/form_wrap.h"

#include <QEvent>
#include <QFormLayout>
#include <QWidget>

#include "ui/layout_manager.h"

namespace {

/*
 * Re-asks the question on every resize of the page the form is on.
 *
 * A filter parented to that page rather than a member of either editor,
 * for the reason the header gives.
 */
class form_wrap_watcher : public QObject {
public:
	form_wrap_watcher(QFormLayout *form, QWidget *page)
	    : QObject(page), m_form(form), m_page(page) {
		page->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == m_page && event->type() == QEvent::Resize)
			bssh_apply_form_wrap(m_form, m_page);

		return QObject::eventFilter(watched, event);
	}

private:
	QFormLayout *m_form;
	QWidget *m_page;
};

} /* namespace */

bool bssh_form_stacks(int width_dp) {
	return bssh_layout_manager::size_class_for(width_dp)
	        == bssh_layout_manager::COMPACT;
}

void bssh_apply_form_wrap(QFormLayout *form, const QWidget *page) {
	if (!form || !page) return;

	/*
	 * Qt's logical pixels already ARE device-independent pixels, which is
	 * what sec 8.2's thresholds are written in -- the main window's
	 * resizeEvent records what converting a second time cost.
	 */
	const QFormLayout::RowWrapPolicy wanted =
	        bssh_form_stacks(page->width()) ? QFormLayout::WrapAllRows
	                                        : QFormLayout::DontWrapRows;

	/*
	 * Only on a change. setRowWrapPolicy invalidates the layout, and this
	 * runs from a resize -- setting it unconditionally is a full relayout
	 * for every pixel a window is dragged.
	 */
	if (form->rowWrapPolicy() != wanted) form->setRowWrapPolicy(wanted);
}

void bssh_watch_form_width(QFormLayout *form, QWidget *page) {
	if (!form || !page) return;

	/*
	 * Named so the suite can ASK which forms key off width rather than
	 * carrying a list of them: a test whose population is hand-written is
	 * one that keeps passing about the forms somebody remembered.
	 */
	form->setObjectName(BSSH_WIDTH_KEYED_FORM);

	new form_wrap_watcher(form, page);

	/* And once now, so the first paint is not a layout nobody decided. */
	bssh_apply_form_wrap(form, page);
}
