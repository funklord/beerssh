/*
 * Which user a fuzznet device logs in as, and becoming them
 * (fuzznet_users.h, project.md sec 5.6).
 */

#define _GNU_SOURCE

#include "fuzznet_users.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Bionic has no shadow database: an Android app is never root to read one. */
#if defined(__linux__) && !defined(__ANDROID__)
#define BSSH_HAVE_SHADOW 1
#include <shadow.h>
#endif

/* The most a map may hold, a line of it being about a hundred bytes. */
#define MAP_BYTES_MAX 65536

static int fail(char *why, size_t capacity, const char *format, ...)
        __attribute__((format(printf, 3, 4)));

static int fail(char *why, size_t capacity, const char *format, ...) {
	if (why && capacity) {
		va_list args;
		va_start(args, format);
		vsnprintf(why, capacity, format, args);
		va_end(args);
	}
	return -1;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
 * A user name as useradd accepts one: a letter or underscore first, then
 * letters, digits, dot, underscore and hyphen, a `$` allowed last, at most 32.
 * Nothing that a path, an option or a shell could read as anything else.
 */
static int name_is_user_name(const char *name, size_t len) {
	if (len == 0 || len > BSSH_USER_NAME_MAX) return 0;
	for (size_t i = 0; i < len; ++i) {
		const char c = name[i];
		const int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
		const int other = (c >= '0' && c <= '9') || c == '.' || c == '-';
		if (i == 0 && !letter) return 0;
		if (c == '$' && i == len - 1 && i > 0) continue;
		if (!letter && !other) return 0;
	}
	return 1;
}

static int is_blank(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

int bssh_fuzznet_users_parse(const char *text, size_t len, bssh_fuzznet_user_t *out, int capacity,
                             char *why, size_t why_capacity) {
	int count = 0;
	int line = 0;
	size_t at = 0;
	while (at < len) {
		++line;
		size_t end = at;
		while (end < len && text[end] != '\n') ++end;
		const size_t next = end < len ? end + 1 : end;
		/* A comment ends the line wherever it starts. */
		for (size_t i = at; i < end; ++i) {
			if (text[i] == '#') {
				end = i;
				break;
			}
		}
		if (memchr(text + at, '\0', end - at))
			return fail(why, why_capacity, "line %d holds a NUL byte", line);

		size_t i = at;
		while (i < end && is_blank(text[i])) ++i;
		if (i == end) {
			at = next;
			continue;
		}

		const size_t key_start = i;
		while (i < end && !is_blank(text[i])) ++i;
		const size_t key_len = i - key_start;
		while (i < end && is_blank(text[i])) ++i;
		const size_t user_start = i;
		while (i < end && !is_blank(text[i])) ++i;
		const size_t user_len = i - user_start;
		while (i < end && is_blank(text[i])) ++i;
		if (i != end)
			return fail(why, why_capacity, "line %d has more than a key and a user", line);
		if (key_len != 2 * BSSH_FUZZNET_KEY_LEN)
			return fail(why, why_capacity, "line %d: a key is %d hex digits", line,
			            2 * BSSH_FUZZNET_KEY_LEN);
		if (user_len == 0) return fail(why, why_capacity, "line %d names no user", line);
		if (!name_is_user_name(text + user_start, user_len))
			return fail(why, why_capacity, "line %d: not a user name", line);
		if (count == capacity)
			return fail(why, why_capacity, "more than %d devices", capacity);

		bssh_fuzznet_user_t *entry = &out[count];
		for (size_t k = 0; k < BSSH_FUZZNET_KEY_LEN; ++k) {
			const int high = hex_digit(text[key_start + 2 * k]);
			const int low = hex_digit(text[key_start + 2 * k + 1]);
			if (high < 0 || low < 0) return fail(why, why_capacity, "line %d: a key is hex digits", line);
			entry->key[k] = (uint8_t)(high << 4 | low);
		}
		for (int other = 0; other < count; ++other) {
			if (!memcmp(out[other].key, entry->key, BSSH_FUZZNET_KEY_LEN))
				return fail(why, why_capacity, "line %d names a device a line above does", line);
		}
		memcpy(entry->user, text + user_start, user_len);
		entry->user[user_len] = '\0';
		++count;
		at = next;
	}
	return count;
}

/* Owned by `owner` and writable by nobody else, as StrictModes asks. */
static int held_by(const struct stat *info, uid_t owner) {
	return info->st_uid == owner && !(info->st_mode & (S_IWGRP | S_IWOTH));
}

int bssh_fuzznet_users_load(const char *path, uid_t owner, bssh_fuzznet_user_t *out, int capacity,
                            char *why, size_t why_capacity) {
	if (!path || path[0] != '/') return fail(why, why_capacity, "the map's path is not absolute");

	char directory[BSSH_LOGIN_PATH_MAX];
	if (snprintf(directory, sizeof(directory), "%s", path) >= (int)sizeof(directory))
		return fail(why, why_capacity, "the map's path is too long");
	char *slash = strrchr(directory, '/');
	if (slash == directory) slash[1] = '\0';
	else *slash = '\0';
	struct stat info;
	if (lstat(directory, &info) != 0)
		return fail(why, why_capacity, "%s: %s", directory, strerror(errno));
	if (!S_ISDIR(info.st_mode) || !held_by(&info, owner))
		return fail(why, why_capacity, "%s is not a directory only uid %u may write",
		            directory, (unsigned)owner);

	/* Not through a link: what is checked is what is read. */
	const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return fail(why, why_capacity, "%s: %s", path, strerror(errno));
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || !held_by(&info, owner)) {
		close(fd);
		return fail(why, why_capacity, "%s is not a file only uid %u may write", path,
		            (unsigned)owner);
	}
	if (info.st_size > MAP_BYTES_MAX) {
		close(fd);
		return fail(why, why_capacity, "%s is larger than a map", path);
	}

	char *text = malloc(MAP_BYTES_MAX + 1);
	if (!text) {
		close(fd);
		return fail(why, why_capacity, "no memory");
	}
	size_t len = 0;
	for (;;) {
		const ssize_t got = read(fd, text + len, MAP_BYTES_MAX + 1 - len);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0) {
			const int error = errno;
			close(fd);
			free(text);
			return fail(why, why_capacity, "%s: %s", path, strerror(error));
		}
		if (got == 0) break;
		len += (size_t)got;
		if (len > MAP_BYTES_MAX) {
			close(fd);
			free(text);
			return fail(why, why_capacity, "%s is larger than a map", path);
		}
	}
	close(fd);
	const int count = bssh_fuzznet_users_parse(text, len, out, capacity, why, why_capacity);
	free(text);
	return count;
}

const char *bssh_fuzznet_user_for(const bssh_fuzznet_user_t *map, int count,
                                  const uint8_t key[BSSH_FUZZNET_KEY_LEN]) {
	for (int i = 0; i < count; ++i) {
		if (!memcmp(map[i].key, key, BSSH_FUZZNET_KEY_LEN)) return map[i].user;
	}
	return NULL;
}

void bssh_fuzznet_key_hex(const uint8_t key[BSSH_FUZZNET_KEY_LEN], char out[2 * BSSH_FUZZNET_KEY_LEN + 1]) {
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < BSSH_FUZZNET_KEY_LEN; ++i) {
		out[2 * i] = digits[key[i] >> 4];
		out[2 * i + 1] = digits[key[i] & 15];
	}
	out[2 * BSSH_FUZZNET_KEY_LEN] = '\0';
}

/* Past its expiry date, as shadow says; readable only by root, and unknown otherwise. */
static int account_expired(const char *user) {
#ifdef BSSH_HAVE_SHADOW
	struct spwd entry;
	struct spwd *found = NULL;
	char buffer[4096];
	if (getspnam_r(user, &entry, buffer, sizeof(buffer), &found) != 0 || !found) return 0;
	if (found->sp_expire <= 0) return 0;
	const long today = (long)(time(NULL) / 86400);
	return today >= found->sp_expire;
#else
	(void)user;
	return 0;
#endif
}

int bssh_login_lookup(const char *user, const char *nologin, bssh_login_t *out, char *why,
                      size_t why_capacity) {
	if (!user || !name_is_user_name(user, strlen(user)))
		return fail(why, why_capacity, "not a user name");

	struct passwd entry;
	struct passwd *found = NULL;
	char buffer[16384];
	const int error = getpwnam_r(user, &entry, buffer, sizeof(buffer), &found);
	if (error != 0) return fail(why, why_capacity, "%s: %s", user, strerror(error));
	if (!found) return fail(why, why_capacity, "no user %s here", user);
	if (found->pw_uid == 0)
		return fail(why, why_capacity, "%s is uid 0, and root is never a fuzznet login", user);
	if (account_expired(user)) return fail(why, why_capacity, "%s's account has expired", user);
	if (nologin && access(nologin, F_OK) == 0)
		return fail(why, why_capacity, "%s exists: logins are off", nologin);

	memset(out, 0, sizeof(*out));
	out->uid = found->pw_uid;
	out->gid = found->pw_gid;
	snprintf(out->name, sizeof(out->name), "%s", found->pw_name);
	const char *home = found->pw_dir && *found->pw_dir ? found->pw_dir : "/";
	const char *shell = found->pw_shell && *found->pw_shell ? found->pw_shell : "/bin/sh";
	if (snprintf(out->home, sizeof(out->home), "%s", home) >= (int)sizeof(out->home)
	    || snprintf(out->shell, sizeof(out->shell), "%s", shell) >= (int)sizeof(out->shell))
		return fail(why, why_capacity, "%s's home or shell is too long", user);
	if (out->shell[0] != '/' || access(out->shell, X_OK) != 0)
		return fail(why, why_capacity, "%s's shell %s cannot be run", user, out->shell);

	int groups = BSSH_LOGIN_GROUPS_MAX;
	if (getgrouplist(out->name, out->gid, out->groups, &groups) < 0)
		return fail(why, why_capacity, "%s is in more than %d groups", user, BSSH_LOGIN_GROUPS_MAX);
	out->group_count = groups;
	return 0;
}

int bssh_login_environment(const bssh_login_t *login, const char *runtime_root, char **envp,
                           size_t envp_capacity, char *storage, size_t storage_capacity) {
	size_t count = 0;
	size_t used = 0;
	int failed = 0;
	char line[2 * BSSH_LOGIN_PATH_MAX];

#define BSSH_PUT(...) \
	do { \
		const int len = snprintf(line, sizeof(line), __VA_ARGS__); \
		if (len < 0 || (size_t)len >= sizeof(line) || used + (size_t)len + 1 > storage_capacity \
		    || count + 1 >= envp_capacity) { \
			failed = 1; \
		} else { \
			memcpy(storage + used, line, (size_t)len + 1); \
			envp[count++] = storage + used; \
			used += (size_t)len + 1; \
		} \
	} while (0)

	BSSH_PUT("HOME=%s", login->home);
	BSSH_PUT("USER=%s", login->name);
	BSSH_PUT("LOGNAME=%s", login->name);
	BSSH_PUT("SHELL=%s", login->shell);
	BSSH_PUT("PATH=%s", "/usr/local/bin:/usr/bin:/bin");

	/*
	 * Only a runtime directory that is the user's and closed to others: the
	 * helper keeps its sockets there, and one found under another owner is
	 * no place for them. Where there is none -- no logind session, as here
	 * without PAM -- the helper makes its own under /tmp.
	 */
	if (runtime_root) {
		char runtime[BSSH_LOGIN_PATH_MAX];
		struct stat info;
		if (snprintf(runtime, sizeof(runtime), "%s/%u", runtime_root, (unsigned)login->uid)
		            < (int)sizeof(runtime)
		    && lstat(runtime, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == login->uid
		    && !(info.st_mode & 077))
			BSSH_PUT("XDG_RUNTIME_DIR=%s", runtime);
	}

	static const char *const kept[] = { "LANG", "TZ" };
	for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); ++i) {
		const char *value = getenv(kept[i]);
		if (value) BSSH_PUT("%s=%s", kept[i], value);
	}
#undef BSSH_PUT

	if (failed || count >= envp_capacity) return -1;
	envp[count] = NULL;
	return 0;
}

int bssh_login_become(const bssh_login_t *login, char *why, size_t why_capacity) {
	if (login->uid == 0) return fail(why, why_capacity, "refused to become uid 0");
	if (setgroups((size_t)login->group_count, login->groups) != 0)
		return fail(why, why_capacity, "setgroups: %s", strerror(errno));
#ifdef __linux__
	if (setresgid(login->gid, login->gid, login->gid) != 0)
		return fail(why, why_capacity, "setresgid: %s", strerror(errno));
	if (setresuid(login->uid, login->uid, login->uid) != 0)
		return fail(why, why_capacity, "setresuid: %s", strerror(errno));
	uid_t ruid, euid, suid;
	gid_t rgid, egid, sgid;
	if (getresuid(&ruid, &euid, &suid) != 0 || getresgid(&rgid, &egid, &sgid) != 0
	    || ruid != login->uid || euid != login->uid || suid != login->uid || rgid != login->gid
	    || egid != login->gid || sgid != login->gid)
		return fail(why, why_capacity, "the ids are not the user's after the change");
#else
	if (setgid(login->gid) != 0) return fail(why, why_capacity, "setgid: %s", strerror(errno));
	if (setuid(login->uid) != 0) return fail(why, why_capacity, "setuid: %s", strerror(errno));
	if (getuid() != login->uid || geteuid() != login->uid || getgid() != login->gid
	    || getegid() != login->gid)
		return fail(why, why_capacity, "the ids are not the user's after the change");
#endif
	/* And no way back: a process that can become root again has not dropped. */
	if (setuid(0) == 0 || seteuid(0) == 0)
		return fail(why, why_capacity, "root could be had back after the change");
	return 0;
}
