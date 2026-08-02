#include "ui/key_row.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "input/input_router.h"
#include "platform/platform.h"
#include "term/emulator.h"
#include "ui/key_row_layout.h"

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

	/*
	 * Built from a layout rather than hardcoded (sec 6.2, sec 10.2): the
	 * keys that matter differ by what is on the other end of the
	 * connection, which is why the row's layout is a preset-owned setting
	 * and why the starter presets are named after host types.
	 */
	set_layout(QString());

	if (m_router) {
		connect(m_router, &bssh_input_router::sticky_modifiers_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
	}

	setVisible(should_be_visible());
}

void bssh_key_row::set_layout(const QString &id) {
	const bssh_key_row_catalog &catalog = bssh_key_row_catalog::instance();

	const QString wanted = catalog.effective_id(id);
	if (wanted == m_layout_id && !m_buttons.isEmpty()) return;

	m_layout_id = wanted;

	/*
	 * An unknown layout falls back to the default rather than to an empty
	 * strip. A row with no keys on a phone is a session that cannot send
	 * Ctrl-C, which is a far worse outcome than the wrong keys.
	 */
	const bssh_key_row_layout_t *layout = catalog.by_id(wanted);
	if (!layout) layout = catalog.by_id(catalog.default_id());
	if (!layout) return;

	qDeleteAll(m_buttons);
	m_buttons.clear();
	m_modifier_buttons.clear();
	m_modifier_values.clear();

	QHBoxLayout *row = qobject_cast<QHBoxLayout *>(this->layout());
	if (!row) return;

	/* The stretch is re-added after the keys, so it stays last. */
	while (QLayoutItem *item = row->takeAt(0)) delete item;

	for (const bssh_key_t &key : layout->keys) {
		switch (key.kind) {
		case bssh_key_t::KEY:
			add_key(key.label, key.value);
			break;
		case bssh_key_t::MODIFIER:
			add_modifier(key.label, key.modifier);
			break;
		case bssh_key_t::TEXT:
			add_text(key.label, char32_t(key.value));
			break;
		case bssh_key_t::CHORD:
			add_chord(key.label, char32_t(key.value), key.modifier);
			break;
		}
	}

	row->addStretch(1);
	refresh_modifier_buttons();
}

void bssh_key_row::add_chord(const QString &label, char32_t ch, quint8 modifier) {
	QAbstractButton *button = make_button(label);

	connect(button, &QAbstractButton::clicked, this, [this, ch, modifier] {
		if (!m_router) return;

		/*
		 * Sent as one event with the modifier attached, NOT by setting a
		 * sticky modifier and then the key: a sticky Ctrl the user
		 * already had set would combine with this one and send something
		 * nobody asked for. A chord is a single press with a single
		 * meaning.
		 */
		const Qt::KeyboardModifiers qt_modifiers =
		        (modifier & bssh_emulator::MOD_CTRL ? Qt::ControlModifier : Qt::NoModifier) |
		        (modifier & bssh_emulator::MOD_ALT ? Qt::AltModifier : Qt::NoModifier);

		/*
		 * The KEY CODE, with empty text, and this is not incidental: with
		 * Ctrl held the router reads the codepoint from key() rather than
		 * from text(), because a real keyboard's text() is already the
		 * control character and using it would encode the modifier twice.
		 * A synthetic event carrying only text therefore falls through
		 * that branch and arrives as a plain letter -- pressing the tmux
		 * prefix would type "b" into the shell.
		 */
		const int code = QChar(char16_t(ch)).toUpper().unicode();

		QKeyEvent event(QEvent::KeyPress, code, qt_modifiers, QString());
		m_router->handle_key(&event);
	});
}

QAbstractButton *bssh_key_row::make_button(const QString &label) {
	QToolButton *button = new QToolButton(this);
	button->setText(label);
	m_buttons.append(button);
	button->setMinimumSize(BSSH_TOUCH_TARGET_DP, BSSH_TOUCH_TARGET_DP);

	/*
	 * The row must never take focus: a key press that lands on a button
	 * instead of the terminal is a keystroke the remote never sees, and
	 * the user has no way to tell why.
	 */
	button->setFocusPolicy(Qt::NoFocus);

	layout()->addWidget(button);

	/*
	 * Explicitly shown, because adding a widget to a layout whose parent
	 * is ALREADY visible does not show it -- Qt only shows children when
	 * the parent itself is shown. The keys built in the constructor
	 * escaped this (the whole row was shown afterwards) and every layout
	 * switched to at runtime did not: the row rebuilt itself correctly,
	 * reported the right number of buttons, and drew an empty strip.
	 *
	 * The tests counted buttons and passed. A screenshot is what noticed.
	 */
	button->show();

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

void bssh_key_row::set_router(bssh_input_router *router) {
	if (m_router == router) return;

	if (m_router) disconnect(m_router, nullptr, this, nullptr);
	m_router = router;

	if (m_router) {
		connect(m_router, &bssh_input_router::sticky_modifiers_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
	}

	refresh_modifier_buttons();
}

void bssh_key_row::set_visibility_mode(visibility_t mode) {
	m_mode = mode;
	setVisible(should_be_visible());
}
