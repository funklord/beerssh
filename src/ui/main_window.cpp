#include "ui/main_window.h"

#include <QLabel>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "ui/terminal_view.h"

bssh_main_window::bssh_main_window(QWidget *parent)
    : QWidget(parent),
      m_terminal_view(new bssh_terminal_view(this)),
      m_status_label(new QLabel(this)),
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
	 * The status line is M0 scaffolding, not a feature: it reports which
	 * platform implementation was built in and the grid size the view
	 * computed, which together are the whole of what M0 claims to prove.
	 * At M7 this space becomes the per-session status (sec 8.3).
	 */
	connect(m_terminal_view, &bssh_terminal_view::grid_resized,
	         this, &bssh_main_window::on_grid_resized);

	m_cols = m_terminal_view->cols();
	m_rows = m_terminal_view->rows();
	update_status();
}

void bssh_main_window::on_grid_resized(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;
	update_status();
}

void bssh_main_window::update_status() {
	const bssh_platform *platform = bssh_platform::instance();

	m_status_label->setText(QStringLiteral("%1  |  %2x%3 cells  |  no session")
	                         .arg(platform->name())
	                         .arg(m_cols)
	                         .arg(m_rows));
}
