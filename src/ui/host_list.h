#ifndef BSSH_HOST_LIST_H
#define BSSH_HOST_LIST_H

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "ui/long_press.h"

#include "model/profile.h"

class QListWidget;
class QPushButton;
class QEvent;
class QResizeEvent;
class QToolButton;
class bssh_profile_store;
class bssh_session;

/*
 * bssh_host_list -- the touch front door (project.md sec 8.5).
 *
 * The same profiles as the desktop's PuTTY-shaped dialog, presented the
 * way a phone expects: one full-bleed list, a row you tap to connect, a
 * floating button to add one, and no category tree anywhere near it.
 *
 * The PuTTY layout is genuinely wrong here and not merely cramped -- a
 * tree of setting categories beside a form of small fields is a desktop
 * idiom that survives neither a thumb nor a 5-inch screen. Same model,
 * same store, different front door: that is the split sec 8.5 draws, and
 * the reason it is drawn at the front door rather than deeper.
 *
 * Still Qt Widgets, and still no QML (sec 2, non-negotiable): touch
 * problems get fixed by subclassing and styling widgets, which is what
 * the row height and the kinetic scroller below are.
 */
class bssh_host_list : public QWidget {
	Q_OBJECT

public:
	explicit bssh_host_list(bssh_profile_store *store, QWidget *parent = nullptr);

	void reload();

	/*
	 * The sessions this window has open, listed above the hosts: a tap
	 * goes back to THAT session, a host below opens a new one (sec 8.5,
	 * the holder's choice 2026-10-22). It replaced a single "N sessions
	 * open -- back to them" button, which named no session -- so going
	 * from one terminal to another meant Back, the button, then the tab
	 * bar, and a phone with one session had no visible way to a second.
	 */
	void set_open_sessions(const QList<bssh_session *> &sessions);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

signals:
	void open_requested(const bssh_profile_t &profile);
	void add_requested();

	/*
	 * Editing and removing a host, which this front door had no way to
	 * do at all: a row connected and that was the whole vocabulary, so a
	 * host imported from an SSH config could never be corrected and a
	 * mistyped one could never be removed (sec 8.5).
	 */
	void edit_requested(const bssh_profile_t &profile);
	void delete_requested(const bssh_profile_t &profile);

	/*
	 * The one road into config (sec 8.7). It used to be three -- Presets
	 * and Licences had signals of their own here, and the index this
	 * opens offered Licences again -- and they are rows on that index
	 * now, so this front door names config once.
	 */
	void settings_requested();

	/* An open session's row was tapped: show that one. */
	void session_picked(bssh_session *session);

private slots:
	void on_row_activated();
	void on_row_menu(const QPoint &where);

private:
	void style_add_button();
	void style_rows();

	bssh_profile_store *m_store;
	QListWidget *m_list;
	/* What set_open_sessions() was last given, listed by reload(). */
	QList<QPointer<bssh_session>> m_open;
	QString m_open_drawn;
	QString open_signature() const;

	/* Folders folded shut, by name (sec 10.5); for as long as the list lives. */
	QStringList m_collapsed;
	bssh_long_press *m_long_press;
	QToolButton *m_add;
	QToolButton *m_menu;
};

#endif /* BSSH_HOST_LIST_H */
