#ifndef BSSH_INTERFACE_SCALE_H
#define BSSH_INTERFACE_SCALE_H

#include <QFont>

/*
 * The application font scaled for the system font-scale accessibility
 * setting (project.md sec 9.5). Pure, so the arithmetic is tested without
 * an application: the glue that decides WHETHER to apply it -- the setting
 * and the platform's reported scale -- is bssh_apply_interface_scale below.
 *
 * A scale of 1.0, or one out of any sane range, returns the base font
 * untouched, so a broken or absent reading is a no-op rather than a
 * collapsed interface. Point and pixel sizing are both handled, because a
 * base font may carry either.
 */
inline QFont bssh_interface_font(const QFont &base, double scale) {
	if (scale <= 0.0 || qFuzzyCompare(scale, 1.0)) return base;

	QFont scaled = base;
	if (base.pointSizeF() > 0.0)
		scaled.setPointSizeF(base.pointSizeF() * scale);
	else if (base.pixelSize() > 0)
		scaled.setPixelSize(int(base.pixelSize() * scale + 0.5));

	return scaled;
}

/*
 * Sets the application font for the system font scale now (sec 9.5):
 * the font the application started with, scaled by the platform's reading
 * when "Follow the system font size" is on and left alone when it is off.
 *
 * Always from the STARTING font, never from the current one, so a change
 * from 1.3 to 1.1 lands on 1.1 rather than on 1.43. And only when the
 * scale differs from the one last applied, so a font something else set
 * -- a test, an accessibility tool -- is not reset by a call that had
 * nothing to change.
 */
void bssh_apply_interface_scale();

/*
 * Applies it, and again every time the application becomes active: a
 * font-size change is made in Android's Settings, so the app is in the
 * background while it happens and comes back to the front after. Call
 * once, at start, before any widget is built. Idempotent.
 */
void bssh_follow_interface_scale();

#endif /* BSSH_INTERFACE_SCALE_H */
