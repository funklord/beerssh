#include "ui/tab_status.h"

#include <QApplication>
#include <QPalette>

#include <QPainter>

#include "model/session.h"

namespace bssh_tab_status {

state_t state_for(const bssh_session *session) {
	if (!session) return DISCONNECTED;

	switch (session->state()) {
	case bssh_ssh_session::DISCONNECTED:
	case bssh_ssh_session::FAILED:
		/*
		 * Dead beats everything, including an unheard bell: a bell on a
		 * tab whose connection has dropped would invite the user to go
		 * read output that is no longer arriving.
		 */
		return DISCONNECTED;
	case bssh_ssh_session::CONNECTED:
		return session->has_unseen_bell() ? BELL : CONNECTED;
	default:
		/* Resolving, connecting, authenticating -- all "not yet". */
		return CONNECTING;
	}
}

/*
 * Two sets, same four hues, because these were chosen against a dark tab
 * bar and stopped working when sec 6.2's light theme arrived. Measured
 * against the light window colour, every one of them failed WCAG's 3:1
 * for a graphical indicator -- green 1.83:1, grey 2.22:1, and the
 * "connecting" yellow 1.44:1, which is as good as invisible. They are
 * the only thing on a tab that says whether a session is alive.
 *
 * The light variants are the same hue and saturation darkened until they
 * pass with a little headroom, so a user moving between themes still
 * reads green as green rather than learning two palettes.
 */
QColor colour_for(state_t state) {
	const bool light =
		      QApplication::palette().color(QPalette::Window).lightness() >= 128;

	switch (state) {
	case CONNECTED:
		return light ? QColor(0x22, 0x97, 0x54) : QColor(0x2e, 0xcc, 0x71);
	case CONNECTING:
		return light ? QColor(0xa0, 0x82, 0x09) : QColor(0xf1, 0xc4, 0x0f);
	case BELL:
		return light ? QColor(0x26, 0x89, 0xd1) : QColor(0x2b, 0x90, 0xd9);
	case DISCONNECTED:
		return light ? QColor(0x74, 0x89, 0x8b) : QColor(0x95, 0xa5, 0xa6);
	}
	return light ? QColor(0x74, 0x89, 0x8b) : QColor(0x95, 0xa5, 0xa6);
}

QPixmap dot(int size, state_t state) {
	size = qMax(4, size);

	QPixmap pixmap(size, size);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(colour_for(state));

	/*
	 * Inset by a pixel: an ellipse drawn to the exact bounds of its
	 * pixmap loses its antialiased edge to clipping and reads as a
	 * slightly square blob at tab-bar sizes.
	 */
	painter.drawEllipse(1, 1, size - 2, size - 2);
	return pixmap;
}

} /* namespace bssh_tab_status */
