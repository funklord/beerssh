#ifndef BSSH_FUZZNET_USERS_H
#define BSSH_FUZZNET_USERS_H

/*
 * beersshd as a root fuzznet daemon (project.md sec 5.6, "beersshd as a root
 * fuzznet daemon, sshd's shape"): which user a paired device logs in as, and
 * becoming that user before the session helper runs.
 *
 * Plain C and POSIX, none of fuzznet's headers, so the suite can test it as a
 * user; the one step that needs root, bssh_login_become(), is tested as root
 * in a hull vm.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fuzznet's FZN_PUBKEY_LEN; fuzznet_node.c checks the two agree. */
#define BSSH_FUZZNET_KEY_LEN 32
#define BSSH_FUZZNET_USERS_MAX 256
#define BSSH_USER_NAME_MAX 32
#define BSSH_LOGIN_GROUPS_MAX 256
#define BSSH_LOGIN_PATH_MAX 1024

typedef struct bssh_fuzznet_user {
	uint8_t key[BSSH_FUZZNET_KEY_LEN];
	char user[BSSH_USER_NAME_MAX + 1];
} bssh_fuzznet_user_t;

/*
 * The map's text: a line per device, its key as 64 hex digits, whitespace,
 * a user name; `#` to the end of a line is a comment. The number of lines
 * read, or -1 and `why` -- for the whole map, never one line, so a typo
 * cannot quietly grant or drop a device.
 */
int bssh_fuzznet_users_parse(const char *text, size_t len, bssh_fuzznet_user_t *out, int capacity,
                             char *why, size_t why_capacity);

/*
 * The map at `path`, checked as sshd's StrictModes checks a file of keys: a
 * regular file owned by `owner`, writable by nobody else, in a directory
 * owned by `owner` and writable by nobody else. `owner` is 0 when beersshd
 * runs as root; a test passes its own uid.
 */
int bssh_fuzznet_users_load(const char *path, uid_t owner, bssh_fuzznet_user_t *out, int capacity,
                            char *why, size_t why_capacity);

/* The user `key` logs in as, or NULL. */
const char *bssh_fuzznet_user_for(const bssh_fuzznet_user_t *map, int count,
                                  const uint8_t key[BSSH_FUZZNET_KEY_LEN]);

/* `key` as the 64 hex digits the map holds, for a log line. */
void bssh_fuzznet_key_hex(const uint8_t key[BSSH_FUZZNET_KEY_LEN], char out[2 * BSSH_FUZZNET_KEY_LEN + 1]);

/* Who a session starts as, looked up before the fork. */
typedef struct bssh_login {
	uid_t uid;
	gid_t gid;
	char name[BSSH_USER_NAME_MAX + 1];
	char home[BSSH_LOGIN_PATH_MAX];
	char shell[BSSH_LOGIN_PATH_MAX];
	gid_t groups[BSSH_LOGIN_GROUPS_MAX];
	int group_count;
} bssh_login_t;

/*
 * `user` looked up, and whether they may log in: refused for a name not in
 * the password database, uid 0 (root is never a fuzznet login), an account
 * past its expiry date, a shell that is not executable, and any user while
 * `nologin` names a file that exists -- "/etc/nologin", as sshd does without
 * PAM. 0, or -1 and `why`.
 */
int bssh_login_lookup(const char *user, const char *nologin, bssh_login_t *out, char *why,
                      size_t why_capacity);

/*
 * The helper's environment, built from the password entry rather than
 * inherited: HOME, USER, LOGNAME, SHELL, a plain PATH, XDG_RUNTIME_DIR where
 * `runtime_root`/<uid> exists, is the user's and is closed to others, and
 * LANG and TZ as this process has them. Written into `envp` (NULL ended) and
 * `storage`; 0, or -1 when they are too small.
 */
int bssh_login_environment(const bssh_login_t *login, const char *runtime_root, char **envp,
                           size_t envp_capacity, char *storage, size_t storage_capacity);

/*
 * Becomes `login`, in a child about to run the helper: its groups, then its
 * gid, then its uid, and then checked -- every id the user's and a return to
 * root refused. 0, or -1 and `why`, after which nothing may run.
 */
int bssh_login_become(const bssh_login_t *login, char *why, size_t why_capacity);

#ifdef __cplusplus
}
#endif

#endif /* BSSH_FUZZNET_USERS_H */
