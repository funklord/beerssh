#include "keyboard/key_cap.h"

#include <QFontMetrics>
#include <QPainter>
#include <QStyleOptionToolButton>
#include <QStylePainter>

#include "keyboard/sk_modifiers.h"

sk_key_cap::sk_key_cap(QWidget *parent) : QToolButton(parent) {}

/*
 * 500ms before the first repeat, then twenty a second.
 *
 * The delay is the load-bearing number and it is chosen against the
 * TAP, not against a physical keyboard: a deliberate tap on a phone is
 * well under 200ms, so half a second cannot be reached by anyone who
 * meant to press once. X11's own default is 660ms, which is safer still
 * and too slow to be worth the extra 160ms on a key somebody is holding
 * because they want twenty of it.
 *
 * The interval is Qt's own default rather than X11's 25ms, because each
 * repeat here is a round trip through the router into the emulator and
 * out to the remote, and a rate that outruns the connection just queues
 * deletions the user cannot see happening.
 */
void sk_set_key_repeat(QAbstractButton *button) {
	button->setAutoRepeat(true);
	button->setAutoRepeatDelay(500);
	button->setAutoRepeatInterval(50);
}

bool sk_key_cap::needs_own_paint(const QString &text, const QString &hint) {
	return !hint.isEmpty() || text.contains(QLatin1Char('&'));
}

void sk_key_cap::set_hint(const QString &hint) {
	if (m_hint == hint) return;

	m_hint = hint;

	/*
	 * Said aloud rather than only drawn. A sighted user learns the third
	 * level by reading the corner of the cap; without this a screen
	 * reader user has no way to discover it at all, which would make the
	 * annotation an accessibility regression dressed as a feature.
	 */
	setAccessibleDescription(m_hint.isEmpty()
	                                 ? QString()
	                                 : QStringLiteral("AltGr: %1").arg(m_hint));

	update();
}

QFont sk_key_cap::fitted_font(const QFont &base, const QString &text, int width) {
	QFont chosen = base;
	if (text.isEmpty() || width <= 0) return chosen;

	/*
	 * The floor is 70 per cent of the widget's font. Below that a
	 * three-letter label on a 33dp key is smaller than the AltGr hint
	 * beside it, which reads as a mistake rather than as a smaller
	 * label -- so the last resort stays the style's, and the caller
	 * elides.
	 */
	const qreal floor = 0.7;
	for (int step = 0; step < 8; ++step) {
		if (QFontMetrics(chosen).horizontalAdvance(text) <= width) break;

		const qreal next = (chosen.pointSizeF() > 0 ? chosen.pointSizeF()
		                                             : chosen.pixelSize()) * 0.94;
		const qreal limit = (base.pointSizeF() > 0 ? base.pointSizeF()
		                                            : base.pixelSize()) * floor;
		if (next < limit) break;

		if (chosen.pointSizeF() > 0) chosen.setPointSizeF(next);
		else chosen.setPixelSize(qMax(1, int(next)));
	}
	return chosen;
}

/*
 * A modifier's state, drawn so that it cannot be mistaken (sec 6.2).
 *
 * This used to be the style's "checked" look for armed and the same plus
 * a bold label for locked. Reported from the phone as unclear, and it
 * was worse than unclear: whether a toggled button reads as pressed is
 * the platform style's choice, so on some styles armed and off were hard
 * to tell apart, and bold on a three-letter label is a difference you
 * have to already know to look for. The two states differ by what the
 * next letter will be, so they get differences of KIND:
 *
 *     off      the style's plain cap
 *     armed    a ring in the highlight colour and one short bar --
 *              one bar, one key
 *     locked   the whole cap filled with the highlight colour and a
 *              long bar, like the light on a Caps Lock key
 *     held     the ring alone over the pressed cap: a finger is on it
 *
 * Highlight and HighlightedText are a pair the theme sets together, so
 * the locked label is readable on its fill in either scheme.
 */
void sk_key_cap::paint_modifier(int state) {
	QStylePainter painter(this);
	QStyleOptionToolButton option;
	initStyleOption(&option);

	const QString label = option.text;
	option.text.clear();
	option.icon = QIcon();

	const QColor accent = option.palette.color(QPalette::Highlight);
	QColor ink = option.palette.color(QPalette::ButtonText);

	painter.setRenderHint(QPainter::Antialiasing);
	const qreal line = qMax(qreal(2), height() / qreal(16));
	const QRectF cap = QRectF(rect()).adjusted(line / 2 + 1, line / 2 + 1,
	                                           -line / 2 - 1, -line / 2 - 1);
	const qreal bar = qMax(qreal(3), height() / qreal(12));

	if (state == sk_modifiers::LOCKED) {
		painter.setPen(Qt::NoPen);
		painter.setBrush(accent);
		painter.drawRoundedRect(cap, 4, 4);
		ink = option.palette.color(QPalette::HighlightedText);
		painter.setBrush(ink);
		painter.drawRect(QRectF(cap.left() + cap.width() * 0.2, cap.bottom() - bar * 2,
		                        cap.width() * 0.6, bar));
	} else {
		painter.drawComplexControl(QStyle::CC_ToolButton, option);
		painter.setPen(QPen(accent, line));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(cap, 4, 4);
		if (state == sk_modifiers::ONCE) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(accent);
			painter.drawRect(QRectF(cap.center().x() - cap.width() * 0.1,
			                        cap.bottom() - bar * 2, cap.width() * 0.2, bar));
		}
	}

	const QRect box = rect().adjusted(2, 1, -2, -1);
	painter.setFont(fitted_font(font(), label, box.width()));
	painter.setPen(ink);
	painter.drawText(box, Qt::AlignCenter, label);
}

void sk_key_cap::paintEvent(QPaintEvent *event) {
	const int state = property("sk_modifier_state").toInt();
	if (state != sk_modifiers::OFF) {
		paint_modifier(state);
		return;
	}

	/*
	 * The style draws it when nothing here has to: no second legend, no
	 * ampersand for Qt to eat, and a label that fits. The last of those
	 * is why this is measured rather than asked of the text alone -- it
	 * is a property of the key's WIDTH, which changes with the screen.
	 */
	const int room = rect().width() - 6;
	const bool too_wide = QFontMetrics(font()).horizontalAdvance(text()) > room;

	if (!needs_own_paint(text(), m_hint) && !too_wide) {
		/*
		 * Nothing to add, so nothing is reimplemented: the overwhelming
		 * majority of keys take the style's own drawing, and a cap with
		 * no second legend must be indistinguishable from the buttons
		 * around it.
		 */
		QToolButton::paintEvent(event);
		return;
	}

	QStylePainter painter(this);
	QStyleOptionToolButton option;
	initStyleOption(&option);

	/*
	 * The frame from the style, the text from here. Clearing the label
	 * before drawing the control is what stops the style centring the
	 * primary legend on its own and this one drawing a second copy over
	 * it.
	 */
	const QString primary = option.text;
	option.text.clear();
	option.icon = QIcon();
	painter.drawComplexControl(QStyle::CC_ToolButton, option);

	QFont hint_font = font();
	const qreal hint_points = hint_font.pointSizeF() > 0
	                                  ? hint_font.pointSizeF() * 0.68
	                                  : hint_font.pixelSize() * 0.68;
	if (hint_font.pointSizeF() > 0) {
		hint_font.setPointSizeF(qMax(qreal(6), hint_points));
	} else {
		hint_font.setPixelSize(qMax(6, int(hint_points)));
	}

	/*
	 * Dimmed with ALPHA, and not with bssh_dim() from the colour scheme,
	 * which scales a colour toward black.
	 *
	 * That helper is right for what it was written for -- a terminal
	 * glyph on a background the program owns and knows is dark. A keycap
	 * is drawn on the desktop's button colour, which may be either, and
	 * scaling toward black would dim the legend on a dark theme and
	 * SHARPEN it on a light one, where the text is already dark. Alpha
	 * blends toward whatever is actually behind, so it means the same
	 * thing in both directions. harmonization.md records the general
	 * form: a colour meeting a ground the program did not choose.
	 *
	 * The group is not named either. initStyleOption() has already set
	 * the palette's current group from the widget's state, so asking for
	 * ButtonText plainly gives the disabled colour when disabled and the
	 * inactive one when the window is not focused -- where naming Active
	 * or Disabled by hand would have got two of those three right.
	 */
	QColor ink = option.palette.color(QPalette::ButtonText);
	QColor faded = ink;
	faded.setAlphaF(0.55);

	const QRect box = rect().adjusted(2, 1, -2, -1);

	painter.setFont(hint_font);
	painter.setPen(faded);
	painter.drawText(box, Qt::AlignRight | Qt::AlignBottom, m_hint);

	/*
	 * The primary is centred in the WHOLE cap, exactly as the style
	 * centres it on a cap with no hint. Anything else makes the letters
	 * wander: a hinted cap and an unhinted one sit side by side in every
	 * row, and a baseline that moves depending on whether a key happens
	 * to have a third level is worse than either position.
	 *
	 * The first version raised it by half the hint's height, with a
	 * comment claiming that was what kept the two in line. It was the
	 * opposite -- only the hinted caps moved -- and g, h, v and b, the
	 * four letters us(intl) leaves without a third level, stood visibly
	 * lower than the rest of their rows. No assertion could see it and
	 * the render did, on the phone and, once looked at properly, in the
	 * desktop shot as well.
	 *
	 * There is no collision to avoid. The hint is bottom-RIGHT and one
	 * character wide; a centred single glyph does not reach it even on
	 * the compact board's narrowest key.
	 */
	/*
	 * Shrunk to fit rather than elided, and elided only if shrinking to
	 * the floor still is not enough.
	 */
	const QFont sized = fitted_font(font(), primary, box.width());
	const QFontMetrics metrics(sized);
	const QString drawn = metrics.horizontalAdvance(primary) > box.width()
	        ? metrics.elidedText(primary, Qt::ElideRight, box.width())
	        : primary;

	painter.setFont(sized);
	painter.setPen(ink);
	painter.drawText(box, Qt::AlignCenter, drawn);
}
