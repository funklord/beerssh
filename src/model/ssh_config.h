#ifndef BSSH_SSH_CONFIG_H
#define BSSH_SSH_CONFIG_H

#include <QList>
#include <QString>

#include "model/profile.h"

/*
 * bssh_ssh_config -- reading OpenSSH's client config into host profiles
 * (project.md sec 10.5).
 *
 * An explicit IMPORT and never a live read. The user's `~/.ssh/config` is
 * OpenSSH's file, not this app's: following it at connect time would mean
 * BeerSSH's behaviour changed when a different tool's configuration did,
 * with nothing on screen to explain why. Hosts are copied in once, and
 * from then on they are this app's to edit.
 *
 * Nothing here reads a key, a password or a known_hosts entry. The parser
 * is handed text and returns structs; it opens exactly one file, the one
 * it is asked for, and does not follow Include (see below).
 */

struct bssh_ssh_config_host_t {
	/* The `Host` alias -- what the user types after `ssh`. */
	QString alias;

	/* Resolved `HostName`, or the alias when the config does not say. */
	QString hostname;

	QString user;
	quint16 port;

	/*
	 * IdentityFile is carried into the profile and reaches libssh as
	 * SSH_OPTIONS_ADD_IDENTITY (sec 11).
	 *
	 * ProxyJump is carried too, and honoured: it becomes the profile's
	 * jump host and reaches a direct-tcpip tunnel at connect time
	 * (bssh_jump_tunnel). A CHAIN is the exception -- OpenSSH allows
	 * "a,b" and this does not -- so has_unsupported() still has something
	 * to report rather than letting a host silently lose its route.
	 */
	QString identity_file;
	QString proxy_jump;

	bssh_ssh_config_host_t() : port(22) {}

	bool is_valid() const { return !hostname.isEmpty(); }

	/*
	 * Whether anything was recognised that the app cannot honour, so the
	 * caller can report it rather than the user discovering it at connect
	 * time.
	 */
	bool has_unsupported() const { return proxy_jump.contains(QLatin1Char(',')); }

	bssh_profile_t to_profile() const;
};

class bssh_ssh_config {
public:
	/*
	 * Parses config text into one entry per concrete `Host` alias.
	 *
	 * Three pieces of OpenSSH's semantics are load-bearing, and a parser
	 * that skips them produces plausible entries that connect somewhere
	 * else:
	 *
	 * - **The FIRST value for a keyword wins**, not the last. This is
	 *   backwards from every config format that looks like this one, and
	 *   it is why a `Host *` block at the TOP of a file overrides
	 *   everything below it rather than defaulting it.
	 * - **Patterns are not hosts.** `Host *`, `Host *.internal` and
	 *   `Host !bastion prod-*` describe which blocks apply; they are not
	 *   machines anyone can connect to, so they never become profiles.
	 *   Their settings still reach the concrete aliases they match.
	 * - **`Match` cannot be evaluated statically.** It depends on the
	 *   command line, the local user, even the output of a command, so a
	 *   Match block's contents are skipped rather than guessed at.
	 *
	 * `Include` is not followed. It would mean opening files the user did
	 * not point at, and an import that reaches into paths nobody named is
	 * the live-read behaviour this whole type exists to avoid.
	 */
	static QList<bssh_ssh_config_host_t> parse(const QString &text);

	/*
	 * Reads and parses a file. The path may be a content:// URI on
	 * Android, which is bridged the same way a preset import is
	 * (sec 10.5).
	 */
	static QList<bssh_ssh_config_host_t> read(const QString &raw_path, bool *ok = nullptr);

	/* Where OpenSSH keeps it, for the desktop file picker's start point. */
	static QString default_path();
};

#endif /* BSSH_SSH_CONFIG_H */
