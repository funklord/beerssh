#ifndef BSSH_HOST_LIST_H
#define BSSH_HOST_LIST_H

#include <QWidget>

#include "model/profile.h"

class QListWidget;
class QResizeEvent;
class QToolButton;
class bssh_profile_store;

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

protected:
	void resizeEvent(QResizeEvent *event) override;

signals:
	void open_requested(const bssh_profile_t &profile);
	void add_requested();

private slots:
	void on_row_activated();

private:
	bssh_profile_store *m_store;
	QListWidget *m_list;
	QToolButton *m_add;
};

#endif /* BSSH_HOST_LIST_H */
