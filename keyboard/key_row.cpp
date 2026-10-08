#include "keyboard/sk_metrics.h"
#include "keyboard/key_row.h"

#include <QFont>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QToolButton>

#include "keyboard/sk_modifiers.h"
#include "keyboard/sk_target.h"
#include "keyboard/key_cap.h"
#include "keyboard/touch_chords.h"
#include <QInputDevice>
#include "keyboard/sk_modifiers.h"
#include "keyboard/key_row_layout.h"

bssh_key_row::bssh_key_row(sk_target *target, QWidget *parent)
    : QWidget(parent),
      m_target(target),
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

	m_touch = new bssh_touch_chords(m_keys);

	/*
	 * Two thumbs (sec 6.2): a finger resting on a modifier holds it on
	 * the router, and lifting it with nothing pressed in between is a tap
	 * -- the same cycle a click on the button gives.
	 */
	connect(m_touch, &bssh_touch_chords::modifier_held, this, [this](quint8 modifier) {
		if (m_target) m_target->modifiers().hold(modifier);
		refresh_modifier_buttons();
	});
	connect(m_touch, &bssh_touch_chords::modifier_released,
	         this, [this](quint8 modifier, bool cancelled) {
		if (m_target && m_target->modifiers().release(modifier) && !cancelled) {
			m_target->modifiers().cycle(modifier);
		}
		refresh_modifier_buttons();
	});

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

	if (m_target) {
		connect(&m_target->modifiers(), &sk_modifiers::armed_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
		/*
		 * Both halves, because a lock survives the keystroke that spends
		 * the armed one -- so a listener watching only the armed set
		 * would stop redrawing exactly when the state stopped being
		 * obvious.
		 */
		connect(&m_target->modifiers(), &sk_modifiers::locked_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
		connect(&m_target->modifiers(), &sk_modifiers::held_changed,
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
		case bssh_key_t::SPACER:
			add_spacer();
			break;
		}
	}

	row->addStretch(1);
	refresh_modifier_buttons();
}

void bssh_key_row::add_spacer() {
	/*
	 * `gap` is one of the words a custom row may be written with
	 * (sec 6.2), and it did nothing at all: this switch had no case for
	 * SPACER, so the token parsed, the editor's live preview showed the
	 * row without it, and the keys after it closed up. The compiler had
	 * been saying so -- `enumeration value 'SPACER' not handled in
	 * switch` -- in a warning that rode along with the build for as long
	 * as the token existed.
	 *
	 * A fixed width rather than a stretch, and the width of a key. The
	 * enum's own comment says what a spacer is for: "the geometry has to
	 * hold the space or every key after it shifts", which is a gap you
	 * can see and count rather than however much room happens to be
	 * spare. A stretch would also fight the row's own trailing stretch
	 * and share the slack with it, so a row with two gaps would space
	 * differently from one with one.
	 *
	 * No button, for the reason the soft keyboard gives at its own
	 * spacer: "a button that does nothing is worse than a gap -- it
	 * invites a press and answers with silence." Not appended to
	 * m_buttons either, which is the list the modifier refresh and the
	 * touch-height walk read: a spacer is not a key and must not be
	 * counted as one.
	 */
	QWidget *gap = new QWidget(m_keys);
	gap->setFixedWidth(SK_TOUCH_TARGET_DP);
	gap->setFocusPolicy(Qt::NoFocus);

	m_keys->layout()->addWidget(gap);
	gap->show();
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
	bssh_set_key_repeat(button);

	connect(button, &QAbstractButton::clicked, this, [this, ch, modifier] {
		if (!m_target) return;

		/*
		 * Sent as one event with the modifier attached, NOT by setting a
		 * sticky modifier and then the key: a sticky Ctrl the user
		 * already had set would combine with this one and send something
		 * nobody asked for. A chord is a single press with a single
		 * meaning.
		 */
		/*
		 * The KEY CODE -- Qt::Key_B for b -- not the character: a target
		 * reading a Ctrl letter takes it from the key, because a real
		 * keyboard's text is already the control character, and a key
		 * sent as text would arrive as a plain letter. Pressing the tmux
		 * prefix typed "b" into the shell when it went that way.
		 */
		const int code = QChar(char16_t(ch)).toUpper().unicode();
		m_target->key(code, quint8(modifier & (SK_MOD_CTRL | SK_MOD_ALT)));
	});
}

QAbstractButton *bssh_key_row::make_button(const QString &label) {
	/*
	 * A key cap rather than a plain button, so a modifier on this row
	 * draws its state the way the built-in keyboard's does (sec 6.2).
	 */
	QToolButton *button = new bssh_key_cap(m_keys);
	button->setText(label);
	m_buttons.append(button);
	button->setMinimumSize(SK_TOUCH_TARGET_DP, SK_TOUCH_TARGET_DP);

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
	bssh_set_key_repeat(button);
	connect(button, &QAbstractButton::clicked, this, [this, key] {
		if (m_target) m_target->key(key, SK_MOD_NONE);
	});
}

void bssh_key_row::add_text(const QString &label, char32_t ch) {
	QAbstractButton *button = make_button(label);
	bssh_set_key_repeat(button);
	connect(button, &QAbstractButton::clicked, this, [this, ch] {
		if (!m_target) return;
		/* Through the target, so an armed Ctrl applies to these too. */
		m_target->text(QString::fromUcs4(&ch, 1));
	});
}

void bssh_key_row::add_modifier(const QString &label, quint8 modifier) {
	QAbstractButton *button = make_button(label);
	button->setCheckable(true);
	button->setProperty("bssh_hold_modifier", uint(modifier));

	m_modifier_buttons.append(button);
	m_modifier_values.append(modifier);

	connect(button, &QAbstractButton::clicked, this, [this, modifier] {
		/*
		 * One press arms, a second locks, a third releases -- the Treo's
		 * cycle, and the router owns it because the built-in keyboard
		 * (sec 6.2) asks the same question with different buttons.
		 */
		if (m_target) m_target->modifiers().cycle(modifier);

		/*
		 * Refreshed even with no router, and that is the whole point of
		 * doing it outside the guard. The button is CHECKABLE, so Qt has
		 * already toggled it by the time this runs; returning early
		 * here left it drawn as pressed with no terminal behind it and
		 * nothing able to put it back.
		 */
		refresh_modifier_buttons();
	});
}

void bssh_key_row::refresh_modifier_buttons() {
	/*
	 * No router means no session, which means nothing is armed -- so the
	 * buttons are drawn OFF rather than left as they were.
	 *
	 * Returning early here was the bug this function's own comment warns
	 * about, one step further out: a row whose visibility does not depend
	 * on a session at all (see should_be_visible) kept a locked Ctrl on
	 * screen after the session behind it closed.
	 */
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
		const sk_modifiers::state_t state =
		        m_target ? m_target->modifiers().state(m_modifier_values.at(i))
		                 : sk_modifiers::OFF;

		button->setChecked(state != sk_modifiers::OFF);

		/*
		 * Named on the button as well, so a test can ask which of the
		 * three it is. setChecked cannot answer that -- two of the
		 * states share it, which is the whole difficulty.
		 */
		button->setProperty("bssh_modifier_state", int(state));
		button->update();

		QFont font = button->font();
		if (font.bold() != (state == sk_modifiers::LOCKED)) {
			font.setBold(state == sk_modifiers::LOCKED);
			button->setFont(font);
		}
	}
}

bool bssh_key_row::should_be_visible() const {
	if (m_mode == ALWAYS) return true;
	if (m_mode == NEVER) return false;
	if (m_touch_primary) return m_touch_primary();
	for (const QInputDevice *device : QInputDevice::devices()) {
		if (device->type() == QInputDevice::DeviceType::TouchScreen) return true;
	}
	return false;
}

void bssh_key_row::set_target(sk_target *target) {
	if (m_target == target) return;

	/* A finger still down belongs to the old session; let it go there. */
	if (m_touch) m_touch->cancel();

	if (m_target) disconnect(&m_target->modifiers(), nullptr, this, nullptr);
	m_target = target;

	if (m_target) {
		connect(&m_target->modifiers(), &sk_modifiers::armed_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
		/*
		 * Both halves, because a lock survives the keystroke that spends
		 * the armed one -- so a listener watching only the armed set
		 * would stop redrawing exactly when the state stopped being
		 * obvious.
		 */
		connect(&m_target->modifiers(), &sk_modifiers::locked_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
		connect(&m_target->modifiers(), &sk_modifiers::held_changed,
		         this, [this](quint8) { refresh_modifier_buttons(); });
	}

	refresh_modifier_buttons();
}

void bssh_key_row::set_visibility_mode(visibility_t mode) {
	m_mode = mode;
	setVisible(should_be_visible());
}
