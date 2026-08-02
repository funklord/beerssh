#include "ui/main_window.h"

#include <QCloseEvent>
#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaEnum>
#include <QVBoxLayout>

#include <QStackedWidget>

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QTabBar>

#include "model/profile_store.h"
#include "model/session.h"
#include "model/conn_manager.h"
#include "model/session_registry.h"
#include "ui/layout_manager.h"
#include "input/input_router.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ui/host_list.h"
#include "ui/key_row.h"
#include "ui/session_dialog.h"
#include "ui/tab_status.h"
#include "ui/terminal_view.h"

bssh_main_window::bssh_main_window(const QString &target, QWidget *parent)
    : QWidget(parent),
      m_pages(new QStackedWidget(this)),
      m_terminal_page(new QWidget(this)),
      m_profile_store(new bssh_profile_store(QString(), this)),
      m_session_dialog(nullptr),
      m_host_list(nullptr),
      m_terminal_view(new bssh_terminal_view(this)),
      m_status_label(new QLabel(this)),
      m_registry(new bssh_session_registry(this)),
      m_layout(nullptr),
      m_current(nullptr),
      m_tabs(new QTabBar(m_terminal_page)),
      m_key_row(nullptr),
      m_cols(0),
      m_rows(0),
      m_asked_about_closing(false),
      m_background_on_close(false) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);
	outer->addWidget(m_pages, 1);

	QVBoxLayout *layout = new QVBoxLayout(m_terminal_page);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/*
	 * Tabs above the terminal (sec 8.3). One per session, labelled from
	 * the OSC title with the profile name as fallback, and hidden while
	 * there is only one -- a single tab is a strip of wasted rows on a
	 * phone.
	 */
	m_tabs->setExpanding(false);
	m_tabs->setTabsClosable(true);
	m_tabs->setDrawBase(false);
	m_tabs->hide();
	layout->addWidget(m_tabs, 0);
	layout->addWidget(m_terminal_view, 1);
	layout->addWidget(m_status_label, 0);

	connect(m_tabs, &QTabBar::currentChanged, this, &bssh_main_window::on_tab_changed);
	connect(m_tabs, &QTabBar::tabCloseRequested,
	         this, &bssh_main_window::on_tab_close_requested);
	connect(m_registry, &bssh_session_registry::changed,
	         this, &bssh_main_window::on_registry_changed);

	/*
	 * A session about to be destroyed must be let go of first: the view
	 * holds pointers into its screen, scrollback and images, and the next
	 * repaint would read freed cells (sec 3, core principle 2).
	 */
	connect(m_registry, &bssh_session_registry::session_closing,
	         this, [this](bssh_session *session) {
		if (m_current == session) detach_session();
	});

	m_pages->addWidget(m_terminal_page);

	m_status_label->setContentsMargins(6, 3, 6, 3);

	/*
	 * The status line is scaffolding, not a feature: it reports which
	 * platform implementation was built in, the grid size, and where the
	 * session is. At M7 this space becomes the per-session status (sec
	 * 8.3).
	 */
	connect(m_terminal_view, &bssh_terminal_view::grid_resized,
	         this, &bssh_main_window::on_grid_resized);

	m_cols = m_terminal_view->cols();
	m_rows = m_terminal_view->rows();

	/*
	 * The key row is built with the window but its router is not: a
	 * router belongs to a session, and the key row is retargeted at
	 * whichever session is showing (sec 3, core principle 2).
	 */
	m_key_row = new bssh_key_row(nullptr, this);
	layout->insertWidget(2, m_key_row);

	/*
	 * Shortcuts are the window's, because they act on the window: copy
	 * takes the view's selection, paste and scrolling act on whatever
	 * session is currently showing. Connected once here, retargeted by
	 * show_session().
	 */
	auto handle_shortcut = [this](bssh_input_router::shortcut_t shortcut) {
		if (shortcut == bssh_input_router::SHORTCUT_COPY) {
			const QString selected = m_terminal_view->selection_text();
			if (!selected.isEmpty()) QGuiApplication::clipboard()->setText(selected);
		}
		if (shortcut == bssh_input_router::SHORTCUT_PASTE && m_current) {
			m_current->input_router()->paste(QGuiApplication::clipboard()->text());
		}
		if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_UP) {
			m_terminal_view->scroll_by(m_terminal_view->rows() / 2);
		}
		if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_DOWN) {
			m_terminal_view->scroll_by(-m_terminal_view->rows() / 2);
		}
	};
	connect(m_registry, &bssh_session_registry::session_added,
	         this, [this, handle_shortcut](bssh_session *session) {
		/*
		 * Reconnection is only real to the user if they can see it. A
		 * terminal that has silently stopped receiving output looks
		 * identical to one whose remote is merely quiet (sec 5.3).
		 */
		if (bssh_conn_manager *manager = m_registry->conn_manager_for(session)) {
			connect(manager, &bssh_conn_manager::reconnect_scheduled,
			         this, [this, session](int delay_ms, int attempt) {
				if (session != m_current) return;
				m_error = QStringLiteral("reconnecting in %1s (attempt %2)")
				                  .arg((delay_ms + 999) / 1000)
				                  .arg(attempt);
				update_status();
			});
			connect(manager, &bssh_conn_manager::reconnect_started, this, [this, session](int) {
				if (session != m_current) return;
				m_error = QStringLiteral("reconnecting...");
				update_status();
			});
			connect(manager, &bssh_conn_manager::gave_up, this, [this, session] {
				if (session != m_current) return;
				m_error = QStringLiteral("gave up reconnecting -- the shell is gone, "
				                          "not suspended");
				update_status();
			});
		}

		connect(session->input_router(), &bssh_input_router::shortcut_pressed,
		         this, handle_shortcut);

		/* OSC title also drives the window title on desktop (sec 8.3). */
		connect(session, &bssh_session::title_changed, this, [this, session] {
			if (session == m_current) {
				setWindowTitle(QStringLiteral("%1 - BeerSSH").arg(session->title()));
			}
		});
	});



	/*
	 * X11/Wayland's primary selection: selecting text makes it available
	 * to middle-click paste, without touching the ordinary clipboard.
	 * Qt reports the mode as unsupported on platforms that lack it, so
	 * this needs no platform check of its own.
	 */
	connect(m_terminal_view, &bssh_terminal_view::selection_changed, this, [this] {
		if (!QGuiApplication::clipboard()->supportsSelection()) return;

		const QString selected = m_terminal_view->selection_text();
		if (selected.isEmpty()) return;

		QGuiApplication::clipboard()->setText(selected, QClipboard::Selection);
	});

	update_status();

	/*
	 * A command-line target skips the front door entirely, which is what
	 * anyone typing one wants. Otherwise the front door is the first
	 * thing shown, as it is in PuTTY.
	 */
	if (!target.isEmpty()) {
		m_pages->setCurrentWidget(m_terminal_page);
		connect_to(target);
	} else {
		restore_layout();
	}
}

void bssh_main_window::connect_to(const QString &target) {
	/* [user@]host[:port] from the command line -- see the header. */
	bssh_profile_t profile;
	QString rest = target;

	const int at = rest.lastIndexOf(QLatin1Char('@'));
	if (at >= 0) {
		profile.user = rest.left(at);
		rest = rest.mid(at + 1);
	}
	const int colon = rest.lastIndexOf(QLatin1Char(':'));
	if (colon >= 0) {
		profile.port = static_cast<quint16>(rest.mid(colon + 1).toUShort());
		rest = rest.left(colon);
	}
	profile.host = rest;
	if (profile.user.isEmpty()) profile.user = qEnvironmentVariable("USER");

	open_profile(profile);
}

void bssh_main_window::open_profile(const bssh_profile_t &profile) {
	if (!profile.is_valid()) return;

	/*
	 * No password prompt here. Keys are tried first and most people have
	 * one, so asking up front is asking for a credential that is usually
	 * not needed -- the session asks through needs_password() only when
	 * the keys have actually failed.
	 */
	bssh_session *session = m_registry->create(profile);
	connect(session, &bssh_session::failed, this, &bssh_main_window::on_failed);

	/*
	 * First contact with a host. The fingerprint is shown because it is
	 * the only thing the user can actually check -- against what the
	 * server's owner published, or against `ssh-keyscan` from somewhere
	 * they already trust.
	 */
	connect(session, &bssh_session::host_key_unknown, this, [this, session](const QString &fp) {
		QMessageBox box(this);
		box.setIcon(QMessageBox::Question);
		box.setWindowTitle(QStringLiteral("Unknown host key"));
		box.setText(QStringLiteral("The host %1 is not in BeerSSH's known-hosts store.")
		                    .arg(session->profile().target()));
		box.setInformativeText(QStringLiteral(
		        "Key fingerprint:\n%1\n\nCheck it against a source you already trust "
		        "before accepting. Accepting records it for next time.").arg(fp));

		QPushButton *accept = box.addButton(QStringLiteral("Accept and connect"),
		                                     QMessageBox::AcceptRole);
		box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);

		/*
		 * Cancel is the default, so leaning on Enter does not trust a
		 * host nobody looked at.
		 */
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();

		if (box.clickedButton() == static_cast<QAbstractButton *>(accept)) {
			session->accept_host_key();
		} else {
			session->disconnect_from_host();
		}
	});

	/*
	 * A changed key is a different question wearing the same clothes, and
	 * the dialog says so: this is what a man-in-the-middle looks like, and
	 * it is also what a reinstalled server looks like. Only a person can
	 * tell those apart, which is why it is asked rather than decided.
	 */
	connect(session, &bssh_session::host_key_changed, this, [this, session](const QString &fp) {
		QMessageBox box(this);
		box.setIcon(QMessageBox::Warning);
		box.setWindowTitle(QStringLiteral("HOST KEY CHANGED"));
		box.setText(QStringLiteral("The key for %1 is NOT the one recorded.")
		                    .arg(session->profile().target()));
		box.setInformativeText(QStringLiteral(
		        "New fingerprint:\n%1\n\nThis happens when a server is rebuilt -- and it "
		        "is also exactly what an interception attack looks like. Do not accept "
		        "unless you know why the key changed.").arg(fp));

		QPushButton *accept = box.addButton(QStringLiteral("Accept the new key"),
		                                     QMessageBox::DestructiveRole);
		box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();

		if (box.clickedButton() == static_cast<QAbstractButton *>(accept)) {
			session->accept_host_key();
		} else {
			session->disconnect_from_host();
		}
	});

	connect(session, &bssh_session::needs_password, this, [this, session] {
		bool ok = false;
		const QString password = QInputDialog::getText(
		        this, QStringLiteral("BeerSSH"),
		        QStringLiteral("Password for %1:").arg(session->profile().target()),
		        QLineEdit::Password, QString(), &ok);

		if (!ok) {
			session->disconnect_from_host();
			return;
		}

		session->supply_password(password);
	});

	connect(session, &bssh_session::shell_exited, this, [this, session](int status) {
		if (session != m_current) return;

		m_error = status == 0
		                  ? QStringLiteral("session ended")
		                  : QStringLiteral("session ended (exit %1)").arg(status);
		update_status();
	});
	connect(session, &bssh_session::state_changed, this, [this, session] {
		/* A successful (re)connect clears whatever the last message was. */
		if (session == m_current && session->is_live()) {
			m_error.clear();
			update_status();
		}
	});

	m_pages->setCurrentWidget(m_terminal_page);
	show_session(session);
	session->connect_to_host();
}

void bssh_main_window::on_add_host() {
	/*
	 * The touch front door's "+". Deliberately the same four fields the
	 * desktop's Session page has, because they are the same profile --
	 * a second, different idea of what a host is would be a second thing
	 * to keep in step (sec 8.5).
	 */
	bool ok = false;
	const QString target = QInputDialog::getText(
	        this, QStringLiteral("Add host"),
	        QStringLiteral("user@host[:port]"), QLineEdit::Normal, QString(), &ok);
	if (!ok || target.trimmed().isEmpty()) return;

	bssh_profile_t profile;
	QString rest = target.trimmed();

	const int at = rest.lastIndexOf(QLatin1Char('@'));
	if (at >= 0) {
		profile.user = rest.left(at);
		rest = rest.mid(at + 1);
	}
	const int colon = rest.lastIndexOf(QLatin1Char(':'));
	if (colon >= 0) {
		profile.port = static_cast<quint16>(rest.mid(colon + 1).toUShort());
		rest = rest.left(colon);
	}
	profile.host = rest;
	profile.name = profile.host;

	m_profile_store->save(profile);
	if (m_host_list) m_host_list->reload();
}

void bssh_main_window::show_session(bssh_session *session) {
	if (!session) return;

	/*
	 * Only one session is on screen, and it is the one whose bell has
	 * been heard. Telling the outgoing session first means a bell that
	 * arrives during the switch lands on the right tab.
	 */
	if (m_current && m_current != session) m_current->set_on_screen(false);

	m_current = session;
	session->set_on_screen(true);

	/*
	 * Attaching is the whole operation: the view points at this session's
	 * screen, scrollback, images and router, and nothing is copied or
	 * moved. Switching tabs is four pointer assignments, which is what
	 * core principle 2 buys.
	 */
	m_terminal_view->attach_screen(&session->emulator()->screen());
	m_terminal_view->attach_scrollback(&session->emulator()->scrollback());
	m_terminal_view->attach_images(&session->emulator()->images());
	m_terminal_view->attach_emulator(session->emulator());
	m_terminal_view->attach_input_router(session->input_router());

	if (m_key_row) m_key_row->set_router(session->input_router());

	session->emulator()->set_cell_pixels(m_terminal_view->cell_width(),
	                                      m_terminal_view->cell_height());
	session->resize(m_terminal_view->cols(), m_terminal_view->rows());

	connect(session->emulator(), &bssh_emulator::damaged,
	         m_terminal_view, QOverload<>::of(&QWidget::update),
	         Qt::UniqueConnection);
	connect(session->emulator(), &bssh_emulator::images_changed,
	         m_terminal_view, QOverload<>::of(&QWidget::update),
	         Qt::UniqueConnection);

	m_terminal_view->setFocus();
	m_terminal_view->update();
	update_status();
}

void bssh_main_window::detach_session() {
	m_current = nullptr;

	m_terminal_view->attach_screen(nullptr);
	m_terminal_view->attach_scrollback(nullptr);
	m_terminal_view->attach_images(nullptr);
	m_terminal_view->attach_emulator(nullptr);
	m_terminal_view->attach_input_router(nullptr);
	if (m_key_row) m_key_row->set_router(nullptr);
}

void bssh_main_window::on_tab_changed(int index) {
	bssh_session *session = m_registry->at(index);
	if (session && session != m_current) show_session(session);
}

void bssh_main_window::on_tab_close_requested(int index) {
	bssh_session *session = m_registry->at(index);
	if (session) m_registry->close(session);
}

void bssh_main_window::on_registry_changed() {
	/*
	 * The tab bar is rebuilt from the registry rather than edited
	 * alongside it: two lists kept in step by hand drift, and the one
	 * that drifts here shows the wrong session's name over the wrong
	 * terminal.
	 */
	const QSignalBlocker blocker(m_tabs);

	while (m_tabs->count() > m_registry->count()) {
		m_tabs->removeTab(m_tabs->count() - 1);
	}
	while (m_tabs->count() < m_registry->count()) {
		m_tabs->addTab(QString());
	}

	for (int i = 0; i < m_registry->count(); ++i) {
		bssh_session *session = m_registry->at(i);
		m_tabs->setTabText(i, session->title());
		m_tabs->setTabToolTip(i, session->profile().target());

		/*
		 * The status dot (sec 8.3). Painted rather than shipped as an
		 * icon file: four solid circles are cheaper to draw than to
		 * package, and they follow the theme's own sizing.
		 */
		const int size = m_tabs->fontMetrics().height() / 2;
		m_tabs->setTabIcon(i, QIcon(bssh_tab_status::dot(
		        size, bssh_tab_status::state_for(session))));

		if (session == m_current) m_tabs->setCurrentIndex(i);
	}

	/* One tab is not worth a row of chrome, least of all on a phone. */
	m_tabs->setVisible(m_registry->count() > 1);

	/*
	 * The notification and tray tooltip count sessions, never panes or
	 * tabs (sec 5.2).
	 */
	bssh_platform::instance()->set_session_count(m_registry->live_count());

	if (m_registry->count() == 0) {
		detach_session();
		m_pages->setCurrentIndex(0);
	}

	update_status();
}

void bssh_main_window::save_layout() {
	/*
	 * Profile ids, not session ids: sessions do not survive a restart,
	 * because plain SSH cannot resume a shell (sec 5.3). What can
	 * honestly be remembered is which hosts were open and how they were
	 * arranged.
	 *
	 * Sessions opened from a command-line target have no profile behind
	 * them and are skipped rather than invented into the store.
	 */
	bssh_profile_store::layout_state_t layout;

	for (bssh_session *session : m_registry->sessions()) {
		const int id = session->profile().id;
		if (id == 0) continue;

		if (session == m_current) layout.active_index = layout.profile_ids.size();
		layout.profile_ids.append(id);
	}

	layout.pane_count = qMax(1, m_layout ? m_layout->visible_panes().size() : 1);
	m_profile_store->save_layout(layout);
}

void bssh_main_window::restore_layout() {
	const bssh_profile_store::layout_state_t layout = m_profile_store->layout();
	if (layout.is_empty()) return;

	/*
	 * The arrangement comes back; the connections do not, and cannot --
	 * each would need its password again. So the front door opens with
	 * the host that was active last time already selected, which is one
	 * keystroke from where the user left off without pretending to have
	 * restored a session.
	 *
	 * [OPEN] Reopening every remembered host at once needs key
	 * authentication (M6) to be anything but a queue of password
	 * prompts.
	 */
	const int active = layout.profile_ids.value(layout.active_index,
	                                             layout.profile_ids.first());
	const bssh_profile_t profile = m_profile_store->profile(active);
	if (!profile.is_valid()) return;

	if (m_session_dialog) m_session_dialog->set_current_profile(profile);
}

void bssh_main_window::on_failed(const QString &reason) {
	m_error = reason;
	update_status();
}

void bssh_main_window::on_grid_resized(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;

	/*
	 * window-change for the session on screen only (sec 7). With panes at
	 * M8 each computes its own geometry, so this stays per-session rather
	 * than becoming a window-wide broadcast.
	 */
	if (m_current) {
		m_current->emulator()->set_cell_pixels(m_terminal_view->cell_width(),
		                                        m_terminal_view->cell_height());
		m_current->resize(cols, rows);
	}

	update_status();
}

void bssh_main_window::update_status() {
	const bssh_platform *platform = bssh_platform::instance();

	QString session_text = QStringLiteral("no session");
	if (m_current) {
		const QMetaEnum states = QMetaEnum::fromType<bssh_ssh_session::state_t>();
		session_text = QStringLiteral("%1 %2")
		                    .arg(m_current->profile().target(),
		                          QString::fromLatin1(states.valueToKey(m_current->state())).toLower());
	}
	if (!m_error.isEmpty()) session_text += QStringLiteral(" -- ") + m_error;

	m_status_label->setText(QStringLiteral("%1  |  %2x%3 cells  |  %4")
	                         .arg(platform->name())
	                         .arg(m_cols)
	                         .arg(m_rows)
	                         .arg(session_text));
}

bssh_main_window::close_action_t bssh_main_window::close_action(bool sessions_live,
                                                                 bool remembered_background) {
	/*
	 * Nothing live: an ordinary close, and the process should go. Keeping
	 * a terminal resident with no sessions would be a background process
	 * doing nothing, which is the kind of thing users hunt down and kill.
	 */
	if (!sessions_live) return CLOSE_AND_QUIT;

	/*
	 * Live sessions and no answer on file: ASK. A terminal that vanishes
	 * into the tray unannounced is how someone loses work they believed
	 * they had closed -- and one that silently kills a running build is
	 * worse (sec 5.5).
	 */
	return remembered_background ? CLOSE_TO_BACKGROUND : ASK_FIRST;
}

void bssh_main_window::closeEvent(QCloseEvent *event) {
	const bool sessions_live = m_registry->live_count() > 0;

	close_action_t action = close_action(sessions_live,
	                                      m_asked_about_closing && m_background_on_close);

	if (action == ASK_FIRST) {
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("BeerSSH"));
		box.setText(m_registry->live_count() == 1
		                    ? QStringLiteral("A session is still connected.")
		                    : QStringLiteral("%1 sessions are still connected.")
		                              .arg(m_registry->live_count()));
		box.setInformativeText(QStringLiteral(
		        "Keep it running in the background, or disconnect and quit?"));

		QPushButton *background = box.addButton(QStringLiteral("Keep running"),
		                                         QMessageBox::AcceptRole);
		box.addButton(QStringLiteral("Disconnect and quit"), QMessageBox::DestructiveRole);
		box.setDefaultButton(background);
		box.exec();

		m_asked_about_closing = true;
		m_background_on_close = box.clickedButton() == static_cast<QAbstractButton *>(background);
		action = m_background_on_close ? CLOSE_TO_BACKGROUND : CLOSE_AND_QUIT;
	}

	if (action == CLOSE_TO_BACKGROUND) {
		/*
		 * The session is untouched: it lives in the registry, not in this
		 * window (sec 3, core principle 2), so hiding the window is the
		 * whole operation.
		 */
		bssh_platform::instance()->set_keep_alive(true);
		bssh_platform::instance()->set_session_count(m_registry->live_count());
		hide();
		event->ignore();
		return;
	}

	save_layout();

	for (bssh_session *session : m_registry->sessions()) {
		session->disconnect_from_host();
	}
	bssh_platform::instance()->set_keep_alive(false);
	event->accept();
}

bssh_main_window::front_door_t bssh_main_window::front_door_for(bool touch_primary) {
	return touch_primary ? FRONT_DOOR_HOST_LIST : FRONT_DOOR_SESSION_DIALOG;
}

