#ifndef BSSH_TAB_STATUS_H
#define BSSH_TAB_STATUS_H

#include <QColor>
#include <QPixmap>

class bssh_session;

/*
 * The per-tab status dot (project.md sec 8.3).
 *
 * A pure mapping from a session to a state, kept apart from the painting
 * so the RULE can be tested rather than checked by squinting at a tab bar.
 *
 * There was a fourth state, a bell rung while you looked elsewhere, kept
 * until the tab was seen. The holder took it out (2026-11-17): a bell is
 * a call for attention now, "it rarely carries value afterwards", and is
 * answered by a flash and the window's urgency hint instead (sec 5.5).
 */
namespace bssh_tab_status {

enum state_t {
	CONNECTED,
	CONNECTING,
	DISCONNECTED
};

state_t state_for(const bssh_session *session);
QColor colour_for(state_t state);

/*
 * The dot itself. Lives here rather than in the window so that what gets
 * rendered in a check is the same code the tab bar runs -- a harness that
 * redraws its own approximation of a widget verifies the harness.
 */
QPixmap dot(int size, state_t state);

} /* namespace bssh_tab_status */

#endif /* BSSH_TAB_STATUS_H */
