#ifndef BSSH_TERMINAL_FIELDS_H
#define BSSH_TERMINAL_FIELDS_H

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

/*
 * The terminal and connection fields of a host's settings (sec 10.2):
 * TERM, scrollback, the bell, the cursor, the terminal extensions, the key
 * row, keepalive, compression, the clipboard and the command run on
 * connect.
 *
 * One form for the three places that set them -- every host on this device,
 * one host of its own, and a preset -- so a field added to the settings is
 * added once. The holder, 2026-11-02: everything a preset holds gets a
 * default for every host and a value of a host's own, since nothing else
 * could set them for a host that follows no preset.
 */
class bssh_terminal_fields : public QWidget {
	Q_OBJECT

public:
	explicit bssh_terminal_fields(QWidget *parent = nullptr);

	/* The JSON keys this form holds, as bssh_preset_t names them. */
	static QStringList keys();

	/*
	 * Shows what these settings follow, `inherited` (resolved, so every
	 * key is there), with `own` -- the keys chosen here -- over it,
	 * quietly. A field is chosen once somebody changes it and stays so,
	 * even set back to what it follows, until its Default button puts it
	 * back to following: a config records choices, not values (the
	 * holder's rule, 2026-11-06). Values compared against what they
	 * follow lose a pick that equals today's default.
	 */
	void show_settings(const QJsonObject &inherited, const QJsonObject &own);

	/* What they follow changed (a template, this device's defaults): unchosen fields show it. */
	void set_inherited(const QJsonObject &inherited);

	/* Names what the fields follow, for the Default buttons' tips. */
	void set_source_name(const QString &name) { m_source_name = name; }

	/* What the form shows, for every key it holds. */
	QJsonObject values() const;

	/* The chosen keys, with what the form shows for them. */
	QJsonObject chosen_values() const;
	QSet<QString> chosen() const { return m_chosen; }


	QFormLayout *form() const { return m_form; }

	/*
	 * Whether the first heading, "Terminal", shows. A page already titled
	 * Terminal -- the phone's settings page of that name -- said it twice.
	 */
	void set_first_heading_shown(bool shown);

signals:
	/* The user changed a field. Not emitted by set_values(). */
	void changed();

private:
	void emit_changed();
	void choose(const QString &key);
	void follow(const QString &key);
	void mark(const QString &key);
	void fill(const QJsonObject &settings, const QSet<QString> &keys);
	QWidget *with_default(const QString &key, QWidget *field, const QString &label = QString());

	QFormLayout *m_form;
	QLabel *m_first_heading = nullptr;
	QLineEdit *m_term;
	QSpinBox *m_scrollback;
	QCheckBox *m_bell;
	QComboBox *m_cursor_style;
	QCheckBox *m_cursor_blink;
	QPushButton *m_features;
	QComboBox *m_key_row;
	QSpinBox *m_keepalive;
	QCheckBox *m_compression;
	QCheckBox *m_clipboard;
	QLineEdit *m_on_connect;

	/* Held here rather than on a widget: the features are edited in a
	 * dialog of their own, and the button only shows a summary. */
	QString m_features_spec;
	bool m_quiet = false;

	QJsonObject m_inherited;
	QSet<QString> m_chosen;
	QString m_source_name = QStringLiteral("the default");
	QHash<QString, QToolButton *> m_defaults;
	QHash<QString, QWidget *> m_fields_by_key;
};

#endif /* BSSH_TERMINAL_FIELDS_H */
