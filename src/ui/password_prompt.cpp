#include "ui/touch_metrics.h"
#include "ui/password_prompt.h"

#include <QAbstractButton>
#include <QApplication>
#include <QGuiApplication>
#include <QInputMethod>
#include <QKeyEvent>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>

#include "ui/wrapped_note.h"
#include <QLineEdit>
#include <QScreen>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "softkeys/keyboard.h"
#include "softkeys/keyboard_switch.h"

bssh_password_prompt::bssh_password_prompt(const QString &target, bool allow_remember,
                                            QWidget *parent)
    : QDialog(parent),
      m_field(new QLineEdit(this)),
      m_remember(new QCheckBox(QStringLiteral("Remember password"), this)) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *outer = new QVBoxLayout(this);

	/*
	 * The destination on its own line, and allowed to wrap.
	 *
	 * It used to be the label half of a form row, which made the dialog's
	 * width a function of how long the user's host name is: "Password for
	 * nabbe@10.0.2.2:2299:" plus a field is wider than a phone, and the
	 * parts that ran off the edge were the checkbox and the buttons.
	 */
	QLabel *prompt = bssh_wrapped_note(QStringLiteral("Password for %1").arg(target), this);
	outer->addWidget(prompt);

	m_note = bssh_wrapped_note(QString(), this);
	m_note->setObjectName(QStringLiteral("password-note"));
	m_note->hide();
	outer->addWidget(m_note);

	m_field->setEchoMode(QLineEdit::Password);

	/*
	 * Named for a screen reader, since the visible label above is a
	 * separate widget and nothing otherwise connects the two (sec 6.3).
	 */
	m_field->setAccessibleName(QStringLiteral("Password"));
	m_field->setAccessibleDescription(
	        QStringLiteral("Password for %1").arg(target));
	outer->addWidget(m_field);

	/*
	 * Unchecked by default: storing a credential is a decision, not a
	 * default.
	 */
	m_remember->setEnabled(allow_remember);
	outer->addWidget(m_remember);

	/*
	 * All the slack in ONE place, between the question and the buttons.
	 *
	 * There was none, and a vertical layout with nowhere to put spare
	 * height gives it to whichever widget will take it -- here the
	 * wrapped label, whose policy allows growth. On a phone-sized dialog
	 * that put roughly 200px of nothing above the label and 250px between
	 * the label and the field it belongs to, so a question and its answer
	 * box read as two unrelated things at opposite ends of the screen.
	 *
	 * Invisible on the desktop, where the dialog is only as tall as its
	 * contents and there is no slack to misplace. Found by rendering it
	 * at 360x640, which is the size it actually appears at on the
	 * platform that has to type into it.
	 */
	outer->addStretch(1);

	QDialogButtonBox *buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_buttons = buttons;
	m_outer = outer;
	outer->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	/*
	 * Sec 6.2's 48dp floor, which this dialog never had.
	 *
	 * Sec 11 reshaped it once already, for WIDTH: the destination sat in
	 * a form layout's label column and demanded 508px on a 360px screen,
	 * pushing OK off the edge. That was fixed and the heights were not
	 * looked at, so the field, the checkbox and both buttons stayed
	 * pointer-sized on the one dialog a user cannot avoid -- you cannot
	 * reach a password-authenticated host without it. A 20px checkbox is
	 * a target you miss, and the thing you miss it into is a password
	 * field.
	 *
	 * Gated on the input device rather than the OS, the same question the
	 * key row and the front door ask (sec 6.2, sec 8.5): a tablet with a
	 * keyboard and a laptop with a touchscreen are the two cases an OS
	 * check gets wrong in opposite directions.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	if (platform && platform->is_touch_primary()) {
		m_field->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		m_remember->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

		for (QAbstractButton *button : buttons->buttons()) {
			button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		}

		/*
		 * Its own softkeys, under the buttons (sec 6.2). Reported from a
		 * phone: this prompt could only be typed with Android's keyboard,
		 * the forms' keyboard living in another window.
		 */
		/*
		 * No minimum from the layout: it is taken from the wrapped lines'
		 * heights at a narrower width than the prompt gets, and held the
		 * window taller than its contents (2026-10-23). fit_to_screen()
		 * sizes it to what the layout needs at its real width.
		 */
		outer->setSizeConstraint(QLayout::SetNoConstraint);

		m_keyboard = new sk_keyboard(&m_target, this);
		m_keyboard->setObjectName(QStringLiteral("prompt-keyboard"));
		m_keyboard->set_pages({ QStringLiteral("letters"), QStringLiteral("numbers"),
		                        QStringLiteral("editing") });
		m_keyboard->set_system_key_shown(true);
		m_keyboard->hide();
		outer->addWidget(m_keyboard);

		/*
		 * The screen's width and its own height, at the bottom while its
		 * keys are up -- placed in showEvent, where the screen is known. It was maximized, because
		 * as a small dialog over the terminal its keyboard read as a
		 * second, smaller one beside the terminal's (reported from the
		 * phone). The holder found a whole screen for one field too much
		 * (2026-10-22), so the width stays -- the keyboard is the
		 * terminal's width -- and the window puts the terminal's keyboard
		 * away while this is open (ask_for_password), which is what made
		 * it the second one.
		 */

		/*
		 * OK, Cancel and the checkbox take no focus on touch. Taking it
		 * from the field hid the keyboard -- ours, or the system's -- on
		 * the PRESS, the dialog shrank under the finger, and the release
		 * landed off the button: OK did nothing the first time and worked
		 * the second, when nothing was left to move (the holder,
		 * 2026-10-22). The field keeps the focus, so nothing moves.
		 */
		for (QAbstractButton *button : buttons->buttons()) button->setFocusPolicy(Qt::NoFocus);
		m_remember->setFocusPolicy(Qt::NoFocus);

		/*
		 * The checkbox at the left of the buttons' row rather than a row
		 * of its own: its 48dp row stood between the field and the
		 * buttons with its text centred in it, and read as a gap (the
		 * holder, 2026-10-23). The buttons are taken out of the outer
		 * layout into a row with it; place_remember() puts it back on a
		 * row of its own where a screen is too narrow for both.
		 * "Remember password" rather than "Remember this password", so
		 * the row fits a 360dp phone: the longer label needed 363.
		 */
		const int at = outer->indexOf(buttons);
		outer->removeWidget(buttons);
		outer->removeWidget(m_remember);
		m_button_row = new QHBoxLayout();
		m_button_row->setContentsMargins(0, 0, 0, 0);
		m_button_row->addWidget(m_remember);
		m_button_row->addStretch(1);
		m_button_row->addWidget(buttons);
		/* Where the buttons were, one place up: the checkbox left. */
		outer->insertLayout(at - 1, m_button_row);

		m_switch = new sk_keyboard_switch(this);
		connect(m_switch, &sk_keyboard_switch::switch_requested,
		         this, [this] { use_softkeys(true); });
		connect(m_keyboard, &sk_keyboard::system_keyboard_requested,
		         this, [this] { use_softkeys(false); });

		/* Hide leaves the field; the next tap on it brings the keys back. */
		connect(m_keyboard, &sk_keyboard::hide_requested, this, [this] {
			m_field->clearFocus();
			update_keyboard();
		});
		connect(qApp, &QApplication::focusChanged,
		         this, [this](QWidget *, QWidget *) { update_keyboard(); });
	}

	m_field->setFocus();
}

/*
 * The keyboard goes before the target it types into, and the focus
 * listener before either: deleting the children moves the focus.
 */
bssh_password_prompt::~bssh_password_prompt() {
	disconnect(qApp, &QApplication::focusChanged, this, nullptr);
	delete m_keyboard;
	m_keyboard = nullptr;
}

void bssh_password_prompt::set_note(const QString &note) {
	m_note->setText(note);
	m_note->setVisible(!note.isEmpty());
}

void bssh_password_prompt::use_softkeys(bool use) {
	m_softkeys = use;
	update_keyboard();
	if (!m_keyboard || !isVisible()) return;

	if (use) QGuiApplication::inputMethod()->hide();
	else if (m_field->hasFocus()) QGuiApplication::inputMethod()->show();
}

/*
 * Shown while the field has the focus and softkeys is the choice; the
 * switch while it is not. Android's keyboard is kept down the way the
 * forms keep it down, through autoSipEnabled, and given back on close.
 */
void bssh_password_prompt::update_keyboard() {
	if (!m_keyboard || !m_switch) return;

	const bool ours = m_softkeys && isVisible();
	if (isVisible()) qApp->setAutoSipEnabled(!m_softkeys);
	m_keyboard->setVisible(ours && m_field->hasFocus());
	m_switch->set_offered(isVisible() && !m_softkeys);
	fit_to_screen();
}

/*
 * The screen's full width, as tall as what it holds: at the bottom with
 * its own keyboard, at the top with the system's, which comes up below
 * it without covering the field. Re-fitted as either comes and goes.
 */
void bssh_password_prompt::fit_to_screen() {
	if (!m_keyboard || !isVisible() || !screen()) return;

	/*
	 * Resized, not given a minimum: a minimum outlives the screen it was
	 * measured on, and a fold or a turn changes the width.
	 */
	const QRect room = screen()->availableGeometry();
	int wide = room.width();

	/*
	 * The platform's screen size where it has one, as every other sizing
	 * decision in the app takes it -- so a test that sets a phone's gets
	 * a phone's prompt -- and never past the real screen.
	 */
	if (const bssh_platform *platform = bssh_platform::instance()) {
		const QSize dp = platform->screen_size();
		if (dp.isValid() && dp.width() > 0) wide = qMin(wide, dp.width());
	}
	/*
	 * The margins added by hand. Measured on the Fold (2026-10-23): the
	 * safe area arrived as a 31dp top margin after the prompt had sized
	 * itself, and sizeHint() stayed 444 with it -- it does not count
	 * the widget's own contents margins -- so the content moved down by
	 * 31 and the bottom 31 were cut off: the buttons, or the keyboard's
	 * last row. Re-fitting on ContentsRectChange was right and had
	 * nothing to add until this did.
	 */
	place_remember(wide);

	const QMargins margins = contentsMargins();
	int tall = qMax(sizeHint().height(), minimumSizeHint().height())
	           + margins.top() + margins.bottom();

	/*
	 * At the width it is about to have. The destination line wraps, and
	 * sizeHint() and minimumSizeHint() measure it at a default width
	 * narrower than a phone's, so they ask for lines it does not use; the
	 * spare height went to the stretch, an empty band between the checkbox
	 * and the buttons (seen on the Fold, 2026-10-23). totalHeightForWidth
	 * counts the widget's own margins too, which the margin fix above had
	 * to add by hand. The layout's own minimum is lifted on touch, below,
	 * or it would hold the window at the wrong height anyway.
	 */
	if (layout() && layout()->hasHeightForWidth()) tall = layout()->totalHeightForWidth(wide);
	resize(wide, qMin(tall, room.height()));

	/*
	 * With its own keys up, on the bottom edge, so they come up where the
	 * terminal's were rather than jumping to the top of the screen with
	 * the prompt (the holder, 2026-10-23) -- the field just above them, as
	 * Android puts a field above its keyboard. With Android's keyboard it
	 * stays at the top, where that keyboard cannot cover it.
	 */
	if (m_keyboard->isVisible()) move(room.left(), room.bottom() + 1 - frameGeometry().height());
	else move(room.topLeft());
}

/*
 * Beside the buttons where the row has room for the checkbox's whole
 * label and both buttons; on a row of its own above them where it does
 * not, as on a screen narrower than the Fold's cover. A checkbox does
 * not wrap, so a row that did not fit would push the prompt past the
 * screen's edge -- which is how OK went off it once before (sec 11).
 */
void bssh_password_prompt::place_remember(int wide) {
	if (!m_button_row) return;

	int left = 0, top = 0, right = 0, bottom = 0;
	m_outer->getContentsMargins(&left, &top, &right, &bottom);
	const QMargins margins = contentsMargins();
	const int need = m_remember->sizeHint().width() + m_button_row->spacing()
	                 + m_buttons->sizeHint().width() + left + right
	                 + margins.left() + margins.right();
	const bool beside = need <= wide;
	const bool is_beside = m_button_row->indexOf(m_remember) >= 0;
	if (beside == is_beside) return;

	if (beside) {
		m_outer->removeWidget(m_remember);
		m_button_row->insertWidget(0, m_remember);
	} else {
		m_button_row->removeWidget(m_remember);
		/* Under the field, above the stretch, as the desktop has it. */
		m_outer->insertWidget(m_outer->indexOf(m_field) + 1, m_remember);
	}
}

void bssh_password_prompt::showEvent(QShowEvent *event) {
	QDialog::showEvent(event);
	update_keyboard();

	/*
	 * The system keyboard, asked for: the field had the focus and no
	 * keyboard came up until it was tapped (seen on the phone,
	 * 2026-10-22). A dialog's focus is not a request for the input panel
	 * on Android; a tap is, and the first one should not have to be.
	 * Once the window is up, which is when Android will listen.
	 */
	if (m_keyboard && !m_softkeys) {
		QTimer::singleShot(0, this, [this] {
			if (isVisible() && m_field->hasFocus()) QGuiApplication::inputMethod()->show();
		});
	}
}

/*
 * Re-fitted when the content area moves. On Android Qt applies the safe
 * area -- the camera's band at the top, the system keyboard at the
 * bottom -- as contents margins AFTER the dialog has sized itself, so the
 * size taken in showEvent left out the top margin and OK and Cancel were
 * cut off at the bottom (seen on the phone, 2026-10-22).
 */
/*
 * Android's Back is Cancel here, as it leaves every other page. The prompt
 * ignored it, so Android took it and sent the app to the background with
 * the prompt still open and the connection waiting (seen on the Fold,
 * 2026-10-23).
 */
void bssh_password_prompt::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Back) {
		reject();
		event->accept();
		return;
	}
	QDialog::keyPressEvent(event);
}

bool bssh_password_prompt::event(QEvent *event) {
	const bool handled = QDialog::event(event);
	if (event->type() == QEvent::ContentsRectChange) fit_to_screen();
	return handled;
}

/* After closing, since closing moves the focus and that re-applies it. */
void bssh_password_prompt::done(int result) {
	QDialog::done(result);
	if (m_keyboard) qApp->setAutoSipEnabled(true);
}

QString bssh_password_prompt::password() const {
	return m_field->text();
}

bool bssh_password_prompt::remember() const {
	return m_remember->isEnabled() && m_remember->isChecked();
}
