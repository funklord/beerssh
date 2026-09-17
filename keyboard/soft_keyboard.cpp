#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

#include <cmath>
#include "keyboard/soft_keyboard.h"

#include "keyboard/key_cap.h"

#include <QAbstractButton>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QToolButton>

#include "input/input_router.h"
#include "term/emulator.h"
#include "keyboard/soft_keyboard_layout.h"
#include "ui/touch_metrics.h"

namespace {

/*
 * The two side-column icons, DRAWN rather than set from a character.
 *
 * A gear at U+2699 and a chevron at U+25BE would be three lines of code
 * and a gamble: the label font is the system's, Android falls back
 * through it for symbols, and a device without the glyph draws a tofu
 * box. sec 11's rule is absent-not-faked, and a box where a control
 * should be is worse than the word it replaced. Qt's standard pixmaps
 * have no settings icon either.
 *
 * So they are painted from primitives, which cannot be missing, scale
 * with the button, and take their colour from the palette -- so they
 * follow the light and dark schemes without a second asset.
 */
QIcon bssh_painted_icon(const QColor &ink, int size, bool settings) {
	QPixmap canvas(size, size);
	canvas.fill(Qt::transparent);

	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::Antialiasing, true);

	QPen pen(ink);
	pen.setWidthF(qMax(1.5, size / 11.0));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	painter.setPen(pen);

	const qreal unit = size;
	if (settings) {
		/*
		 * A cog: the settings symbol everywhere a person has used a
		 * phone, so it needs no learning.
		 *
		 * This was three sliders, and the reasoning for them was
		 * backwards. Sliders were chosen because a gear's teeth are the
		 * first thing to turn to mush at 36dp -- an argument about
		 * RENDERING, offered against an argument about MEANING, and
		 * meaning wins: sliders read as a filter or an equaliser to most
		 * people, and an icon that has to be explained is the wrong
		 * icon. Reported by the copyright holder, who expected a cog.
		 *
		 * The rendering problem is real and is answered by drawing
		 * FEWER, FATTER teeth than a gear really has. Six radial strokes
		 * with round caps stay separate at any size this is drawn at; a
		 * finely-toothed gear becomes a blurred disc, which is what the
		 * old comment was afraid of and was right about.
		 */
		const QPointF centre(unit * 0.5, unit * 0.5);

		/*
		 * FILLED, not stroked, and that is the whole difference between
		 * a cog and a ship's wheel.
		 *
		 * The first attempt drew a stroked ring with six radial lines
		 * out of it. On a device it read as a helm or a sun: thin spokes
		 * STICKING OUT of a circle, where a gear's teeth are part of the
		 * rim. So the outline is one path -- out to the tooth, along it,
		 * back to the rim, around to the next -- filled solid, with the
		 * centre punched out by an odd-even subpath.
		 */
		/*
		 * Eight teeth with RADIAL sides, and the sides are what matter.
		 *
		 * The version before this walked from the rim at one angle out
		 * to the tip at another, so every tooth was a wedge that came to
		 * a point, and the whole read as a starfish -- reported as "not
		 * quite clear", which it was. A gear tooth has parallel flanks:
		 * both corners of its base sit at the SAME angles as the two
		 * corners of its tip, so the side runs straight out along a
		 * radius and the top is flat.
		 *
		 * Short teeth, too. The rim is 0.33 and the tip 0.42, about a
		 * quarter again rather than half, because a tall tooth on a
		 * small body is the other half of the starfish.
		 */
		const int teeth = 8;
		const qreal step = 360.0 / teeth;
		const qreal half = step * 0.20;
		const qreal outer = unit * 0.42;
		const qreal inner = unit * 0.33;

		const auto polar = [&](qreal degrees, qreal radius) {
			const qreal angle = qDegreesToRadians(degrees);
			return QPointF(centre.x() + std::cos(angle) * radius,
			                centre.y() + std::sin(angle) * radius);
		};

		/*
		 * Between the teeth the outline follows the root CIRCLE rather
		 * than a straight chord, which would flatten the body.
		 *
		 * arcTo's angles run the other way from polar(): Qt measures
		 * counter-clockwise from three o'clock and screen y grows
		 * downward, so a clockwise walk is negated.
		 */
		const QRectF root(centre.x() - inner, centre.y() - inner,
		                   inner * 2, inner * 2);

		QPainterPath gear;
		gear.moveTo(polar(-half, inner));
		for (int i = 0; i < teeth; ++i) {
			const qreal base = step * i;
			gear.lineTo(polar(base - half, inner));
			gear.lineTo(polar(base - half, outer));
			gear.lineTo(polar(base + half, outer));
			gear.lineTo(polar(base + half, inner));
			gear.arcTo(root, -(base + half), -(step - half * 2));
		}
		gear.closeSubpath();

		/* The hole. Without it a filled gear is just a lumpy disc. */
		gear.addEllipse(centre, unit * 0.15, unit * 0.15);
		gear.setFillRule(Qt::OddEvenFill);

		painter.setPen(Qt::NoPen);
		painter.fillPath(gear, ink);
	} else {
		/*
		 * A chevron coming down to a bar: the keyboard going away, which
		 * is what every platform's dismiss control draws.
		 */
		QPainterPath path;
		path.moveTo(unit * 0.26, unit * 0.34);
		path.lineTo(unit * 0.50, unit * 0.56);
		path.lineTo(unit * 0.74, unit * 0.34);
		painter.drawPath(path);
		painter.drawLine(QPointF(unit * 0.26, unit * 0.74),
		                  QPointF(unit * 0.74, unit * 0.74));
	}

	painter.end();
	return QIcon(canvas);
}



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
      m_script_button(nullptr),
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
	m_settings_button->setObjectName(QStringLiteral("keyboard-settings"));

	/*
	 * An icon, and the NAME is still said. A button whose label becomes
	 * a picture loses the text a screen reader was reading, so the word
	 * moves to the accessible name and the tooltip rather than being
	 * dropped -- the same trap the AltGr hint had, where what is drawn
	 * and what is announced are two different things.
	 */
	m_settings_button->setAccessibleName(QStringLiteral("Settings"));
	m_settings_button->setToolTip(QStringLiteral("Settings"));
	m_settings_button->setFocusPolicy(Qt::NoFocus);
	m_settings_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_settings_button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	m_settings_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_settings_button, 0);

	/*
	 * The script toggle, and it is only here for a layout that needs it.
	 *
	 * Russian, Greek and Ukrainian put their own script on the whole
	 * letter block, so a terminal opened under one of them had no a-z at
	 * all -- which is why they were refused rather than shipped. A Latin
	 * layout has no second script to reach, so it gets no button: an
	 * always-present control that does nothing on 27 of 30 layouts is
	 * worse than an absent one, and sec 11's absent-not-faked rule says
	 * which way to fall.
	 *
	 * In the side column rather than on a key, because it belongs to the
	 * KEYBOARD and not to a page -- the same argument that put settings
	 * and hide here, and it means the script can be changed from the
	 * numbers page without going back to the letters first.
	 */
	m_script_button = new QToolButton(this);
	m_script_button->setObjectName(QStringLiteral("keyboard-script"));
	m_script_button->setFocusPolicy(Qt::NoFocus);
	m_script_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_script_button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	m_script_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_script_button, 0);

	connect(m_script_button, &QAbstractButton::clicked, this, [this] {
		bssh_set_latin_mode(!bssh_latin_mode());
		rebuild_keys();
	});

	m_group_button = new QToolButton(this);
	m_group_button->setFocusPolicy(Qt::NoFocus);
	m_group_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_group_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	side->addWidget(m_group_button, 1);

	m_hide_button = new QToolButton(this);
	m_hide_button->setObjectName(QStringLiteral("keyboard-hide"));
	m_hide_button->setAccessibleName(QStringLiteral("Hide the keyboard"));
	m_hide_button->setToolTip(QStringLiteral("Hide the keyboard"));
	m_hide_button->setFocusPolicy(Qt::NoFocus);
	m_hide_button->setMinimumWidth(BSSH_TOUCH_TARGET_DP);
	m_hide_button->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
	m_hide_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_hide_button, 0);

	refresh_side_icons();

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

bssh_key_cap *bssh_soft_keyboard::make_button(const QString &label) {
	bssh_key_cap *button = new bssh_key_cap(m_keys);
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

	bssh_key_cap *button = make_button(key.label);
	m_grid->addWidget(button, row, column, 1, key.span);

	switch (key.kind) {
	case bssh_key_t::SPACER:
		/* Returned above; named so the switch stays exhaustive. */
		break;

	case bssh_key_t::KEY: {
		const int code = key.value;
		bssh_set_key_repeat(button);
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
		if (QChar::isLetter(uint(ch)) || key.shifted_value != 0
		     || key.altgr_value != 0) {
			m_letter_buttons.append(button);
			m_letter_keys.append(key);
		}

		bssh_set_key_repeat(button);
		connect(button, &QAbstractButton::clicked, this, [this, key] {
			if (!m_router) return;

			/*
			 * A chord first, exactly as a physical keyboard's letter chord
			 * is stolen before it becomes text (sec 6.1): if the armed
			 * modifiers make this letter Ctrl+Shift+C and the like, it is
			 * the app's, not a character for the shell.
			 */
			if (m_router->steal_char_chord(char32_t(key.value))) {
				refresh_letter_labels();
				return;
			}

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

			m_router->send_text(QString(bssh_keyboard_char(key, shifted, altgr_armed())));
			refresh_letter_labels();
		});
		break;
	}
	case bssh_key_t::CHORD: {
		const int code = key.value;
		const quint8 modifier = key.modifier;
		bssh_set_key_repeat(button);
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
	refresh_script_button();

	m_modifier_buttons.clear();
	m_modifier_values.clear();
	m_letter_buttons.clear();
	m_letter_keys.clear();

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

bool bssh_soft_keyboard::altgr_armed() const {
	return m_router
	        && m_router->modifier_state(bssh_input_router::MOD_ALTGR)
	                != bssh_input_router::MODIFIER_OFF;
}

/*
 * Paint the side icons for the CURRENT palette.
 *
 * Called again on a palette change, because the icons carry their colour
 * rather than taking it from the widget when drawn: a theme switch would
 * otherwise leave a dark glyph on a dark button, which is the failure
 * bssh_key_cap avoids by using alpha and this cannot, an icon being
 * pixels.
 */
/*
 * Show, hide and relabel the script toggle.
 *
 * The label says what the button will GIVE you, the way the page button
 * names the page it will move to: under Russian in Latin mode it reads
 * RU, and pressing it reads abc.
 *
 * The id in capitals rather than a sample of the script, which was tried
 * and does not generalise. Three letters of the layout's own AD row is
 * idiomatic for Cyrillic -- YTsU is to a Russian keyboard what QWERTY is
 * to this one -- and wrong for Greek, whose AD row opens on a semicolon
 * and a final sigma. A two-letter id is short, fits the column, and is
 * the same string the setting uses.
 */
void bssh_soft_keyboard::refresh_script_button() {
	if (!m_script_button) return;

	const bool offered = !bssh_layout_has_latin();
	m_script_button->setVisible(offered);
	if (!offered) return;

	const QString id = bssh_keyboard_layout();

	QString named = id;
	for (const bssh_keyboard_layout_t &known : bssh_keyboard_layouts()) {
		if (known.id == id) named = known.display_name;
	}

	const bool latin = bssh_latin_mode();
	m_script_button->setText(latin ? id.toUpper() : QStringLiteral("abc"));

	const QString says = latin
	        ? QStringLiteral("Switch to %1 letters").arg(named)
	        : QStringLiteral("Switch to Latin letters");
	m_script_button->setAccessibleName(says);
	m_script_button->setToolTip(says);
}

void bssh_soft_keyboard::refresh_side_icons() {
	if (!m_settings_button || !m_hide_button) return;

	const QColor ink = palette().color(QPalette::ButtonText);
	const int size = qMax(16, int(BSSH_TOUCH_TARGET_DP * 0.55));

	m_settings_button->setIcon(bssh_painted_icon(ink, size * 2, true));
	m_settings_button->setIconSize(QSize(size, size));

	m_hide_button->setIcon(bssh_painted_icon(ink, size * 2, false));
	m_hide_button->setIconSize(QSize(size, size));
}

void bssh_soft_keyboard::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event->type() == QEvent::PaletteChange) refresh_side_icons();
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

	const bool altgr = altgr_armed();

	for (int i = 0; i < m_letter_buttons.size(); ++i) {
		const bssh_key_t &key = m_letter_keys.at(i);
		const QChar active = bssh_keyboard_char(key, shifted, altgr);

		/*
		 * ANNOTATED while AltGr is idle, because US International is
		 * unfamiliar to most people and a third legend nobody can see is
		 * a feature nobody finds.
		 *
		 * The cap always reads, prominently, the character it will
		 * actually send: the plain letter now, the accented one once
		 * AltGr is armed. The third level rides along as a HINT in the
		 * corner -- smaller and dimmer, the way it is printed on a real
		 * keycap -- and goes away when AltGr arms it, because a hint
		 * repeating what the user has just done is forty caps' worth of
		 * reading at the moment they are hunting for a character.
		 * bssh_key_cap draws it; see there for why it is a paint rather
		 * than a longer string.
		 */
		m_letter_buttons.at(i)->setText(QString(active));
		m_letter_buttons.at(i)->set_hint(
		        !altgr && key.altgr_value != 0
		                ? QString(QChar(uint(key.altgr_value)))
		                : QString());
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
