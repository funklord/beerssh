#include "ui/app_settings_view.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "model/app_settings.h"
#include "platform/platform.h"

bssh_app_settings_view::bssh_app_settings_view(QWidget *parent)
    : QWidget(parent) {
	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	QVBoxLayout *outer = new QVBoxLayout(this);

	QLabel *heading = new QLabel(QStringLiteral("<b>This device</b>"), this);
	outer->addWidget(heading);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_key_row = new QComboBox(this);
	m_key_row->addItem(QStringLiteral("Automatic"), int(bssh_app_settings::KEY_ROW_AUTO));
	m_key_row->addItem(QStringLiteral("Always show"), int(bssh_app_settings::KEY_ROW_ALWAYS));
	m_key_row->addItem(QStringLiteral("Never show"), int(bssh_app_settings::KEY_ROW_NEVER));
	if (touch) m_key_row->setMinimumHeight(48);

	form->addRow(QStringLiteral("On-screen keys"), m_key_row);
	outer->addLayout(form);

	/*
	 * Says what "automatic" actually decides, because the honest answer is
	 * not "on Android": sec 6.2 gates the row on touch being the PRIMARY
	 * input, since an OS check gets a convertible in tablet mode and a
	 * tablet with a keyboard docked wrong in opposite directions.
	 */
	QLabel *note = new QLabel(
	        QStringLiteral("Automatic shows the key row when touch is this device's main "
	                        "input. Override it if you have a keyboard attached to a "
	                        "tablet, or a touchscreen on a laptop."), this);
	note->setWordWrap(true);
	outer->addWidget(note);

	outer->addStretch(1);

	if (touch) {
		QPushButton *back = new QPushButton(QStringLiteral("Back"), this);
		back->setMinimumHeight(48);
		connect(back, &QPushButton::clicked, this, &bssh_app_settings_view::back_requested);
		outer->addWidget(back);
	}

	const bssh_app_settings settings;
	const int index = m_key_row->findData(int(settings.key_row_visibility()));
	m_key_row->setCurrentIndex(index < 0 ? 0 : index);

	connect(m_key_row, &QComboBox::currentIndexChanged, this, [this] {
		bssh_app_settings stored;
		stored.set_key_row_visibility(
		        bssh_app_settings::key_row_visibility_t(m_key_row->currentData().toInt()));
		emit changed();
	});
}
