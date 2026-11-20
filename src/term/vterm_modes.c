#include "term/vterm_modes.h"

#include "../../vterm/src/vterm_internal.h"

int bssh_vterm_bracketed_paste(VTerm *vt) {
	if (!vt || !vt->state) return 0;
	return vt->state->mode.bracketpaste ? 1 : 0;
}
