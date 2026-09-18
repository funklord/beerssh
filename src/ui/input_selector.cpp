#include "ui/input_selector.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>

#include "ui/touch_metrics.h"

namespace {

/*
 * The states, in the order sec 8.7 names them, with the id the mode enum
 * uses. `object` is what a test and an accessibility reader find the button
 * by; `label` is what a thumb reads. `column_only` marks a state that acts
 * on the column and is shown only when there is a column -- see
 * set_column_available.
 */
struct state_t {
	bssh_input_mode_t mode;
	const char *object;
	const char *label;
	bool column_only;
};

const state_t STATES[] = {
	{ BSSH_INPUT_SCROLL, "input_scroll", "Scroll", false },
	{ BSSH_INPUT_SELECT, "input_select", "Select", false },
	{ BSSH_INPUT_CLICK,  "input_click",  "Click",  false },
	{ BSSH_INPUT_MOVE,   "input_move",   "Move",   true  },
};

} // namespace

bssh_input_selector::bssh_input_selector(QWidget *parent)
    : QWidget(parent),
      m_group(new QButtonGroup(this)),
      m_mode(BSSH_INPUT_SCROLL),
      m_column_available(false) {
	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);

	m_group->setExclusive(true);

	for (const state_t &state : STATES) {
		QToolButton *button = new QToolButton(this);
		button->setText(QString::fromLatin1(state.label));
		button->setObjectName(QString::fromLatin1(state.object));
		button->setAccessibleName(QString::fromLatin1(state.label));
		button->setCheckable(true);
		button->setChecked(state.mode == m_mode);
		button->setToolButtonStyle(Qt::ToolButtonTextOnly);

		/*
		 * Each button carries the same touch floor as the key row and
		 * the menu it replaces (sec 6.2), and shares the width so three
		 * fit a phone without one crowding the others.
		 */
		bssh_apply_touch_height(button, true);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

		/*
		 * A column-only state starts hidden: the window begins tiled and
		 * calls set_column_available when it enters a column. Hidden
		 * rather than absent so the button keeps its place in the row and
		 * the group, and simply appears when the column does.
		 */
		if (state.column_only) button->hide();

		m_group->addButton(button, int(state.mode));
		row->addWidget(button, 1);
	}

	/*
	 * The user's choice, told apart from set_mode()'s silent one: only a
	 * click reaches here, so this is where the signal belongs.
	 */
	connect(m_group, &QButtonGroup::idClicked, this, [this](int id) {
		choose(bssh_input_mode_t(id));
	});
}

void bssh_input_selector::choose(bssh_input_mode_t mode) {
	if (mode == m_mode) return;

	m_mode = mode;
	emit mode_changed(m_mode);
}

void bssh_input_selector::set_mode(bssh_input_mode_t mode) {
	m_mode = mode;

	if (QAbstractButton *button = m_group->button(int(mode))) {
		/*
		 * The group is exclusive, so checking one clears the rest; no
		 * signal goes out, because set_mode is the path that must NOT
		 * emit -- idClicked fires only for a real press.
		 */
		button->setChecked(true);
	}
}

void bssh_input_selector::set_column_available(bool available) {
	if (available == m_column_available) return;
	m_column_available = available;

	for (const state_t &state : STATES) {
		if (!state.column_only) continue;
		if (QAbstractButton *button = m_group->button(int(state.mode))) {
			button->setVisible(available);
		}
	}

	/*
	 * Losing the column while its mode is selected would strand the mode
	 * on a hidden control, so fall back to SCROLL and tell the window --
	 * the reorder gesture stops meaning anything the moment there is one
	 * pane, and a body drag has to go back to scrolling scrollback rather
	 * than reordering a column that is not there.
	 */
	if (!available && is_column_mode(m_mode)) {
		set_mode(BSSH_INPUT_SCROLL);
		emit mode_changed(m_mode);
	}
}
