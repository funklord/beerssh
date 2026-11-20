#ifndef BSSH_VTERM_MODES_H
#define BSSH_VTERM_MODES_H

#include <vterm.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modes libvterm keeps and no API of its own reads.
 *
 * libvterm tracks DEC modes in its state and answers DECRQM from there,
 * but offers no getter; screen_model.c reads the same structure the same
 * way. C rather than C++ because the structure is libvterm's internal
 * header, which is C, and this file is built against the same vendored,
 * patched copy, so the two cannot disagree about its layout.
 */

/* DEC 2004: whether the remote has asked for pastes to be bracketed. */
int bssh_vterm_bracketed_paste(VTerm *vt);

#ifdef __cplusplus
}
#endif

#endif
