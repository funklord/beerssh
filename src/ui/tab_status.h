#ifndef BSSH_TAB_STATUS_H
#define BSSH_TAB_STATUS_H

#include <QColor>

class bssh_session;

/*
 * The per-tab status dot (project.md sec 8.3).
 *
 * A pure mapping from a session to a state, kept apart from the painting
 * so the RULE can be tested: which state wins when a session is both
 * reconnecting and has an unheard bell is a decision, and one that would
 * otherwise only ever be checked by squinting at a tab bar.
 */
namespace bssh_tab_status {

enum state_t {
	CONNECTED,
	CONNECTING,
	BELL,        /* connected, and rang while you were looking elsewhere */
	DISCONNECTED
};

state_t state_for(const bssh_session *session);
QColor colour_for(state_t state);

} /* namespace bssh_tab_status */

#endif /* BSSH_TAB_STATUS_H */
