#ifndef BSSH_INTERFACE_SCALE_H
#define BSSH_INTERFACE_SCALE_H

#include <QFont>

/*
 * The application font scaled for the system font-scale accessibility
 * setting (project.md sec 9.5). Pure, so the arithmetic is tested without
 * an application: the glue that decides WHETHER to apply it -- the setting
 * and the platform's reported scale -- lives in main, and the number it
 * produces is tested there.
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

#endif /* BSSH_INTERFACE_SCALE_H */
