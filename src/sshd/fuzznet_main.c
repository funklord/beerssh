/*
 * beersshd's fuzznet host as a program of its own, for beersshd's Android
 * app (project.md sec 5.6). On the desktop `beersshd --fuzznet` is the same
 * node inside beersshd; an Android app has no command line to give it and
 * runs Qt in the one process it has, so the node ships in the APK beside the
 * session helper, as an executable named lib*.so -- the one kind of file an
 * Android app may run -- and the app starts it.
 */

#include "sshd/fuzznet_node.h"

#include <string.h>

/* `--fuzznet` first is taken and dropped, so this is run as beersshd's own is. */
int main(int argc, char **argv) {
	if (argc > 1 && !strcmp(argv[1], "--fuzznet")) {
		argv[1] = argv[0];
		return bssh_fuzznet_main(argc - 1, argv + 1);
	}
	return bssh_fuzznet_main(argc, argv);
}
