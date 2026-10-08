#include "ui/interface_scale.h"

#include <QApplication>

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

} /* namespace */

void bssh_apply_interface_scale() {
	const QFont base = starting_font();

	double scale = 1.0;
	bssh_platform *platform = bssh_platform::instance();
	if (platform && bssh_app_settings().honour_system_font_scale()) {
		scale = platform->system_font_scale();
	}
	if (qFuzzyCompare(scale, g_applied)) return;

	g_applied = scale;
	QApplication::setFont(bssh_interface_font(base, scale));
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
