#include "ui/find_bar.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

#include "ui/touch_metrics.h"

bssh_find_bar::bssh_find_bar(QWidget *parent)
    : QWidget(parent), m_field(nullptr), m_status(nullptr) {
	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(4, 2, 4, 2);
	row->setSpacing(4);

	m_field = new QLineEdit(this);
	m_field->setObjectName(QStringLiteral("find_field"));
	m_field->setPlaceholderText(QStringLiteral("Find"));
	bssh_apply_touch_height(m_field, false);
	row->addWidget(m_field, 1);

	m_status = new QLabel(this);
	m_status->setObjectName(QStringLiteral("find_status"));
	row->addWidget(m_status, 0);

	/*
	 * Previous and next as buttons a thumb can reach, since the keyboard
	 * step (Enter for next) is a desktop convenience the phone does not
	 * have. Short glyphs rather than words so all three fit a narrow bar.
	 */
	QToolButton *prev = new QToolButton(this);
	prev->setObjectName(QStringLiteral("find_prev"));
	prev->setText(QStringLiteral("↑"));
	prev->setAccessibleName(QStringLiteral("Previous match"));
	bssh_apply_touch_height(prev, true);
	row->addWidget(prev, 0);

	QToolButton *next = new QToolButton(this);
	next->setObjectName(QStringLiteral("find_next"));
	next->setText(QStringLiteral("↓"));
	next->setAccessibleName(QStringLiteral("Next match"));
	bssh_apply_touch_height(next, true);
	row->addWidget(next, 0);

	QToolButton *close = new QToolButton(this);
	close->setObjectName(QStringLiteral("find_close"));
	close->setText(QStringLiteral("×"));
	close->setAccessibleName(QStringLiteral("Close find"));
	bssh_apply_touch_height(close, true);
	row->addWidget(close, 0);

	connect(m_field, &QLineEdit::textChanged, this, &bssh_find_bar::search_changed);
	/* Enter walks to the next match -- the incremental search on every
	 * keystroke has already jumped to the nearest one. */
	connect(m_field, &QLineEdit::returnPressed, this, [this] { emit step_requested(1); });
	connect(prev, &QToolButton::clicked, this, [this] { emit step_requested(-1); });
	connect(next, &QToolButton::clicked, this, [this] { emit step_requested(1); });
	connect(close, &QToolButton::clicked, this, &bssh_find_bar::dismissed);

	set_status(0, 0);
}

void bssh_find_bar::take_focus() {
	m_field->setFocus();
	m_field->selectAll();
}

QString bssh_find_bar::query() const {
	return m_field->text();
}

void bssh_find_bar::set_status(int current, int total) {
	if (m_field->text().isEmpty())
		m_status->setText(QString());
	else if (total == 0)
		m_status->setText(QStringLiteral("no matches"));
	else
		m_status->setText(QStringLiteral("%1/%2").arg(current).arg(total));
}

void bssh_find_bar::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Escape) {
		emit dismissed();
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}
