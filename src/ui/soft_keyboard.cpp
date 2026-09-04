#include "ui/soft_keyboard.h"

#include <QAbstractButton>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QToolButton>

#include "input/input_router.h"
#include "term/emulator.h"
#include "ui/soft_keyboard_layout.h"
#include "ui/touch_metrics.h"

namespace {

/*
 * How wide a key may be asked to be, in device-independent pixels.
 *
 * Sec 6.2's 48dp floor is a floor on what a thumb can HIT, and the check
 * that enforces it measures height for exactly this reason: ten keys at
 * 48dp wide is 480 of a 360dp phone, so no keyboard anybody ships is 48
 * wide. Android's own are around 34. What the floor actually protects is
 * the vertical dimension, where nothing is competing for the room.
 *
 * So the minimum WIDTH is set small and deliberately: a button left to
 * its own size hint asks for as much as its label needs, and twenty
 * half-columns of that is a keyboard wider than the screen -- which is
 * the defect sec 6.2 records the key row shipping with, where Ctrl drew
 * as "Ctr" and the row read as merely cramped.
 */
const int BSSH_KEY_MIN_WIDTH_DP = 12;

} /* namespace */

bssh_soft_keyboard::bssh_soft_keyboard(bssh_input_router *router, QWidget *parent)
    : QWidget(parent),
      m_router(router),
      m_keys(nullptr),
      m_grid(nullptr),
      m_group_button(nullptr),
      m_settings_button(nullptr),
      m_hide_button(nullptr),
      m_group(0),
      m_style(bssh_soft_keyboard_catalog::STYLE_COMPACT) {
	QHBoxLayout *outer = new QHBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	m_keys = new QWidget(this);
	m_grid = new QGridLayout(m_keys);
	m_grid->setContentsMargins(0, 0, 0, 0);
	m_grid->setSpacing(1);
	outer->addWidget(m_keys, 1);

	/*
	 * The side button, which is what the whole design turns on: one
	 * thumb-width column, full height, always in the same place. Reaching
	 * the other groups must not depend on finding a key that moved.
	 *
	 * On the right because that is where a right thumb rests, and it is
	 * the edge a phone is usually held by. A left-handed user reaches
	 * further for it than a right-handed one; making the side
	 * configurable is a real question and not one to answer by guessing
	 * here.
	 */
	/*
	 * The side column: settings, the page button, hide. Three things that
	 * are about the KEYBOARD rather than about a page, so they stay put
	 * when the page button is pressed -- which is what makes hide usable
	 * from the numpad and not only from the letters.
	 *
	 * The page button keeps the stretch, so it stays the big one: it is
	 * pressed constantly and the other two are not.
	 */
	QVBoxLayout *side = new QVBoxLayout;
	side->setContentsMargins(0, 0, 0, 0);
	side->setSpacing(1);   /* the key grid's own spacing */

	m_settings_button = new QToolButton(this);
	m_settings_button->setText(QStringLiteral("Cfg"));
	m_settings_button->setObjectName(QStringLiteral("keyboard-settings"));
	m_settings_button->setFocusPolicy(Qt::NoFocus);
	m_settings_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_settings_button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	m_settings_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_settings_button, 0);

	m_group_button = new QToolButton(this);
	m_group_button->setFocusPolicy(Qt::NoFocus);
	m_group_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_group_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	side->addWidget(m_group_button, 1);

	m_hide_button = new QToolButton(this);
	m_hide_button->setText(QStringLiteral("Hide"));
	m_hide_button->setObjectName(QStringLiteral("keyboard-hide"));
	m_hide_button->setFocusPolicy(Qt::NoFocus);
	m_hide_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_hide_button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	m_hide_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_hide_button, 0);

	outer->addLayout(side, 0);

	connect(m_group_button, &QAbstractButton::clicked, this, [this] {
		set_group(bssh_soft_keyboard_catalog::instance().next_index(m_group, m_style));
	});
	connect(m_settings_button, &QAbstractButton::clicked,
	         this, &bssh_soft_keyboard::settings_requested);
	connect(m_hide_button, &QAbstractButton::clicked,
	         this, &bssh_soft_keyboard::hide_requested);

	if (m_router) {
		connect(m_router, &bssh_input_router::sticky_modifiers_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(m_router, &bssh_input_router::locked_modifiers_changed,
		         this, [this](quint8) { refresh_modifiers(); });
	}

	rebuild();
}

QString bssh_soft_keyboard::group_id() const {
	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(m_style);
	if (m_group < 0 || m_group >= groups.size()) return QString();
	return groups.at(m_group).id;
}

void bssh_soft_keyboard::set_style(bssh_soft_keyboard_catalog::style_t style) {
	if (style == m_style) return;

	m_style = style;
	m_group = 0;
	rebuild();
}

void bssh_soft_keyboard::set_group(int index) {
	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(m_style);
	if (groups.isEmpty()) return;

	const int wanted = (index % groups.size() + groups.size()) % groups.size();
	if (wanted == m_group && m_grid->count() > 0) return;

	m_group = wanted;
	rebuild();
}

QAbstractButton *bssh_soft_keyboard::make_button(const QString &label) {
	QToolButton *button = new QToolButton(m_keys);
	button->setText(label);

	/*
	 * Never takes focus, for the reason the key row gives: a key press
	 * that lands on a button instead of the terminal is a keystroke the
	 * remote never sees, and nothing tells the user why.
	 */
	button->setFocusPolicy(Qt::NoFocus);

	button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	button->setMinimumWidth(BSSH_KEY_MIN_WIDTH_DP);
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	return button;
}

void bssh_soft_keyboard::add_key(const bssh_key_t &key, int row, int column) {
	/*
	 * A spacer holds its columns and draws nothing: the gap between the
	 * function banks, and the keys a terminal cannot send. No button is
	 * made, because a button that does nothing is worse than a gap -- it
	 * invites a press and answers with silence.
	 */
	if (key.kind == bssh_key_t::SPACER) return;

	QAbstractButton *button = make_button(key.label);
	m_grid->addWidget(button, row, column, 1, key.span);

	switch (key.kind) {
	case bssh_key_t::SPACER:
		/* Returned above; named so the switch stays exhaustive. */
		break;

	case bssh_key_t::KEY: {
		const int code = key.value;
		connect(button, &QAbstractButton::clicked, this, [this, code] {
			if (m_router) m_router->send_key(code, bssh_emulator::MOD_NONE);
		});
		break;
	}
	case bssh_key_t::MODIFIER: {
		const quint8 modifier = key.modifier;
		button->setCheckable(true);
		m_modifier_buttons.append(button);
		m_modifier_values.append(modifier);
		connect(button, &QAbstractButton::clicked, this, [this, modifier] {
			/* One press arms, a second locks, a third releases (sec 6.2). */
			if (m_router) m_router->cycle_modifier(modifier);

			/*
			 * Refreshed outside the guard: the button is checkable, so
			 * Qt has already toggled it, and returning early would leave
			 * it drawn as pressed with no terminal behind it.
			 */
			refresh_modifiers();
		});
		break;
	}
	case bssh_key_t::TEXT: {
		const char32_t ch = char32_t(key.value);

		/*
		 * Every key that CHANGES under Shift goes in the relabelling
		 * list: letters, which become capitals, and anything carrying a
		 * second legend -- 1 becoming !, / becoming ?.
		 *
		 * This used to hold letters only, on the reasoning that "a digit
		 * or a bracket has no capital to become". True of toUpper and
		 * false of a keyboard, and on the full-size board's real number
		 * row it left 21 characters unreachable.
		 */
		if (QChar::isLetter(uint(ch)) || key.shifted_value != 0) {
			m_letter_buttons.append(button);
			m_letter_values.append(ch);
			m_letter_shifted.append(char32_t(key.shifted_value));
		}

		const char32_t twin = char32_t(key.shifted_value);
		connect(button, &QAbstractButton::clicked, this, [this, ch, twin] {
			if (!m_router) return;

			/*
			 * Shift is applied HERE, to the character, and not sent as a
			 * modifier. That is what a shift key on a soft keyboard is:
			 * the button sends a different character. The router masks
			 * SHIFT out of send_text for the same reason, so doing it
			 * there as well would be asking twice and getting neither.
			 */
			const bool shifted =
			        m_router->modifier_state(bssh_emulator::MOD_SHIFT)
			                != bssh_input_router::MODIFIER_OFF;

			m_router->send_text(QString(bssh_shifted_char(ch, twin, shifted)));
			refresh_letter_labels();
		});
		break;
	}
	case bssh_key_t::CHORD: {
		const int code = key.value;
		const quint8 modifier = key.modifier;
		connect(button, &QAbstractButton::clicked, this, [this, code, modifier] {
			if (m_router) m_router->send_key(code, modifier);
		});
		break;
	}
	case bssh_key_t::FUNCTION_TOGGLE:
		/*
		 * A row swaps itself for F1..F12 because it has one strip to
		 * spend (sec 6.2). This keyboard has a group for them and the
		 * side button to reach it, so the toggle has nothing to do here
		 * and no layout uses it. Named rather than defaulted, so adding
		 * a kind to bssh_key_t fails to compile here rather than
		 * silently drawing a dead button.
		 */
		break;
	}
}

void bssh_soft_keyboard::rebuild() {
	m_modifier_buttons.clear();
	m_modifier_values.clear();
	m_letter_buttons.clear();
	m_letter_values.clear();
	m_letter_shifted.clear();

	while (QLayoutItem *item = m_grid->takeAt(0)) {
		delete item->widget();
		delete item;
	}

	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(m_style);
	if (m_group < 0 || m_group >= groups.size()) return;

	const bssh_soft_key_group_t &group = groups.at(m_group);

	for (int r = 0; r < group.rows.size(); ++r) {
		int column = 0;
		for (const bssh_key_t &key : group.rows.at(r)) {
			add_key(key, r, column);
			column += key.span;
		}
	}

	/*
	 * Every half-column pulls equally, so a key's width is its span and
	 * nothing else. Without this the columns are sized from whatever
	 * happens to sit in them and a row of ten letters comes out ragged.
	 */
	for (int c = 0; c < group.columns; ++c) m_grid->setColumnStretch(c, 1);

	/*
	 * And the columns the PREVIOUS group used and this one does not, or a
	 * narrower group keeps stretch on columns nothing occupies and comes
	 * out squeezed into the left of the widget.
	 */
	for (int c = group.columns; c < BSSH_KEYBOARD_MAX_COLUMNS; ++c) {
		m_grid->setColumnStretch(c, 0);
	}

	/*
	 * The side button names the group it goes TO, not the one on screen.
	 * A button labelled with where you already are is one nobody presses.
	 */
	const int next = bssh_soft_keyboard_catalog::instance().next_index(m_group, m_style);
	if (next >= 0 && next < groups.size()) {
		m_group_button->setText(groups.at(next).button_label);
	}

	refresh_modifiers();
}

void bssh_soft_keyboard::refresh_modifiers() {
	/* No router means no session, so nothing is armed; see the key row. */
	for (int i = 0; i < m_modifier_buttons.size(); ++i) {
		QAbstractButton *button = m_modifier_buttons.at(i);
		const bssh_input_router::modifier_state_t state =
		        m_router ? m_router->modifier_state(m_modifier_values.at(i))
		                 : bssh_input_router::MODIFIER_OFF;

		button->setChecked(state != bssh_input_router::MODIFIER_OFF);
		button->setProperty("bssh_modifier_state", int(state));

		QFont font = button->font();
		if (font.bold() != (state == bssh_input_router::MODIFIER_LOCKED)) {
			font.setBold(state == bssh_input_router::MODIFIER_LOCKED);
			button->setFont(font);
		}
	}

	refresh_letter_labels();
}

void bssh_soft_keyboard::refresh_letter_labels() {
	/*
	 * Same rule as the modifiers: with no session there is no Shift, so
	 * the letters read lowercase. Returning early left a keyboard drawn
	 * in capitals after the session that shifted it had gone.
	 */
	const bool shifted = m_router
	        && m_router->modifier_state(bssh_emulator::MOD_SHIFT)
	                != bssh_input_router::MODIFIER_OFF;

	for (int i = 0; i < m_letter_buttons.size(); ++i) {
		const char32_t plain = m_letter_values.at(i);
		const char32_t twin = m_letter_shifted.at(i);
		m_letter_buttons.at(i)->setText(
		        QString(bssh_shifted_char(plain, twin, shifted)));
	}
}

void bssh_soft_keyboard::set_router(bssh_input_router *router) {
	if (m_router == router) return;

	if (m_router) disconnect(m_router, nullptr, this, nullptr);
	m_router = router;

	if (m_router) {
		connect(m_router, &bssh_input_router::sticky_modifiers_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(m_router, &bssh_input_router::locked_modifiers_changed,
		         this, [this](quint8) { refresh_modifiers(); });
	}

	refresh_modifiers();
}
