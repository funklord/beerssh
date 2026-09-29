#include "ui/theme.h"
#include "term/colour_scheme.h"

#include <QApplication>
#include <QStyleHints>

#include "platform/platform.h"

namespace {

bssh_theme::mode_t g_mode = bssh_theme::MODE_SYSTEM;
bool g_following = false;

/*
 * A palette built from a handful of colours rather than tweaked from
 * whatever Qt handed us.
 *
 * Starting from the current palette and darkening it sounds cheaper and
 * is not reproducible: the starting point differs between a Fusion
 * session, a GTK-themed one and Android, so the same setting would give
 * three different results. These are the values, and they are the same
 * everywhere the app runs.
 */
QPalette build_dark() {
	QPalette p;

	const QColor window(0x1f, 0x21, 0x24);
	const QColor base(0x17, 0x19, 0x1b);
	const QColor text(0xdc, 0xdc, 0xdc);
	const QColor highlight(0x2b, 0x90, 0xd9);   /* the add button's blue (sec 8.5) */

	p.setColor(QPalette::Window, window);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, base);
	p.setColor(QPalette::AlternateBase, window.lighter(115));
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, window.lighter(115));
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::ToolTipBase, base);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
	p.setColor(QPalette::Link, highlight.lighter(120));
	p.setColor(QPalette::Highlight, highlight);
	p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
	p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8f, 0x95));

	/*
	 * Disabled text needs its own colour in both palettes. Qt derives one
	 * by blending toward the window colour, which on a dark palette lands
	 * close enough to the enabled colour to make a greyed-out control
	 * look enabled.
	 */
	const QColor disabled(0x6b, 0x70, 0x76);
	p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
	p.setColor(QPalette::Disabled, QPalette::Text, disabled);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
	p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

	return p;
}

QPalette build_light() {
	QPalette p;

	const QColor window(0xef, 0xef, 0xef);
	const QColor base(0xff, 0xff, 0xff);
	const QColor text(0x1c, 0x1c, 0x1c);
	const QColor highlight(0x2b, 0x90, 0xd9);

	p.setColor(QPalette::Window, window);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, base);
	p.setColor(QPalette::AlternateBase, window.darker(105));
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, window);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::ToolTipBase, base);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::BrightText, QColor(0xc0, 0x00, 0x00));
	p.setColor(QPalette::Link, highlight.darker(115));
	p.setColor(QPalette::Highlight, highlight);
	p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
	p.setColor(QPalette::PlaceholderText, QColor(0x77, 0x77, 0x77));

	const QColor disabled(0x9a, 0x9a, 0x9a);
	p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
	p.setColor(QPalette::Disabled, QPalette::Text, disabled);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
	p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

	return p;
}

bssh_appearance_t g_applied = BSSH_APPEARANCE_UNKNOWN;

void repaint_everything(bssh_appearance_t appearance) {
	/*
	 * Nothing to do if it is already what is on screen. Repainting every
	 * widget for no change is wasteful; more importantly, setPalette
	 * raises the very application palette event the platforms watch to
	 * notice a SYSTEM change, so an unguarded re-apply is a loop waiting
	 * for a platform that reports both through one channel.
	 */
	if (appearance == g_applied) return;
	g_applied = appearance;

	/*
	 * The terminal default follows the app (sec 6.2). The views are told
	 * by appearance_applied rather than by the palette change, which was
	 * tried first and did not reach a terminal view that was not shown.
	 */
	bssh_colour_scheme_catalog::instance().set_light_default(appearance == BSSH_APPEARANCE_LIGHT);

	QApplication::setPalette(bssh_theme::palette_for(appearance));
	emit bssh_theme::instance()->appearance_applied();

	/*
	 * Qt is told as well, so anything that asks IT rather than us gets
	 * the same answer -- a widget styled from styleHints would otherwise
	 * disagree with the palette around it. Ignored on the platforms where
	 * it does nothing, which is why it is not the mechanism.
	 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	if (QGuiApplication::styleHints()) {
		QGuiApplication::styleHints()->setColorScheme(
		        appearance == BSSH_APPEARANCE_DARK ? Qt::ColorScheme::Dark
		                                            : Qt::ColorScheme::Light);
	}
#endif
}

} /* namespace */

bssh_theme *bssh_theme::instance() {
	static bssh_theme *theme = new bssh_theme();
	return theme;
}

bssh_appearance_t bssh_theme::resolved_system_appearance() {
	const bssh_platform *platform = bssh_platform::instance();
	const bssh_appearance_t detected =
	        platform ? platform->system_appearance() : BSSH_APPEARANCE_UNKNOWN;

	/*
	 * Light when nothing will say. A guess is unavoidable at this point,
	 * and guessing dark paints white text into whatever the platform
	 * actually drew if it turns out to be light -- the more damaging way
	 * round to be wrong.
	 */
	return detected == BSSH_APPEARANCE_UNKNOWN ? BSSH_APPEARANCE_LIGHT : detected;
}

bssh_appearance_t bssh_theme::effective_appearance(mode_t mode) {
	switch (mode) {
	case MODE_LIGHT: return BSSH_APPEARANCE_LIGHT;
	case MODE_DARK:  return BSSH_APPEARANCE_DARK;
	case MODE_SYSTEM: break;
	}
	return resolved_system_appearance();
}

QPalette bssh_theme::palette_for(bssh_appearance_t appearance) {
	return appearance == BSSH_APPEARANCE_DARK ? build_dark() : build_light();
}

void bssh_theme::apply(mode_t mode) {
	g_mode = mode;
	repaint_everything(effective_appearance(mode));

	if (g_following) return;
	g_following = true;

	/*
	 * Connected once and left connected, for both sources, because either
	 * may be the one that notices. Following only while the mode is
	 * MODE_SYSTEM is decided at the point of the change rather than by
	 * disconnecting, so switching back to System takes effect without
	 * re-subscribing.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	if (platform) {
		QObject::connect(platform, &bssh_platform::appearance_changed,
		                  qApp, [] {
			if (g_mode == MODE_SYSTEM) repaint_everything(resolved_system_appearance());
		});
	}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	if (QGuiApplication::styleHints()) {
		QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
		                  qApp, [](Qt::ColorScheme) {
			if (g_mode == MODE_SYSTEM) repaint_everything(resolved_system_appearance());
		});
	}
#endif
}

QString bssh_theme::to_text(mode_t mode) {
	switch (mode) {
	case MODE_LIGHT: return QStringLiteral("light");
	case MODE_DARK:  return QStringLiteral("dark");
	case MODE_SYSTEM: break;
	}
	return QStringLiteral("system");
}

bssh_theme::mode_t bssh_theme::from_text(const QString &text) {
	if (text.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) return MODE_LIGHT;
	if (text.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0) return MODE_DARK;
	return MODE_SYSTEM;
}
