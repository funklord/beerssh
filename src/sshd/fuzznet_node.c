/*
 * beersshd as a fuzznet host (fuzznet_node.h, project.md sec 5.6).
 */

#include "sshd/fuzznet_node.h"

#include <stddef.h>

#include "node/daemon.h"

/*
 * Nothing of beersshd's own yet but the name it runs under: the node is
 * fuzznetd's, so pairing, revoking and the rules of who may speak are
 * fuzznet's and work today, and what a session says is dropped until the
 * stream is wired to the session helper.
 */
static const fzn_daemon_ext_t extension = { 0 };

int bssh_fuzznet_main(int argc, char **argv) {
	/* First, before anything is opened: it may execute this program again. */
	fzn_daemon_refuse_huge_pages(argv);
	return fzn_daemon_main(argc, argv, &extension);
}
