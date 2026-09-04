#include "ui/key_cap.h"

#include <QStyleOptionToolButton>
#include <QStylePainter>

bssh_key_cap::bssh_key_cap(QWidget *parent) : QToolButton(parent) {}

bool bssh_key_cap::needs_own_paint(const QString &text, const QString &hint) {
	return !hint.isEmpty() || text.contains(QLatin1Char('&'));
}

void bssh_key_cap::set_hint(const QString &hint) {
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

void bssh_key_cap::paintEvent(QPaintEvent *event) {
	if (!needs_own_paint(text(), m_hint)) {
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
	painter.setFont(font());
	painter.setPen(ink);
	painter.drawText(box, Qt::AlignCenter, primary);
}
