#include "ui/input_selector.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QEvent>
#include <QMouseEvent>
#include <QToolButton>
#include <QWheelEvent>

#include "platform/platform.h"
#include "ui/touch_metrics.h"

namespace {

/*
 * The states, in drum order, with the id the mode enum uses. `object` is
 * what a test and an accessibility reader find the button by; `label` is
 * what is read. `column_only` marks a state that acts on the column and is
 * offered only when there is one -- see set_column_available.
 */
struct state_t {
	bssh_input_mode_t mode;
	const char *object;
	const char *label;
	bool column_only;
};

const state_t STATES[] = {
	{ BSSH_INPUT_SCROLL,        "input_scroll", "Scroll", false },
	{ BSSH_INPUT_SELECT,        "input_select", "Select", false },
	{ BSSH_INPUT_CLICK,         "input_click",  "Click",  false },
	{ BSSH_INPUT_MOVE,          "input_move",   "Move",   true  },
	{ BSSH_INPUT_COLUMN_SCROLL, "input_pan",    "Pan",    true  },
};

bool touch_primary() {
	const bssh_platform *platform = bssh_platform::instance();
	return platform && platform->is_touch_primary();
}

/*
 * What Click reads while the program is listening to the mouse. Measured
 * for the width as well as shown, so the drum does not change width when
 * a program grabs the mouse.
 */
const char *const LISTENING_MARK = " •";

} // namespace

bssh_input_mode_t bssh_default_input_mode() {
	return touch_primary() ? BSSH_INPUT_SCROLL : BSSH_INPUT_CLICK;
}

bssh_input_selector::bssh_input_selector(QWidget *parent)
    : QWidget(parent),
      m_group(new QButtonGroup(this)),
      m_mode(bssh_default_input_mode()),
      m_column_available(false),
      m_program_listening(false) {
	setObjectName(QStringLiteral("input_selector"));
	setAccessibleName(QStringLiteral("Input mode"));
	setToolTip(QStringLiteral("What a click or a drag does here"));
	m_group->setExclusive(true);

	for (const state_t &state : STATES) {
		QToolButton *button = new QToolButton(this);
		button->setText(QString::fromLatin1(state.label));
		button->setObjectName(QString::fromLatin1(state.object));
		button->setAccessibleName(QString::fromLatin1(state.label));
		button->setCheckable(true);
		button->setAutoRaise(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolButtonStyle(Qt::ToolButtonTextOnly);

		/*
		 * The drum takes every press itself, so a drag that starts on a
		 * row scrolls rather than being eaten by the button under it; a
		 * tap is told from a drag on release and chooses that row. The
		 * buttons stay real buttons for the accessibility tree and for
		 * click(), which is how a reader or a test chooses one.
		 */
		button->setAttribute(Qt::WA_TransparentForMouseEvents);

		m_group->addButton(button, int(state.mode));
	}

	/* A click(), from a reader or a test: the user's choice. */
	connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
		choose(bssh_input_mode_t(id));
	});

	relayout();
}

QList<bssh_input_mode_t> bssh_input_selector::available() const {
	QList<bssh_input_mode_t> modes;
	for (const state_t &state : STATES) {
		if (state.column_only && !m_column_available) continue;
		modes.append(state.mode);
	}
	return modes;
}

bool bssh_input_selector::rotates() const {
	return available().size() > 3;
}

bssh_input_mode_t bssh_input_selector::shown(int row) const {
	const QList<bssh_input_mode_t> modes = available();
	const int n = modes.size();

	/*
	 * Three modes are all on screen at once, so they stay in their order
	 * with the current one marked: the holder, "with only three options
	 * all on the screen, why would we scroll?" Only with the column's two
	 * more does the drum turn, keeping the current mode in the middle and
	 * wrapping so every mode is two steps away.
	 */
	if (n <= 3) return modes.at(qBound(0, row, n - 1));
	const int at = qMax(0, int(modes.indexOf(m_mode)));
	return modes.at(((at + row - 1) % n + n) % n);
}

int bssh_input_selector::row_height() const {
	/*
	 * A row a thumb taps is a touch target, so it takes the 48dp floor
	 * every touch control here does (sec 6.2) -- 36 was tried first, to
	 * keep three rows off half a landscape phone's height, and the sweep
	 * that holds every touch surface to the floor refused it. The drum
	 * covers columns at the corner, not whole rows, so the height costs
	 * less than it looks. With a pointer: the text and a margin.
	 */
	const int text = fontMetrics().height() + 6;
	return touch_primary() ? qMax(text, int(BSSH_TOUCH_TARGET_DP)) : text;
}

QString bssh_input_selector::label_for(bssh_input_mode_t mode) const {
	for (const state_t &state : STATES) {
		if (state.mode != mode) continue;
		QString label = QString::fromLatin1(state.label);
		if (mode == BSSH_INPUT_CLICK && m_program_listening) label += QString::fromUtf8(LISTENING_MARK);
		return label;
	}
	return QString();
}

QSize bssh_input_selector::sizeHint() const {
	int wide = 0;
	for (const state_t &state : STATES) {
		QString label = QString::fromLatin1(state.label);
		if (state.mode == BSSH_INPUT_CLICK) label += QString::fromUtf8(LISTENING_MARK);
		wide = qMax(wide, fontMetrics().horizontalAdvance(label));
	}
	/* The labels and a small margin, no more: it sits over the text. */
	return QSize(wide + 10, 3 * row_height());
}

void bssh_input_selector::relayout() {
	const int row = row_height();
	for (const state_t &state : STATES) {
		QAbstractButton *button = m_group->button(int(state.mode));
		if (!button) continue;
		button->hide();
		button->setChecked(state.mode == m_mode);
		button->setText(label_for(state.mode));
	}
	for (int r = 0; r < 3; ++r) {
		QAbstractButton *button = m_group->button(int(shown(r)));
		if (!button) continue;
		button->setGeometry(0, r * row, width(), row);
		button->show();
	}
	update();
}

void bssh_input_selector::choose(bssh_input_mode_t mode) {
	if (mode == m_mode) {
		relayout();
		return;
	}
	m_mode = mode;
	relayout();
	emit mode_changed(m_mode);
}

void bssh_input_selector::step(int direction) {
	if (rotates()) {
		choose(shown(direction > 0 ? 2 : 0));
		return;
	}

	/* A fixed list: the mode beside the current one, stopping at the ends. */
	const QList<bssh_input_mode_t> modes = available();
	const int at = qMax(0, int(modes.indexOf(m_mode)));
	choose(modes.at(qBound(0, at + (direction > 0 ? 1 : -1), int(modes.size()) - 1)));
}

void bssh_input_selector::set_mode(bssh_input_mode_t mode) {
	m_mode = mode;
	relayout();
}

void bssh_input_selector::set_column_available(bool available) {
	if (available == m_column_available) return;
	m_column_available = available;

	if (!available && is_column_mode(m_mode)) {
		m_mode = bssh_default_input_mode();
		relayout();
		emit mode_changed(m_mode);
		return;
	}
	relayout();
}

void bssh_input_selector::set_program_listening(bool listening) {
	if (listening == m_program_listening) return;
	m_program_listening = listening;
	if (QAbstractButton *click = m_group->button(int(BSSH_INPUT_CLICK))) {
		click->setToolTip(listening ? QStringLiteral("The program here is using the mouse")
		                            : QString());
	}
	relayout();
}

void bssh_input_selector::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	relayout();
}

void bssh_input_selector::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::FontChange) {
		updateGeometry();
		relayout();
	}
}

void bssh_input_selector::mousePressEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	m_pressed = true;
	m_dragged = false;
	m_drag_y = event->position().toPoint().y();
	event->accept();
}

void bssh_input_selector::mouseMoveEvent(QMouseEvent *event) {
	if (!m_pressed) return;

	/*
	 * A drum: dragging down brings the mode above into the middle, as a
	 * wheel turned toward you would. One step per row moved, measured from
	 * where the last step happened so a long drag walks the whole list.
	 */
	const int row = qMax(1, row_height());
	const int y = event->position().toPoint().y();
	while (y - m_drag_y >= row) {
		m_drag_y += row;
		m_dragged = true;
		step(-1);
	}
	while (m_drag_y - y >= row) {
		m_drag_y -= row;
		m_dragged = true;
		step(1);
	}
	event->accept();
}

void bssh_input_selector::mouseReleaseEvent(QMouseEvent *event) {
	if (!m_pressed) return;
	m_pressed = false;

	/* A tap: the row under it. Choosing the current one changes nothing. */
	if (!m_dragged) {
		const int r = event->position().toPoint().y() / qMax(1, row_height());
		if (r >= 0 && r <= 2) choose(shown(r));
	}
	event->accept();
}

void bssh_input_selector::wheelEvent(QWheelEvent *event) {
	/*
	 * Per notch, accumulated, so a touchpad's small deltas add up to steps
	 * rather than each one stepping. Turned away from you, the mode below
	 * comes up -- the way a list under the wheel would move.
	 */
	m_wheel += event->angleDelta().y();
	while (m_wheel >= 120) {
		m_wheel -= 120;
		step(-1);
	}
	while (m_wheel <= -120) {
		m_wheel += 120;
		step(1);
	}
	event->accept();
}
