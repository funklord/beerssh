#include "ui/key_row.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "input/input_router.h"
#include "platform/platform.h"
#include "term/emulator.h"

/*
 * Minimum touch target. Android's own guidance is 48dp, and a terminal key
 * row is exactly the place where an undersized target hurts most: the keys
 * here are the ones pressed mid-command, where a miss sends the wrong
 * control character to a live shell (sec 8.3 asks for the same 48dp on
 * split dividers).
 */
static const int BSSH_TOUCH_TARGET_DP = 48;

bssh_key_row::bssh_key_row(bssh_input_router *router, QWidget *parent)
    : QWidget(parent),
      m_router(router),
      m_mode(AUTO) {
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(2, 2, 2, 2);
	layout->setSpacing(2);

	add_key(QStringLiteral("Esc"), bssh_emulator::KEY_ESCAPE);
	add_key(QStringLiteral("Tab"), bssh_emulator::KEY_TAB);
	add_modifier(QStringLiteral("Ctrl"), bssh_emulator::MOD_CTRL);
	add_modifier(QStringLiteral("Alt"), bssh_emulator::MOD_ALT);
	add_key(QStringLiteral("←"), bssh_emulator::KEY_LEFT);
	add_key(QStringLiteral("↓"), bssh_emulator::KEY_DOWN);
	add_key(QStringLiteral("↑"), bssh_emulator::KEY_UP);
	add_key(QStringLiteral("→"), bssh_emulator::KEY_RIGHT);

	/*
	 * The punctuation a soft keyboard buries two layers deep, and which a
	 * shell needs constantly. The set is a starting point, not a decision:
	 * sec 10.2 makes the row's layout preset-owned, so an OpenWrt box and
	 * a build server can carry different keys.
	 */
	add_text(QStringLiteral("-"), U'-');
	add_text(QStringLiteral("/"), U'/');
	add_text(QStringLiteral("|"), U'|');
	add_text(QStringLiteral("~"), U'~');

	layout->addStretch(1);

	if (m_router) {
		connect(m_router, &bssh_input_router::sticky_modifiers_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
	}

	setVisible(should_be_visible());
}

QAbstractButton *bssh_key_row::make_button(const QString &label) {
	QToolButton *button = new QToolButton(this);
	button->setText(label);
	button->setMinimumSize(BSSH_TOUCH_TARGET_DP, BSSH_TOUCH_TARGET_DP);

	/*
	 * The row must never take focus: a key press that lands on a button
	 * instead of the terminal is a keystroke the remote never sees, and
	 * the user has no way to tell why.
	 */
	button->setFocusPolicy(Qt::NoFocus);

	layout()->addWidget(button);
	return button;
}

void bssh_key_row::add_key(const QString &label, int key) {
	QAbstractButton *button = make_button(label);
	connect(button, &QAbstractButton::clicked, this, [this, key] {
		if (m_router) m_router->send_key(key, bssh_emulator::MOD_NONE);
	});
}

void bssh_key_row::add_text(const QString &label, char32_t ch) {
	QAbstractButton *button = make_button(label);
	connect(button, &QAbstractButton::clicked, this, [this, ch] {
		if (!m_router) return;
		/* Through the router, so a sticky Ctrl applies to these too. */
		QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, QString::fromUcs4(&ch, 1));
		m_router->handle_key(&event);
	});
}

void bssh_key_row::add_modifier(const QString &label, quint8 modifier) {
	QAbstractButton *button = make_button(label);
	button->setCheckable(true);

	m_modifier_buttons.append(button);
	m_modifier_values.append(modifier);

	connect(button, &QAbstractButton::clicked, this, [this, modifier] {
		if (!m_router) return;
		/* Toggle: pressing Ctrl twice should clear it, not stack it. */
		m_router->set_sticky_modifiers(m_router->sticky_modifiers() ^ modifier);
		refresh_modifier_buttons();
	});
}

void bssh_key_row::refresh_modifier_buttons() {
	if (!m_router) return;

	const quint8 sticky = m_router->sticky_modifiers();
	for (int i = 0; i < m_modifier_buttons.size(); ++i) {
		/*
		 * The button's checked state has to follow the router, not its
		 * own clicks: the modifier is consumed by the next keystroke, and
		 * a Ctrl button still looking pressed afterwards is a button
		 * lying about the state of the terminal.
		 */
		m_modifier_buttons.at(i)->setChecked(sticky & m_modifier_values.at(i));
	}
}

bool bssh_key_row::should_be_visible() const {
	if (m_mode == ALWAYS) return true;
	if (m_mode == NEVER) return false;
	return bssh_platform::instance()->is_touch_primary();
}

void bssh_key_row::set_visibility_mode(visibility_t mode) {
	m_mode = mode;
	setVisible(should_be_visible());
}
