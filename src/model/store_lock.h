#ifndef BSSH_STORE_LOCK_H
#define BSSH_STORE_LOCK_H

#include <QLockFile>
#include <QString>

/*
 * One change at a time across every BeerSSH on this account, from the
 * re-read to the write, for the stores every window process shares -- the
 * host list (profile_store.cpp) and the file of secrets
 * (secret_store_file.cpp). Without it two windows saving at once each wrote
 * back their own copy and erased the other's save (sec 11).
 *
 * It waits as long as the holder is getting on with it. It gave up after
 * five seconds and changed the store anyway, so that a peer stopped under
 * a debugger could not block a save -- and under load a peer that was only
 * slow lost the race the same way: at a load of 28 on 24 cores, 25 of 144
 * runs of three processes saving secrets at once lost one, every loss
 * beside "another BeerSSH holds the store, changing it anyway"
 * (2026-11-24, project.md sec 8.4's store entry). So now it goes on
 * without the lock only where waiting cannot help: the holder stopped
 * (a debugger, SIGSTOP) for five seconds, or a minute gone by with no turn,
 * which a save of a few kilobytes never takes. A dead holder's lock is
 * taken over by QLockFile itself.
 */
class bssh_store_lock {
public:
	/* `label` names the store in the warning: "hosts", "secrets". */
	bssh_store_lock(const QString &path, const char *label);

	/* Held, or gone on without (and said so). */
	bool held() const { return m_held; }

	/* Whether `pid` is stopped or traced, where the system says (Linux). */
	static bool process_is_stopped(qint64 pid);

	/* How long a stopped holder is waited for, and the ceiling for any. */
	static const int stopped_ms = 5000;
	static const int ceiling_ms = 60000;

private:
	QLockFile m_lock;
	bool m_held = false;
};

#endif /* BSSH_STORE_LOCK_H */
