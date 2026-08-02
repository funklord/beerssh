#include "ui/tab_status.h"

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

QColor colour_for(state_t state) {
	switch (state) {
	case CONNECTED:
		return QColor(0x2e, 0xcc, 0x71);
	case CONNECTING:
		return QColor(0xf1, 0xc4, 0x0f);
	case BELL:
		return QColor(0x2b, 0x90, 0xd9);
	case DISCONNECTED:
		return QColor(0x95, 0xa5, 0xa6);
	}
	return QColor(0x95, 0xa5, 0xa6);
}

} /* namespace bssh_tab_status */
