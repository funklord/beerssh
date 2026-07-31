#include "ui/main_window.h"

#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMetaEnum>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "ui/terminal_view.h"

bssh_main_window::bssh_main_window(const QString &target, QWidget *parent)
    : QWidget(parent),
      m_terminal_view(new bssh_terminal_view(this)),
      m_status_label(new QLabel(this)),
      m_session(nullptr),
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
	         m_terminal_view, &bssh_terminal_view::feed);
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
