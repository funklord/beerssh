#ifndef BSSH_CRASH_TRACE_H
#define BSSH_CRASH_TRACE_H

#include <QByteArray>

/*
 * A trace left behind by a crash, on the desktop (project.md sec 5.1).
 *
 * The holder's first crash, closing a tab on 2026-11-20, left nothing to
 * read: core dumps were off, as they are by default, and the program's
 * output reached no log. So a fatal signal writes, into `directory`,
 * crash-<pid>.txt: the signal, the version, the return addresses of the
 * crashing thread and the executable mappings they fall in -- addresses
 * and file names only, no memory content, so nothing typed or shown can
 * reach it (sec 10's "no secrets in logs"). The beerssh-dbgsym package
 * turns the addresses into functions: `addr2line -f -e <file>` with the
 * offset from the mapping. The signal is then raised again with its
 * default action, so the process still dies as it would have, and a core
 * is still written where one is enabled.
 *
 * On its own stack, so a stack overflow -- a layout that recurses -- can
 * be written down too.
 *
 * Linux only: Android writes a tombstone of its own, and nothing else
 * here has /proc/self/maps.
 */
void bssh_install_crash_trace(const QByteArray &directory);

#endif
