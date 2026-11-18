#ifndef BSSH_FUZZNET_NODE_H
#define BSSH_FUZZNET_NODE_H

/*
 * beersshd as a fuzznet host (project.md sec 5.6, fuzznet's sec 925):
 * fuzznet carries and gates, beersshd starts the process. Where the
 * transport is fuzznet rather than SSH, a kept session reaches its client
 * as words in a fuzznet session -- fuzznet's say and heard -- over the
 * reliable stream of src/session/stream.h.
 *
 * Its own process, `beersshd --fuzznet ...`: fuzznet's node runs once per
 * process, owns its loop and its signals, and does not return while it
 * serves, which a Qt event loop beside it cannot share. So main() hands
 * over before Qt starts, and everything after `--fuzznet` is fuzznetd's
 * command line -- `--fuzznet-dir=`, `--new-root`, `--pair=` and the rest
 * -- read by fuzznet's own parser.
 *
 * Linux only for now: fuzznet's Windows and macOS builds wait on fuzznet
 * (its sec 967), and the Android build is a step of its own.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* The UDP port beersshd's fuzznet host serves on unless told another: its SSH port's number. */
#define BSSH_FUZZNET_PORT 8022

/* fuzznet's node with beersshd's extension; `argv[0]` is the program. */
int bssh_fuzznet_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
