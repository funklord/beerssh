#include <QEvent>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include "keyboard/soft_keyboard.h"

#include "keyboard/key_cap.h"
#include "keyboard/resize_grip.h"
#include "keyboard/touch_chords.h"

#include <QAbstractButton>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>

#include "keyboard/sk_modifiers.h"
#include "keyboard/sk_target.h"
#include "keyboard/sk_modifiers.h"
#include "keyboard/soft_keyboard_layout.h"
#include "keyboard/sk_metrics.h"

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

bssh_soft_keyboard::bssh_soft_keyboard(sk_target *target, QWidget *parent)
    : QWidget(parent),
      m_target(target),
      m_keys(nullptr),
      m_grid(nullptr),
      m_group_button(nullptr),
      m_settings_button(nullptr),
      m_hide_button(nullptr),
      m_script_button(nullptr),
      m_group(0),
      m_style(bssh_soft_keyboard_catalog::STYLE_COMPACT) {
	/*
	 * The resize bar on top, the keys below it: the top is the edge that
	 * faces the terminal when this keyboard sits underneath (sec 6.2).
	 */
	QVBoxLayout *column = new QVBoxLayout(this);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(0);
	m_top_grip = new bssh_resize_grip(Qt::Vertical, this);
	column->addWidget(m_top_grip, 0);

	QHBoxLayout *outer = new QHBoxLayout;
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);
	column->addLayout(outer, 1);
	m_outer = outer;

	connect(m_top_grip, &bssh_resize_grip::pressed, this, [this] {
		m_drag_from = m_keys->height();
	});
	connect(m_top_grip, &bssh_resize_grip::dragged, this, [this](int distance) {
		/*
		 * Up is taller: the bar is on top. Never below 1, which the
		 * floor then raises to the smallest keys: 0 means "not chosen"
		 * to set_bottom_height, so a hard drag down snapped the keyboard
		 * back to its natural size -- the opposite of what the finger did.
		 */
		set_bottom_height(qMax(1, m_drag_from - distance));
	});
	connect(m_top_grip, &bssh_resize_grip::released, this, [this] {
		emit size_chosen(m_style, m_bottom_height);
	});

	/*
	 * The split keyboard's halves (sec 6.2), built once and filled by
	 * rebuild(). Each is a key grid in a row of its own, so the right one
	 * can take the side buttons beside its keys while split.
	 */
	for (int side = 0; side < 2; ++side) {
		m_halves[side] = new QWidget(this);
		m_halves[side]->setObjectName(side ? QStringLiteral("keyboard-right")
		                                   : QStringLiteral("keyboard-left"));
		QHBoxLayout *row = new QHBoxLayout(m_halves[side]);
		row->setContentsMargins(0, 0, 0, 0);
		row->setSpacing(1);

		m_half_keys[side] = new QWidget(m_halves[side]);
		m_half_grids[side] = new QGridLayout(m_half_keys[side]);
		m_half_grids[side]->setContentsMargins(0, 0, 0, 0);
		m_half_grids[side]->setSpacing(1);
		row->addWidget(m_half_keys[side], 1);

		/*
		 * On the INNER edge, beside the terminal: the left half's right
		 * edge and the right half's left. Dragging toward the terminal
		 * widens the half, so the sign flips between them.
		 */
		m_half_grips[side] = new bssh_resize_grip(Qt::Horizontal, m_halves[side]);
		if (side) row->insertWidget(0, m_half_grips[side], 0);
		else row->addWidget(m_half_grips[side], 0);

		connect(m_half_grips[side], &bssh_resize_grip::pressed, this, [this] {
			m_drag_from = m_half_keys[0]->width();
			for (bssh_resize_grip *grip : m_half_grips) grip->set_lit(true);
		});
		connect(m_half_grips[side], &bssh_resize_grip::dragged, this, [this, side](int distance) {
			const int wanted = side ? m_drag_from - distance : m_drag_from + distance;
			const int widest = qMax(120, window()->width() * 2 / 5);
			set_half_width(qBound(120, wanted, widest));
		});
		connect(m_half_grips[side], &bssh_resize_grip::released, this, [this] {
			for (bssh_resize_grip *grip : m_half_grips) grip->set_lit(false);
			emit size_chosen(bssh_soft_keyboard_catalog::STYLE_SPLIT, m_half_width);
		});

		m_halves[side]->hide();
	}

	m_keys = new QWidget(this);
	m_grid = new QGridLayout(m_keys);
	m_grid->setContentsMargins(0, 0, 0, 0);
	m_grid->setSpacing(1);
	outer->addWidget(m_halves[0], 0);
	outer->addWidget(m_keys, 1);
	outer->addWidget(m_halves[1], 0);

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
	m_side = new QWidget(this);
	QVBoxLayout *side = new QVBoxLayout(m_side);
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
	m_settings_button->setMinimumWidth(SK_TOUCH_TARGET_DP);
	m_settings_button->setMinimumHeight(SK_TOUCH_TARGET_DP);
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
	m_script_button->setMinimumWidth(SK_TOUCH_TARGET_DP);
	m_script_button->setMinimumHeight(SK_TOUCH_TARGET_DP);
	m_script_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_script_button, 0);

	connect(m_script_button, &QAbstractButton::clicked, this, [this] {
		bssh_set_latin_mode(!bssh_latin_mode());
		rebuild_keys();
	});

	m_group_button = new QToolButton(this);
	m_group_button->setFocusPolicy(Qt::NoFocus);
	m_group_button->setMinimumWidth(SK_TOUCH_TARGET_DP);
	m_group_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	side->addWidget(m_group_button, 1);

	m_hide_button = new QToolButton(this);
	m_hide_button->setObjectName(QStringLiteral("keyboard-hide"));
	m_hide_button->setAccessibleName(QStringLiteral("Hide the keyboard"));
	m_hide_button->setToolTip(QStringLiteral("Hide the keyboard"));
	m_hide_button->setFocusPolicy(Qt::NoFocus);
	m_hide_button->setMinimumWidth(SK_TOUCH_TARGET_DP);
	m_hide_button->setMinimumHeight(SK_TOUCH_TARGET_DP);
	m_hide_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	side->addWidget(m_hide_button, 0);

	refresh_side_icons();

	outer->addWidget(m_side, 0);

	connect(m_group_button, &QAbstractButton::clicked, this, [this] {
		set_group(bssh_soft_keyboard_catalog::instance().next_index(m_group, m_style));
	});
	connect(m_settings_button, &QAbstractButton::clicked,
	         this, &bssh_soft_keyboard::settings_requested);
	connect(m_hide_button, &QAbstractButton::clicked,
	         this, &bssh_soft_keyboard::hide_requested);

	/*
	 * One handler for the whole keyboard, halves included: a chord can
	 * span them -- Ctrl under the left thumb, C under the right -- and
	 * a handler per half would not know the other's finger was down.
	 */
	m_touch = new bssh_touch_chords(this);
	m_touch->watch(m_halves[0]);
	m_touch->watch(m_halves[1]);

	/*
	 * Two thumbs (sec 6.2): a finger resting on a modifier holds it on
	 * the router, and lifting it with nothing pressed in between is a tap
	 * -- the same cycle a click on the button gives.
	 */
	connect(m_touch, &bssh_touch_chords::modifier_held, this, [this](quint8 modifier) {
		if (m_target) m_target->modifiers().hold(modifier);
		refresh_modifiers();
	});
	connect(m_touch, &bssh_touch_chords::modifier_released,
	         this, [this](quint8 modifier, bool cancelled) {
		if (m_target && m_target->modifiers().release(modifier) && !cancelled) {
			m_target->modifiers().cycle(modifier);
		}
		refresh_modifiers();
	});

	if (m_target) {
		connect(&m_target->modifiers(), &sk_modifiers::armed_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(&m_target->modifiers(), &sk_modifiers::locked_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(&m_target->modifiers(), &sk_modifiers::held_changed,
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

bssh_key_cap *bssh_soft_keyboard::make_button(const QString &label, QWidget *parent) {
	bssh_key_cap *button = new bssh_key_cap(parent);
	button->setText(label);

	/*
	 * Never takes focus, for the reason the key row gives: a key press
	 * that lands on a button instead of the terminal is a keystroke the
	 * remote never sees, and nothing tells the user why.
	 */
	button->setFocusPolicy(Qt::NoFocus);

	button->setMinimumHeight(SK_TOUCH_TARGET_DP);
	button->setMinimumWidth(BSSH_KEY_MIN_WIDTH_DP);
	button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	return button;
}

void bssh_soft_keyboard::add_key(const bssh_key_t &key, QGridLayout *grid, int row,
                                 int column) {
	/*
	 * A spacer holds its columns and draws nothing: the gap between the
	 * function banks, and the keys a terminal cannot send. No button is
	 * made, because a button that does nothing is worse than a gap -- it
	 * invites a press and answers with silence.
	 */
	if (key.kind == bssh_key_t::SPACER) return;

	bssh_key_cap *button = make_button(key.label, grid->parentWidget());
	grid->addWidget(button, row, column, 1, key.span);

	switch (key.kind) {
	case bssh_key_t::SPACER:
		/* Returned above; named so the switch stays exhaustive. */
		break;

	case bssh_key_t::KEY: {
		const int code = key.value;
		bssh_set_key_repeat(button);
		connect(button, &QAbstractButton::clicked, this, [this, code] {
			if (m_target) m_target->key(code, SK_MOD_NONE);
		});
		break;
	}
	case bssh_key_t::MODIFIER: {
		const quint8 modifier = key.modifier;
		button->setCheckable(true);
		button->setProperty("bssh_hold_modifier", uint(modifier));
		m_modifier_buttons.append(button);
		m_modifier_values.append(modifier);
		connect(button, &QAbstractButton::clicked, this, [this, modifier] {
			/* One press arms, a second locks, a third releases (sec 6.2). */
			if (m_target) m_target->modifiers().cycle(modifier);

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
			if (!m_target) return;

			/*
			 * Shift is applied HERE, to the character, and not sent as a
			 * modifier. That is what a shift key on a soft keyboard is:
			 * the button sends a different character. The router masks
			 * SHIFT out of send_text for the same reason, so doing it
			 * there as well would be asking twice and getting neither.
			 */
			const bool shifted =
			        m_target->modifiers().state(SK_MOD_SHIFT)
			                != sk_modifiers::OFF;

			const QChar sent = bssh_keyboard_char(key, shifted, altgr_armed());

			/*
			 * Offered to the application before it is typed, exactly as a
			 * physical keyboard's letter is: a chord the application keeps
			 * (Ctrl+Shift+C as copy) or a remap is its business, not a
			 * character to send. The letter as it will be typed, so a
			 * capital reads as Shift held.
			 */
			if (m_target->claim_letter(char32_t(sent.unicode()))) {
				refresh_letter_labels();
				return;
			}

			m_target->text(QString(sent));
			refresh_letter_labels();
		});
		break;
	}
	case bssh_key_t::CHORD: {
		const int code = key.value;
		const quint8 modifier = key.modifier;
		bssh_set_key_repeat(button);
		connect(button, &QAbstractButton::clicked, this, [this, code, modifier] {
			if (m_target) m_target->key(code, modifier);
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

	/*
	 * Retired, not deleted, because this can run INSIDE the grid's own
	 * activation: showing the keyboard lays the grid out, that resizes
	 * the window, and the window's resize asks for the style -- which
	 * lands here while QLayout::activate() is still walking the items
	 * this would free. valgrind showed both reads, the layout item and
	 * the cap; it was silent until a heap layout happened to put
	 * something that faults under the freed memory.
	 *
	 * Unparented now, so findChildren sees only the new keys the moment
	 * this returns; freed on the next turn of the event loop, after
	 * whatever is above this on the stack has finished with them.
	 */
	QList<QLayoutItem *> retired;
	for (QGridLayout *grid : { m_grid, m_half_grids[0], m_half_grids[1] }) {
		while (QLayoutItem *item = grid->takeAt(0)) {
			if (QWidget *widget = item->widget()) {
				widget->hide();
				widget->setParent(nullptr);
				widget->deleteLater();
			}
			retired.append(item);
		}
	}
	update_halves();
	if (!retired.isEmpty()) {
		QTimer::singleShot(0, [retired] { qDeleteAll(retired); });
	}

	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(m_style);
	if (m_group < 0 || m_group >= groups.size()) return;

	const bssh_soft_key_group_t &group = groups.at(m_group);

	if (m_style == bssh_soft_keyboard_catalog::STYLE_SPLIT) {
		lay_out_split(group);
	} else {
		for (int r = 0; r < group.rows.size(); ++r) {
			int column = 0;
			for (const bssh_key_t &key : group.rows.at(r)) {
				add_key(key, m_grid, r, column);
				column += key.span;
			}
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

	/* New keys are made at the default height; give them the chosen one. */
	apply_bottom_height();

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
		const sk_modifiers::state_t state =
		        m_target ? m_target->modifiers().state(m_modifier_values.at(i))
		                 : sk_modifiers::OFF;

		button->setChecked(state != sk_modifiers::OFF);
		button->setProperty("bssh_modifier_state", int(state));
		button->update();

		QFont font = button->font();
		if (font.bold() != (state == sk_modifiers::LOCKED)) {
			font.setBold(state == sk_modifiers::LOCKED);
			button->setFont(font);
		}
	}

	refresh_letter_labels();
}

/*
 * One group into two grids, each key in the half its middle falls in
 * (bssh_soft_keyboard_catalog::in_left_half). Rows keep their stagger: the
 * left half keeps each key's column, and the right half measures from the
 * leftmost column any right-hand key starts at, so the two halves are the
 * one keyboard pulled apart rather than two keyboards.
 *
 * Then two keys a split keyboard needs and a one-piece keyboard does not
 * (sec 6.2, asked for by the holder 2026-10-08), because each thumb now
 * reaches only its own half:
 *
 *   - the space bar on BOTH halves: the cut put it on the right, so the
 *     left thumb could not type a space;
 *   - Shift on both: the left half has it, and the right half's last key
 *     in that row gives up one key's width to a Shift at the outer edge,
 *     mirroring the left one.
 *
 * The second space bar is also what makes a key one width on either side.
 * Without it the left half spanned 10 columns and the right 13, and the
 * left keys came out a third wider, measured on the Fold; with it both are
 * 13. Equalising the counts explicitly was written too, and a sabotage
 * showed it changed nothing once the space bar was there, so it went --
 * the_split_keyboard_holds_every_key_once asserts the widths instead.
 *
 * The keys sit at the BOTTOM of each half, at their own height, with the
 * space above them empty: a half is as tall as the terminal, and a thumb
 * holding a phone sideways reaches the lower corners, not the middle.
 */
void bssh_soft_keyboard::lay_out_split(const bssh_soft_key_group_t &group) {
	struct placed_t {
		bssh_key_t key;
		int column;
	};
	/*
	 * Without the compact pages' digit row (bssh_soft_key_group_t::
	 * digit_row_first): it was asked for in portrait, and five rows of
	 * 48dp keys did not fit a landscape phone.
	 */
	const int skip = group.digit_row_first ? 1 : 0;
	const int rows = int(group.rows.size()) - skip;
	QList<QList<placed_t>> left(rows), right(rows);

	for (int r = 0; r < rows; ++r) {
		int column = 0;
		for (const bssh_key_t &key : group.rows.at(skip + r)) {
			if (bssh_soft_keyboard_catalog::in_left_half(column, key.span, group.columns)) {
				left[r].append({ key, column });
			} else {
				right[r].append({ key, column });
			}
			column += key.span;
		}
	}

	const auto is_space = [](const bssh_key_t &key) {
		return key.kind == bssh_key_t::TEXT && key.value == int(U' ');
	};
	const auto is_shift = [](const bssh_key_t &key) {
		return key.kind == bssh_key_t::MODIFIER && key.modifier == SK_MOD_SHIFT;
	};
	const auto end_of = [](const QList<placed_t> &row) {
		return row.isEmpty() ? 0 : row.last().column + row.last().key.span;
	};

	for (int r = 0; r < rows; ++r) {
		const bool left_space = std::any_of(left[r].cbegin(), left[r].cend(),
		                                    [&](const placed_t &p) { return is_space(p.key); });
		for (const placed_t &p : right[r]) {
			if (!left_space && is_space(p.key)) {
				left[r].append({ p.key, end_of(left[r]) });
				break;
			}
		}

		const bool left_shift = std::any_of(left[r].cbegin(), left[r].cend(),
		                                    [&](const placed_t &p) { return is_shift(p.key); });
		const bool right_shift = std::any_of(right[r].cbegin(), right[r].cend(),
		                                     [&](const placed_t &p) { return is_shift(p.key); });
		if (left_shift && !right_shift && !right[r].isEmpty() && right[r].last().key.span >= 4) {
			bssh_key_t shift;
			for (const placed_t &p : left[r]) {
				if (is_shift(p.key)) shift = p.key;
			}
			right[r].last().key.span -= 2;
			shift.span = 2;
			right[r].append({ shift, end_of(right[r]) });
		}
	}

	int right_origin = group.columns;
	int left_columns = 0;
	int right_end = 0;
	for (int r = 0; r < rows; ++r) {
		left_columns = qMax(left_columns, end_of(left[r]));
		if (!right[r].isEmpty()) right_origin = qMin(right_origin, right[r].first().column);
		right_end = qMax(right_end, end_of(right[r]));
	}
	const int right_columns = qMax(0, right_end - right_origin);

	/* Row 0 is the empty space above the keys; the keys start at 1. */
	for (int r = 0; r < rows; ++r) {
		for (const placed_t &p : left[r]) add_key(p.key, m_half_grids[0], 1 + r, p.column);
		for (const placed_t &p : right[r]) {
			add_key(p.key, m_half_grids[1], 1 + r, p.column - right_origin);
		}
	}

	for (QGridLayout *grid : m_half_grids) {
		const int used = grid == m_half_grids[0] ? left_columns : right_columns;
		for (int c = 0; c < BSSH_KEYBOARD_MAX_COLUMNS; ++c) {
			grid->setColumnStretch(c, c < used ? 1 : 0);
		}
		grid->setRowStretch(0, 1);
		for (int r = 0; r < rows; ++r) grid->setRowStretch(1 + r, 0);
	}
}

/*
 * Whether the halves show, and where the side buttons go. Split keeps the
 * side buttons on the right half's outer edge, where the bottom keyboard
 * has them; otherwise they go back beside the bottom grid.
 */
void bssh_soft_keyboard::update_halves() {
	const bool split = m_style == bssh_soft_keyboard_catalog::STYLE_SPLIT;

	QBoxLayout *right_row = qobject_cast<QBoxLayout *>(m_halves[1]->layout());
	if (split && m_side->parentWidget() != m_halves[1]) {
		right_row->addWidget(m_side, 0);
	} else if (!split && m_side->parentWidget() != this) {
		m_outer->addWidget(m_side, 0);
	}

	m_keys->setVisible(!split);
	const bool shown = split && !isHidden();
	for (QWidget *half : m_halves) half->setVisible(shown);

	m_top_grip->setVisible(m_grips_shown && !split);
	for (QWidget *grip : m_half_grips) grip->setVisible(m_grips_shown);
}

void bssh_soft_keyboard::set_half_width(int width_dp) {
	m_half_width = width_dp;
	for (QWidget *keys : m_half_keys) keys->setFixedWidth(width_dp);
}

void bssh_soft_keyboard::set_grips_shown(bool shown) {
	if (shown == m_grips_shown) return;
	m_grips_shown = shown;
	update_halves();
}

int bssh_soft_keyboard::row_count() const {
	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(m_style);
	if (m_group < 0 || m_group >= groups.size()) return 1;
	return qMax(1, int(groups.at(m_group).rows.size()));
}

void bssh_soft_keyboard::set_bottom_height(int height_dp) {
	if (height_dp > 0) {
		/*
		 * Between the smallest keys anybody may drag to and three fifths
		 * of the window: a keyboard taller than that leaves the terminal
		 * less than the keyboard, which is the problem the bar exists to
		 * solve rather than to cause.
		 */
		const int rows = row_count();
		const int lowest = rows * SMALLEST_KEY_DP + (rows - 1);
		const int highest = qMax(lowest, window()->height() * 3 / 5);
		height_dp = qBound(lowest, height_dp, highest);
	}
	m_bottom_height = qMax(0, height_dp);
	apply_bottom_height();
}

/*
 * The keys' minimum height follows the chosen height, down to
 * SMALLEST_KEY_DP; at 0 everything goes back to the 48dp floor and the
 * keys' own size. Applied again after every rebuild, since a rebuild makes
 * new keys at the default.
 */
void bssh_soft_keyboard::apply_bottom_height() {
	const bool chosen = m_bottom_height > 0
	                    && m_style != bssh_soft_keyboard_catalog::STYLE_SPLIT;
	int key = SK_TOUCH_TARGET_DP;
	if (chosen) {
		const int rows = row_count();
		key = qBound(SMALLEST_KEY_DP, (m_bottom_height - (rows - 1)) / rows,
		             SK_TOUCH_TARGET_DP);
		m_keys->setFixedHeight(m_bottom_height);
	} else {
		m_keys->setMinimumHeight(0);
		m_keys->setMaximumHeight(QWIDGETSIZE_MAX);
	}
	for (bssh_key_cap *cap : m_keys->findChildren<bssh_key_cap *>()) {
		cap->setMinimumHeight(key);
	}
}

void bssh_soft_keyboard::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);
	update_halves();
}

void bssh_soft_keyboard::hideEvent(QHideEvent *event) {
	QWidget::hideEvent(event);
	update_halves();
}

bool bssh_soft_keyboard::altgr_armed() const {
	return m_target
	        && m_target->modifiers().state(SK_MOD_ALTGR)
	                != sk_modifiers::OFF;
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
	const int size = qMax(16, int(SK_TOUCH_TARGET_DP * 0.55));

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
	const bool shifted = m_target
	        && m_target->modifiers().state(SK_MOD_SHIFT)
	                != sk_modifiers::OFF;

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

void bssh_soft_keyboard::set_target(sk_target *target) {
	if (m_target == target) return;

	/* A finger still down belongs to the old session; let it go there. */
	if (m_touch) m_touch->cancel();

	if (m_target) disconnect(&m_target->modifiers(), nullptr, this, nullptr);
	m_target = target;

	if (m_target) {
		connect(&m_target->modifiers(), &sk_modifiers::armed_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(&m_target->modifiers(), &sk_modifiers::locked_changed,
		         this, [this](quint8) { refresh_modifiers(); });
		connect(&m_target->modifiers(), &sk_modifiers::held_changed,
		         this, [this](quint8) { refresh_modifiers(); });
	}

	refresh_modifiers();
}
