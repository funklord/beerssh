#include "ui/interface_scale.h"

#include <QApplication>
#include <QStyle>
#include <QWidget>

#include "model/app_settings.h"
#include "platform/platform.h"

namespace {

/* The font the application had before any scale was applied. */
QFont starting_font() {
	static const QFont base = QApplication::font();
	return base;
}

/* The scale last applied; 1.0 is the starting font itself. */
double g_applied = 1.0;

/*
 * Carries the widgets QApplication::setFont() does not reach.
 *
 * Measured, not assumed: with only the application font changed, the
 * suite's host list kept its rows at 9pt under a scale of 1.5, and on the
 * Fold the host list kept its size while one menu glyph grew. Two kinds of
 * widget hold a font of their own and ignore the application's:
 *
 *   - one given a font with setFont() -- a title at 1.4 times the
 *     interface size, a bold heading, a locked modifier's bold label --
 *     which is rescaled so it stays 1.4 times whatever the interface now
 *     is. From the font it had the first time it was carried and the
 *     scale it was made at, never from its current font: Android's fonts
 *     are sized in pixels, which round at every step, and carried by
 *     ratio a 23px label taken 1.5 -> 1.3 -> 0.8 -> 1.15 -> 1.5 came back
 *     at 22. (A carried title can still differ by a pixel from one built
 *     fresh at the same scale, and the fresh one is the less exact: it is
 *     1.4 times the ROUNDED interface size, rounded again -- measured on
 *     the Fold, 13px fresh at 0.8 against 12px carried, 12.3 exact.);
 *   - one with a style sheet, which takes its font when polished and keeps
 *     it -- the host list's rows, styled for their separators -- which is
 *     polished again so it reads its parent's new font.
 *
 * A widget whose font is a fact rather than chrome sets the property
 * "bssh_fixed_font" and is left alone: the font picker's preview shows a
 * terminal font at the point size chosen for the terminal.
 */
void carry_widget_fonts(double from, double to) {
	for (QWidget *widget : QApplication::allWidgets()) {
		if (widget->property("bssh_fixed_font").toBool()) continue;

		if (widget->testAttribute(Qt::WA_SetFont)) {
			/*
			 * The basis is taken the first time a widget is carried: its
			 * font then, and the scale it was made at. Every later size is
			 * one multiplication from that, so rounding happens once.
			 */
			if (!widget->property("bssh_font_basis").isValid()) {
				widget->setProperty("bssh_font_basis", widget->font());
				widget->setProperty("bssh_font_basis_scale", from);
			}
			QFont font = widget->font();
			const QFont basis = widget->property("bssh_font_basis").value<QFont>();
			const double factor = to / widget->property("bssh_font_basis_scale").toDouble();
			if (basis.pointSizeF() > 0) {
				font.setPointSizeF(basis.pointSizeF() * factor);
			} else if (basis.pixelSize() > 0) {
				font.setPixelSize(qMax(1, qRound(basis.pixelSize() * factor)));
			}
			widget->setFont(font);
		} else if (!widget->styleSheet().isEmpty()) {
			widget->style()->unpolish(widget);
			widget->style()->polish(widget);
			widget->updateGeometry();
			widget->update();
		}
	}
}

} /* namespace */

void bssh_apply_interface_scale() {
	const QFont base = starting_font();

	double scale = 1.0;
	bssh_platform *platform = bssh_platform::instance();
	if (platform && bssh_app_settings().honour_system_font_scale()) {
		scale = platform->system_font_scale();
	}
	if (qFuzzyCompare(scale, g_applied)) return;

	const double from = g_applied;
	g_applied = scale;
	QApplication::setFont(bssh_interface_font(base, scale));
	carry_widget_fonts(from, scale);
}

void bssh_follow_interface_scale() {
	static bool following = false;
	if (following) return;
	following = true;

	bssh_apply_interface_scale();

	bssh_platform *platform = bssh_platform::instance();
	if (!platform) return;
	QObject::connect(platform, &bssh_platform::application_active_changed, platform, [platform] {
		if (platform->is_application_active()) bssh_apply_interface_scale();
	});
}
