/*
 * beersshd's fuzznet host as a program of its own, for beersshd's Android
 * app (project.md sec 5.6). On the desktop `beersshd --fuzznet` is the same
 * node inside beersshd; an Android app has no command line to give it and
 * runs Qt in the one process it has, so the node ships in the APK beside the
 * session helper, as an executable named lib*.so -- the one kind of file an
 * Android app may run -- and the app starts it.
 */

#include "sshd/fuzznet_node.h"

int main(int argc, char **argv) {
	return bssh_fuzznet_main(argc, argv);
}
