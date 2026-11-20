#ifndef BSSH_ARRANGEMENT_H
#define BSSH_ARRANGEMENT_H

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

/*
 * How a BeerSSH has its terminals laid out, as a document (project.md
 * sec 8.4): every window, where it is and how it is shown, and the panes
 * and tabs in it. A new version asks the old one for it over the hand-off
 * socket and builds the same thing from the sessions it takes (the holder,
 * 2026-11-20: "hand over all terminals from an older version into a new
 * version and set up the layout in the same way").
 *
 * Terminals are named by whatever the writer chose -- hand-off ids for an
 * upgrade. The document says nothing about how they are reached, so the
 * same shape can later be saved and loaded with other names in it.
 */
struct bssh_arrangement_t {
	struct window_t {
		/* The normal geometry, not a maximised one: what un-maximising returns to. */
		QRect geometry;
		bool maximized = false;
		bool full_screen = false;
		bool terminals_panel = false;

		/* bssh_layout_manager::shape_text() over the window's tree, or empty for one pane. */
		QString shape;

		/* Every terminal the window holds, in its order: the tabs. */
		QStringList terminals;

		/* The one with the focus, or empty. */
		QString current;
	};

	QList<window_t> windows;

	bool is_empty() const { return windows.isEmpty(); }

	/* As JSON, compact: one message on the hand-off socket. */
	QString to_text() const;

	/* Empty, with why, for text that is not an arrangement of a version this reads. */
	static bssh_arrangement_t from_text(const QString &text, QString *why = nullptr);
};

#endif /* BSSH_ARRANGEMENT_H */
