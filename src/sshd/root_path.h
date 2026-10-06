#ifndef BSSH_ROOT_PATH_H
#define BSSH_ROOT_PATH_H

#include <QByteArray>

/*
 * The one directory beersshd serves, and the mapping of every path a
 * client names into it (project.md sec 19).
 *
 * The client sees the root as "/". Its paths are bytes, as filenames
 * are on the systems this runs on, so they stay QByteArray throughout
 * rather than round-tripping through a QString that could not hold a
 * name that is not UTF-8.
 *
 * Confinement is checked on the RESOLVED path, not the lexical one:
 * "/../.." is easy to clamp, and a symlink inside the root that points
 * at the app's own private directory -- where the host key lives -- is
 * the case that matters. libssh's own SFTP handlers serve whatever path
 * they are given, which is why beersshd does not use them.
 */
class bssh_root_path {
public:
	/* `root` must be an existing directory; valid() says whether it was. */
	explicit bssh_root_path(const QByteArray &root);

	bool valid() const { return !m_root.isEmpty(); }

	/* The resolved root, with no trailing slash unless it is "/". */
	const QByteArray &root() const { return m_root; }

	/*
	 * Lexical cleanup of a client path: "." and empty segments go, ".."
	 * removes the segment before it and never climbs above "/", and a
	 * relative path is taken relative to "/". Always absolute.
	 */
	static QByteArray normalise(const QByteArray &client_path);

	struct resolved_t {
		QByteArray real;   /* empty on failure */
		int error = 0;     /* an errno when real is empty */
	};

	/*
	 * A path that must already exist, with every symlink in it followed.
	 * EACCES when it resolves outside the root.
	 */
	resolved_t existing(const QByteArray &client_path) const;

	/*
	 * A path whose LAST component is the thing being acted on -- created,
	 * removed, renamed -- and so is not followed: removing a symlink
	 * removes the link. Its parent must exist and resolve inside the root.
	 * If the leaf is already a symlink, it is followed and checked as
	 * well, since creating through it would write wherever it points.
	 * The root itself is never a valid leaf.
	 */
	resolved_t leaf(const QByteArray &client_path) const;

	/* The client's view of a real path inside the root: m_root -> "/". */
	QByteArray client_view(const QByteArray &real) const;

	/* Whether a resolved real path is the root or below it. */
	bool contains(const QByteArray &real) const;

private:
	QByteArray m_root;
};

#endif /* BSSH_ROOT_PATH_H */
