#include "ui/touch_metrics.h"
#include "ui/key_row.h"

#include <QFont>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "input/input_router.h"
#include "platform/platform.h"
#include "term/emulator.h"
#include "ui/key_row_layout.h"

bssh_key_row::bssh_key_row(bssh_input_router *router, QWidget *parent)
    : QWidget(parent),
      m_router(router),
      m_mode(AUTO),
      m_scroll(nullptr),
      m_keys(nullptr),
      m_function_mode(false) {
	/*
	 * The keys scroll horizontally, because they do not fit and never did.
	 *
	 * sec 6.2 asks for Esc, Tab, Ctrl, Alt, four arrows, Fn and four
	 * punctuation keys, and sec 6.2's own touch floor puts each at 48dp.
	 * That is 624dp of keys on a 360dp phone before spacing -- measured
	 * at 652 -- so the row demanded nearly twice the width of the screen
	 * it exists for. The symptom did not look like a layout bug: Qt
	 * squeezed the buttons past their minimum and the labels elided, so
	 * "Ctrl" drew as "Ctr" and the row read as merely cramped. It is
	 * visible in this project's own device screenshots.
	 *
	 * Scrolling rather than shrinking, because the alternative is going
	 * under the touch floor, and a key too small to hit reliably is worse
	 * than one you have to scroll to. It is what the tab bar does with
	 * the same problem (sec 8.3), and what ConnectBot and Termux do with
	 * this exact strip. Measured: 652px of minimum width becomes 92.
	 */
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	m_scroll = new QScrollArea(this);
	m_scroll->setFrameShape(QFrame::NoFrame);
	m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_scroll->setWidgetResizable(true);
	outer->addWidget(m_scroll);

	m_keys = new QWidget(m_scroll);
	m_scroll->setWidget(m_keys);

	QHBoxLayout *layout = new QHBoxLayout(m_keys);
	layout->setContentsMargins(2, 2, 2, 2);
	layout->setSpacing(2);

	/*
	 * Dragged with a finger, like the host list (sec 8.5): a scrollbar
	 * dragged with a thumb is the most obvious way a Widgets app
	 * announces it was not written for touch. The bar itself is off --
	 * there is no room for one under 48dp keys, and the drag is the
	 * affordance.
	 */
	QScroller::grabGesture(m_scroll->viewport(), QScroller::LeftMouseButtonGesture);

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
		/*
		 * Both halves, because a lock survives the keystroke that spends
		 * the armed one -- so a listener watching only the armed set
		 * would stop redrawing exactly when the state stopped being
		 * obvious.
		 */
		connect(m_router, &bssh_input_router::locked_modifiers_changed,
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

	QHBoxLayout *row = qobject_cast<QHBoxLayout *>(m_keys->layout());
	if (!row) return;

	/* The stretch is re-added after the keys, so it stays last. */
	while (QLayoutItem *item = row->takeAt(0)) delete item;

	/*
	 * Either the layout's own keys or F1..F12, depending on whether Fn is
	 * held down -- and Fn itself stays on the row in both, or there
	 * would be no way back.
	 */
	QList<bssh_key_t> keys = m_function_mode ? bssh_function_keys() : layout->keys;
	if (m_function_mode) {
		bssh_key_t back;
		back.label = QStringLiteral("Fn");
		back.kind = bssh_key_t::FUNCTION_TOGGLE;
		keys.prepend(back);
	}

	for (const bssh_key_t &key : keys) {
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
		case bssh_key_t::FUNCTION_TOGGLE:
			add_function_toggle(key.label);
			break;
		}
	}

	row->addStretch(1);
	refresh_modifier_buttons();
}

void bssh_key_row::add_function_toggle(const QString &label) {
	QAbstractButton *button = make_button(label);
	button->setCheckable(true);
	button->setChecked(m_function_mode);

	connect(button, &QAbstractButton::clicked, this, [this] {
		/*
		 * Rebuilds the row in place. set_layout() early-returns when the
		 * id has not changed, which is right for every other caller and
		 * wrong here -- the id is the same and the keys are not -- so
		 * the flag is flipped and the layout forced.
		 */
		m_function_mode = !m_function_mode;

		const QString id = m_layout_id;
		m_layout_id.clear();
		set_layout(id);
	});
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
	QToolButton *button = new QToolButton(m_keys);
	button->setText(label);
	m_buttons.append(button);
	button->setMinimumSize(BSSH_TOUCH_TARGET_DP, BSSH_TOUCH_TARGET_DP);

	/*
	 * The row must never take focus: a key press that lands on a button
	 * instead of the terminal is a keystroke the remote never sees, and
	 * the user has no way to tell why.
	 */
	button->setFocusPolicy(Qt::NoFocus);

	m_keys->layout()->addWidget(button);

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
		/*
		 * One press arms, a second locks, a third releases -- the Treo's
		 * cycle, and the router owns it because the built-in keyboard
		 * (sec 6.2) asks the same question with different buttons.
		 */
		m_router->cycle_modifier(modifier);
		refresh_modifier_buttons();
	});
}

void bssh_key_row::refresh_modifier_buttons() {
	if (!m_router) return;

	for (int i = 0; i < m_modifier_buttons.size(); ++i) {
		/*
		 * The button's state has to follow the router, not its own
		 * clicks: the armed half is consumed by the next keystroke, and
		 * a Ctrl button still looking pressed afterwards is a button
		 * lying about the state of the terminal.
		 *
		 * ARMED and LOCKED must not look the same. They differ by
		 * exactly the thing a user needs to know before typing the next
		 * letter -- whether it is about to be a control character -- and
		 * a lock that reads as an arm is how somebody sends Ctrl-L,
		 * Ctrl-S, Ctrl-Q while believing they typed "lsq". Bold rather
		 * than a second colour, because the row is drawn against
		 * whatever palette the theme is in (sec 6.2) and weight survives
		 * both.
		 */
		QAbstractButton *button = m_modifier_buttons.at(i);
		const bssh_input_router::modifier_state_t state =
		        m_router->modifier_state(m_modifier_values.at(i));

		button->setChecked(state != bssh_input_router::MODIFIER_OFF);

		/*
		 * Named on the button as well, so a test can ask which of the
		 * three it is. setChecked cannot answer that -- two of the
		 * states share it, which is the whole difficulty.
		 */
		button->setProperty("bssh_modifier_state", int(state));

		QFont font = button->font();
		if (font.bold() != (state == bssh_input_router::MODIFIER_LOCKED)) {
			font.setBold(state == bssh_input_router::MODIFIER_LOCKED);
			button->setFont(font);
		}
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
		/*
		 * Both halves, because a lock survives the keystroke that spends
		 * the armed one -- so a listener watching only the armed set
		 * would stop redrawing exactly when the state stopped being
		 * obvious.
		 */
		connect(m_router, &bssh_input_router::locked_modifiers_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
	}

	refresh_modifier_buttons();
}

void bssh_key_row::set_visibility_mode(visibility_t mode) {
	m_mode = mode;
	setVisible(should_be_visible());
}
