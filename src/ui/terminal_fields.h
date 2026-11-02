#ifndef BSSH_TERMINAL_FIELDS_H
#define BSSH_TERMINAL_FIELDS_H

#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QPushButton;
class QSpinBox;

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

	/* Shows `settings` -- resolved, so every key is there -- quietly. */
	void set_values(const QJsonObject &settings);

	/* What the form shows, for every key it holds. */
	QJsonObject values() const;

	/*
	 * The keys where the form differs from `inherited`, with the form's
	 * values: what a host records as its own, or the device as its
	 * default. Compared as the settings mean them -- an empty TERM is the
	 * built-in one and an unset key row the default row -- so showing a
	 * value and leaving it alone records nothing.
	 */
	QJsonObject differing_from(const QJsonObject &inherited) const;

	/* The same comparison for values held anywhere, not only on a form. */
	static QJsonObject differing(const QJsonObject &shown, const QJsonObject &inherited);

	QFormLayout *form() const { return m_form; }

signals:
	/* The user changed a field. Not emitted by set_values(). */
	void changed();

private:
	void emit_changed();

	QFormLayout *m_form;
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
};

#endif /* BSSH_TERMINAL_FIELDS_H */
