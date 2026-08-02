#include "ui/main_window.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaEnum>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "ui/key_row.h"
#include "ui/terminal_view.h"

bssh_main_window::bssh_main_window(const QString &target, QWidget *parent)
    : QWidget(parent),
      m_terminal_view(new bssh_terminal_view(this)),
      m_status_label(new QLabel(this)),
      m_session(nullptr),
      m_emulator(nullptr),
      m_input_router(nullptr),
      m_key_row(nullptr),
      m_cols(0),
      m_rows(0) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(m_terminal_view, 1);
	layout->addWidget(m_status_label, 0);

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
	 * The emulator owns the screen; the view only draws it (sec 3, core
	 * principle 2). It is created with the window rather than with the
	 * session so that output arriving before -- or after -- a connection
	 * still has somewhere to land.
	 */
	m_emulator = new bssh_emulator(m_cols, m_rows, this);
	m_terminal_view->attach_screen(&m_emulator->screen());
	m_terminal_view->attach_images(&m_emulator->images());
	m_terminal_view->attach_scrollback(&m_emulator->scrollback());

	/*
	 * The emulator cannot know how big a cell is -- only the view, which
	 * owns the font, can. Without this an inline image gets a zero-cell
	 * footprint and lands invisible (sec 7.1).
	 */
	m_emulator->set_cell_pixels(m_terminal_view->cell_width(),
	                             m_terminal_view->cell_height());

	connect(m_emulator, &bssh_emulator::images_changed,
	         m_terminal_view, QOverload<>::of(&QWidget::update));

	connect(m_emulator, &bssh_emulator::damaged,
	         m_terminal_view, QOverload<>::of(&QWidget::update));
	connect(m_emulator, &bssh_emulator::output_ready,
	         this, &bssh_main_window::on_emulator_output);
	connect(m_emulator, &bssh_emulator::title_changed, this, [this](const QString &title) {
		/* OSC title -> window title on desktop, tab label at M7 (sec 8.3). */
		setWindowTitle(title.isEmpty() ? QStringLiteral("BeerSSH")
		                                : QStringLiteral("%1 - BeerSSH").arg(title));
	});

	m_input_router = new bssh_input_router(m_emulator, this);
	m_terminal_view->attach_input_router(m_input_router);
	m_terminal_view->setFocus();

	/*
	 * Between the terminal and the status line, so it sits directly above
	 * the soft keyboard when one is up (sec 6.2). Hidden unless touch is
	 * the primary input -- it is built on every platform regardless, so
	 * desktop can enable it and so it stays compiled and tested.
	 */
	m_key_row = new bssh_key_row(m_input_router, this);
	layout->insertWidget(1, m_key_row);

	connect(m_input_router, &bssh_input_router::shortcut_pressed,
	         this, [this](bssh_input_router::shortcut_t shortcut) {
		if (shortcut == bssh_input_router::SHORTCUT_PASTE) {
			m_input_router->paste(QGuiApplication::clipboard()->text());
		}
		if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_UP) {
			m_terminal_view->scroll_by(m_terminal_view->rows() / 2);
		}
		if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_DOWN) {
			m_terminal_view->scroll_by(-m_terminal_view->rows() / 2);
		}
		/* SHORTCUT_COPY needs a selection to copy, which arrives with
		 * mouse selection (sec 6.4) -- not yet built. */
	});

	update_status();

	if (!target.isEmpty()) connect_to(target);
}

void bssh_main_window::connect_to(const QString &target) {
	/* [user@]host[:port] -- see the header on why this is temporary. */
	QString rest = target;
	QString user;
	quint16 port = 22;

	const int at = rest.lastIndexOf(QLatin1Char('@'));
	if (at >= 0) {
		user = rest.left(at);
		rest = rest.mid(at + 1);
	}
	const int colon = rest.lastIndexOf(QLatin1Char(':'));
	if (colon >= 0) {
		port = static_cast<quint16>(rest.mid(colon + 1).toUShort());
		rest = rest.left(colon);
	}
	if (user.isEmpty()) user = qEnvironmentVariable("USER");

	m_target = user + QLatin1Char('@') + rest;

	bool ok = false;
	m_password = QInputDialog::getText(this, QStringLiteral("BeerSSH"),
	                                    QStringLiteral("Password for %1:").arg(m_target),
	                                    QLineEdit::Password, QString(), &ok);
	if (!ok) return;

	/*
	 * The password is held in a QString until the auth step consumes it,
	 * and cleared immediately after. That is NOT the same as being
	 * handled safely -- QString does not lock or scrub its memory, and
	 * proper key/credential handling is sec 11's Keystore work at M6.
	 * Noted here rather than left looking finished.
	 */
	m_session = bssh_ssh_session::create(this);

	connect(m_session, &bssh_ssh_session::bytes_received,
	         m_emulator, &bssh_emulator::feed);
	connect(m_session, &bssh_ssh_session::state_changed,
	         this, &bssh_main_window::on_state_changed);
	connect(m_session, &bssh_ssh_session::failed,
	         this, &bssh_main_window::on_failed);

	m_session->connect_to_host(rest, port, user);
}

void bssh_main_window::on_state_changed(bssh_ssh_session::state_t state) {
	switch (state) {
	case bssh_ssh_session::AUTHENTICATING:
		m_session->authenticate_password(m_password);
		m_password.clear();
		break;
	case bssh_ssh_session::CONNECTED:
		m_session->open_shell(m_terminal_view->cols(), m_terminal_view->rows());
		break;
	default:
		break;
	}

	update_status();
}

void bssh_main_window::on_emulator_output() {
	/*
	 * Replies the terminal owes the remote -- cursor-position reports and
	 * device attributes, and later mouse and bracketed-paste events. A
	 * client that never sends these hangs any editor that asks the
	 * terminal a question at startup and waits for the answer.
	 */
	if (!m_session) return;

	const QByteArray reply = m_emulator->take_output();
	if (!reply.isEmpty()) m_session->send(reply);
}

void bssh_main_window::on_failed(const QString &reason) {
	m_error = reason;
	update_status();
}

void bssh_main_window::on_grid_resized(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;

	/*
	 * window-change for this session only (sec 7). With panes at M8 each
	 * one computes its own geometry, so this stays per-session rather
	 * than becoming a window-wide broadcast.
	 */
	if (m_emulator) {
		m_emulator->set_cell_pixels(m_terminal_view->cell_width(),
		                             m_terminal_view->cell_height());
		m_emulator->resize(cols, rows);
	}
	if (m_session) m_session->resize(cols, rows);

	update_status();
}

void bssh_main_window::update_status() {
	const bssh_platform *platform = bssh_platform::instance();

	QString session_text = QStringLiteral("no session");
	if (m_session) {
		const QMetaEnum states = QMetaEnum::fromType<bssh_ssh_session::state_t>();
		session_text = QStringLiteral("%1 %2")
		                    .arg(m_target, QString::fromLatin1(states.valueToKey(m_session->state())).toLower());
	}
	if (!m_error.isEmpty()) session_text += QStringLiteral(" -- ") + m_error;

	m_status_label->setText(QStringLiteral("%1  |  %2x%3 cells  |  %4")
	                         .arg(platform->name())
	                         .arg(m_cols)
	                         .arg(m_rows)
	                         .arg(session_text));
}
