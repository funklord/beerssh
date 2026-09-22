#include "ui/term_features_editor.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "ui/scroll_guard.h"
#include "ui/touch_metrics.h"
#include "ui/wrapped_note.h"

bssh_term_features_editor::bssh_term_features_editor(const QString &spec,
                                                      QWidget *parent)
    : QDialog(parent) {
	setWindowTitle(QStringLiteral("Terminal features"));

	/*
	 * An unreadable spec opens as everything on rather than refusing.
	 * from_spec reports the error and returns what it had read so far,
	 * and a dialog that will not open is no way to fix a string somebody
	 * mistyped by hand -- opening at the default and letting them tick
	 * what they meant is.
	 */
	QString ignored;
	const bssh_term_features_t features =
	        bssh_term_features_t::from_spec(spec, ignored);

	QVBoxLayout *outer = new QVBoxLayout(this);

	QLabel *note = bssh_wrapped_note(
	        QStringLiteral("Turn these off to see how a program behaves against a "
	                        "terminal that lacks them. Switching one off also stops "
	                        "this terminal answering that it has it, so a program "
	                        "that asks is told no."),
	        this);
	outer->addWidget(note);

	/*
	 * Scrolled, because fourteen rows at a touch height is taller than a
	 * phone and this dialog opens on one. The guard is the same one
	 * every long surface here uses.
	 */
	QWidget *body = new QWidget(this);
	QVBoxLayout *list = new QVBoxLayout(body);
	list->setContentsMargins(0, 0, 0, 0);

	for (int i = 0; i < bssh_term_features_t::FEATURE_COUNT; ++i) {
		const bssh_term_features_t::feature_t feature =
		        bssh_term_features_t::feature_t(i);

		QCheckBox *box = new QCheckBox(
		        QString::fromLatin1(bssh_term_features_t::name(feature)), body);

		/*
		 * Named for the spec name rather than numbered, so a test can
		 * ask for one by the name sec 7.1 uses and a renumbering of the
		 * enum cannot silently point it at another feature.
		 */
		box->setObjectName(QStringLiteral("feature_")
		                    + QString::fromLatin1(bssh_term_features_t::name(feature)));
		box->setToolTip(
		        QString::fromLatin1(bssh_term_features_t::description(feature)));
		box->setChecked(features.enabled(feature));

		m_boxes.append(box);
		list->addWidget(box);
	}

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setWidget(body);

	/*
	 * A floor of three rows, because a scroll area's own minimum is
	 * tiny by design -- it scrolls -- and without one this dialog's
	 * minimum was 274x197, which `make shots` rendered with the
	 * explanatory note clipped through its top and two checkboxes
	 * visible. That is the wrapped-note failure this tree has already
	 * paid for, arriving from the other side: there a label
	 * under-reported its height, here it reports it correctly and the
	 * dialog is allowed to be shorter than the sum.
	 *
	 * Three rather than all fourteen: the list is MEANT to scroll on a
	 * phone, and a minimum that demanded every row would be a dialog
	 * that does not fit the screen it opens on.
	 */
	scroll->setMinimumHeight(3 * BSSH_TOUCH_TARGET_DP);
	outer->addWidget(scroll, 1);

	QDialogButtonBox *buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel
	                | QDialogButtonBox::RestoreDefaults,
	        this);

	QPushButton *reset = buttons->button(QDialogButtonBox::RestoreDefaults);
	reset->setText(QStringLiteral("&All on"));
	reset->setToolTip(QStringLiteral("Put every feature back, which is what a host "
	                                  "with nothing set here gets."));

	/*
	 * Cancel is the default, as it is in the colour editor and for the
	 * same reason: a stray Return must not commit a terminal somebody
	 * was still reading.
	 */
	buttons->button(QDialogButtonBox::Ok)->setDefault(false);
	buttons->button(QDialogButtonBox::Cancel)->setDefault(true);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(reset, &QPushButton::clicked, this, [this] {
		for (QCheckBox *box : m_boxes) box->setChecked(true);
	});

	outer->addWidget(buttons);

	/*
	 * After the page is populated, per the header: the checkboxes are
	 * inside a scroll area, and a wheel over one of them must scroll the
	 * list rather than toggle the feature under the pointer.
	 */
	bssh_dont_steal_the_wheel(this);

	/*
	 * Asked of the platform rather than passed in, and NOT hardcoded to
	 * false the way the colour editor's call is: that dialog's controls
	 * are tool buttons it sizes itself, and this one's are checkboxes,
	 * which stand 23dp -- half sec 6.2's floor. The preset editor
	 * learned the same lesson about the same widget, and its comment
	 * says why a page that grows a control must not also have to
	 * remember a second place.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	bssh_apply_touch_heights(this, platform && platform->is_touch_primary());
}

QString bssh_term_features_editor::spec() const {
	bssh_term_features_t features;

	for (int i = 0; i < m_boxes.size(); ++i) {
		features.set(bssh_term_features_t::feature_t(i), m_boxes.at(i)->isChecked());
	}

	/*
	 * to_spec writes the OFF ones as `-name` and nothing when they are
	 * all on, which is exactly what a preset wants to store: an empty
	 * field means a whole terminal, so a host that was never asked this
	 * question keeps a field it can be read from.
	 */
	return features.to_spec();
}

QString bssh_term_features_editor::summary(const QString &spec) {
	QString ignored;
	const bssh_term_features_t features =
	        bssh_term_features_t::from_spec(spec, ignored);

	int off = 0;
	for (int i = 0; i < bssh_term_features_t::FEATURE_COUNT; ++i) {
		if (!features.enabled(bssh_term_features_t::feature_t(i))) ++off;
	}

	if (off == 0) return QStringLiteral("All on");
	return QStringLiteral("%1 off").arg(off);
}
