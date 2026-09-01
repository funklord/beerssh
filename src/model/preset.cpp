#include "model/preset.h"

#include "model/document_path.h"
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QUrl>

#include <QJsonArray>

namespace {

/*
 * Bumped when the shape changes, so a preset exported by a newer build
 * can be recognised rather than silently half-read (sec 10.5).
 */
const int BSSH_PRESET_SCHEMA = 1;

} /* namespace */

QJsonObject bssh_preset_t::to_json() const {
	QJsonObject object;

	object.insert(QStringLiteral("schema"), BSSH_PRESET_SCHEMA);
	object.insert(QStringLiteral("name"), name);
	object.insert(QStringLiteral("term"), term);
	object.insert(QStringLiteral("scrollback_lines"), scrollback_lines);
	object.insert(QStringLiteral("bell_enabled"), bell_enabled);
	object.insert(QStringLiteral("cursor_style"), cursor_style);
	object.insert(QStringLiteral("cursor_blink"), cursor_blink);
	object.insert(QStringLiteral("term_features"), term_features);
	object.insert(QStringLiteral("font_family"), font_family);
	object.insert(QStringLiteral("font_size"), font_size);
	object.insert(QStringLiteral("colour_scheme"), colour_scheme);
	object.insert(QStringLiteral("allow_clipboard_write"), allow_clipboard_write);
	object.insert(QStringLiteral("keepalive_seconds"), keepalive_seconds);
	object.insert(QStringLiteral("compression"), compression);
	object.insert(QStringLiteral("on_connect_command"), on_connect_command);
	object.insert(QStringLiteral("key_row_layout"), key_row_layout);

	/*
	 * id and is_builtin are deliberately NOT exported: they are facts
	 * about this installation's database, not about the preset, and a
	 * shared file carrying them would import as "builtin" somewhere it
	 * was never shipped.
	 */
	return object;
}

bssh_preset_t bssh_preset_t::from_json(const QJsonObject &object) {
	bssh_preset_t preset;

	preset.name = object.value(QStringLiteral("name")).toString();
	preset.term = object.value(QStringLiteral("term")).toString(preset.term);
	/*
	 * CLAMPED, not taken as written. sec 10.5 makes a preset a file
	 * people send each other, so these numbers arrive from wherever that
	 * file came from -- and the only thing enforcing their ranges was a
	 * spinbox in the editor, which an import does not go through.
	 *
	 * Clamped rather than refused, which is what the spinbox does to a
	 * number typed past its range: rejecting a whole preset over one
	 * field would lose the fifteen that were fine.
	 */
	preset.scrollback_lines = qBound(
	        BSSH_PRESET_MIN_SCROLLBACK,
	        object.value(QStringLiteral("scrollback_lines")).toInt(preset.scrollback_lines),
	        BSSH_PRESET_MAX_SCROLLBACK);
	preset.bell_enabled = object.value(QStringLiteral("bell_enabled")).toBool(preset.bell_enabled);
	preset.cursor_style = object.value(QStringLiteral("cursor_style")).toString(preset.cursor_style);
	preset.cursor_blink = object.value(QStringLiteral("cursor_blink")).toBool(preset.cursor_blink);
	preset.term_features = object.value(QStringLiteral("term_features"))
	                               .toString(preset.term_features);
	preset.font_family = object.value(QStringLiteral("font_family")).toString();
	preset.font_size = qBound(
	        BSSH_PRESET_MIN_POINT_SIZE,
	        object.value(QStringLiteral("font_size")).toInt(preset.font_size),
	        BSSH_PRESET_MAX_POINT_SIZE);
	preset.colour_scheme = object.value(QStringLiteral("colour_scheme")).toString();
	preset.allow_clipboard_write =
	        object.value(QStringLiteral("allow_clipboard_write")).toBool(false);
	preset.keepalive_seconds = qBound(
	        BSSH_PRESET_MIN_KEEPALIVE,
	        object.value(QStringLiteral("keepalive_seconds")).toInt(preset.keepalive_seconds),
	        BSSH_PRESET_MAX_KEEPALIVE);
	preset.compression = object.value(QStringLiteral("compression")).toBool(preset.compression);
	preset.on_connect_command = object.value(QStringLiteral("on_connect_command")).toString();
	preset.key_row_layout = object.value(QStringLiteral("key_row_layout")).toString();

	/*
	 * An imported preset is never builtin and never keeps a foreign id.
	 * Reading either from the file would let a shared preset claim to be
	 * one of the shipped ones, which are the things users clone from.
	 */
	preset.id = 0;
	preset.is_builtin = false;
	return preset;
}

QList<bssh_preset_t> bssh_preset_t::builtins() {
	QList<bssh_preset_t> presets;

	/*
	 * The starter set (sec 10.4). These double as documentation of what a
	 * preset can express, which is why each one differs in a way that
	 * matters rather than in cosmetics.
	 */
	bssh_preset_t linux_default;
	linux_default.name = QStringLiteral("Default Linux");
	linux_default.is_builtin = true;
	presets.append(linux_default);

	bssh_preset_t sbc;
	sbc.name = QStringLiteral("Raspberry Pi / SBC");
	sbc.is_builtin = true;
	/* A slow link and a small machine: less history, keepalive sooner. */
	sbc.scrollback_lines = 2000;
	sbc.keepalive_seconds = 30;
	sbc.compression = true;
	presets.append(sbc);

	bssh_preset_t openwrt;
	openwrt.name = QStringLiteral("OpenWrt / BusyBox router");
	openwrt.is_builtin = true;
	/*
	 * BusyBox's ash and a router's terminfo know far less than
	 * xterm-256color claims. Promising colour a device cannot deliver is
	 * how a TUI ends up drawing escape sequences as text.
	 */
	openwrt.term = QStringLiteral("vt100");
	openwrt.scrollback_lines = 1000;
	openwrt.compression = true;
	presets.append(openwrt);

	bssh_preset_t bsd;
	bsd.name = QStringLiteral("BSD");
	bsd.is_builtin = true;
	bsd.term = QStringLiteral("xterm");
	presets.append(bsd);

	bssh_preset_t network_switch;
	network_switch.name = QStringLiteral("Network switch / serial-ish");
	network_switch.is_builtin = true;
	/*
	 * Conservative on purpose: switch consoles are frequently a serial
	 * line wearing an SSH costume, and box-drawing or a bell can leave
	 * one in a state that needs a power cycle.
	 */
	network_switch.term = QStringLiteral("vt100");
	network_switch.bell_enabled = false;
	network_switch.scrollback_lines = 10000;
	presets.append(network_switch);

	bssh_preset_t tmux_first;
	tmux_first.name = QStringLiteral("tmux-first");
	tmux_first.is_builtin = true;
	/*
	 * The available answer to sec 5.3: plain SSH cannot resume a shell,
	 * so attach to one that outlives the connection.
	 */
	tmux_first.on_connect_command = QStringLiteral("tmux attach || tmux new");
	presets.append(tmux_first);

	return presets;
}

/*
 * Qt reads and writes INI through a file path; Android hands out
 * content:// URIs, which QFile understands and QSettings does not. Rather
 * than write a second INI implementation for one platform, the bytes move
 * through a temporary file and QSettings stays the only thing that parses
 * the format. The two path questions themselves live in
 * model/document_path.h, shared with the OpenSSH config import (sec 10.5).
 */
bool bssh_preset_t::write_ini(const QString &raw_path) const {
	const QString path = bssh_normalise_document_path(raw_path);

	QTemporaryDir staging;
	const QString target = bssh_is_local_document_path(path)
	        ? path
	        : staging.filePath(QStringLiteral("preset.ini"));

	{
		QSettings file(target, QSettings::IniFormat);
		file.clear();
		file.beginGroup(QStringLiteral("preset"));

		const QJsonObject json = to_json();
		for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
			if (it.value().isBool()) {
				file.setValue(it.key(), it.value().toBool() ? QStringLiteral("true")
				                                            : QStringLiteral("false"));
			} else if (it.value().isDouble()) {
				file.setValue(it.key(), it.value().toInt());
			} else {
				file.setValue(it.key(), it.value().toString());
			}
		}

		file.endGroup();
		file.sync();

		if (file.status() != QSettings::NoError) return false;
	}

	if (bssh_is_local_document_path(path)) return true;

	QFile source(target);
	QFile destination(path);
	if (!source.open(QIODevice::ReadOnly)) return false;
	if (!destination.open(QIODevice::WriteOnly)) return false;

	return destination.write(source.readAll()) >= 0;
}

bssh_preset_t bssh_preset_t::read_ini(const QString &raw_path, bool *ok) {
	if (ok) *ok = false;

	const QString path = bssh_normalise_document_path(raw_path);

	QTemporaryDir staging;
	QString source = path;

	if (!bssh_is_local_document_path(path)) {
		QFile incoming(path);
		if (!incoming.open(QIODevice::ReadOnly)) return bssh_preset_t();

		source = staging.filePath(QStringLiteral("preset.ini"));
		QFile local(source);
		if (!local.open(QIODevice::WriteOnly)) return bssh_preset_t();
		local.write(incoming.readAll());
		local.close();
	}

	QSettings file(source, QSettings::IniFormat);
	if (file.status() != QSettings::NoError) return bssh_preset_t();

	/*
	 * [preset] is what this project writes, but a file someone assembled
	 * by copying a section out of presets.ini carries that section's own
	 * name instead. Both are accepted: refusing the obvious way to make
	 * one of these by hand would defeat the point of a text format.
	 */
	QString section = QStringLiteral("preset");
	if (!file.childGroups().contains(section)) {
		if (file.childGroups().isEmpty()) return bssh_preset_t();
		section = file.childGroups().first();
	}

	file.beginGroup(section);

	QJsonObject json = bssh_preset_t().to_json();
	json.insert(QStringLiteral("name"), section);

	for (const QString &key : file.childKeys()) {
		/*
		 * Everything in the section is read; nothing is filtered here.
		 *
		 * This used to skip "id" and "builtin" with a comment saying
		 * they are never imported. The field is called is_builtin, so
		 * the second name matched nothing -- and the filter was dead
		 * either way, because from_json() hard-sets both and this
		 * function sets them again below. A guard that cannot fire
		 * reads as protection, which is worse than no guard: it invites
		 * the next reader to weaken the one that does the work.
		 */
		const QJsonValue reference = json.value(key);
		const QString raw = file.value(key).toString();

		if (reference.isBool()) {
			json.insert(key, raw.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
			                  raw == QStringLiteral("1"));
		} else if (reference.isDouble()) {
			json.insert(key, raw.toInt());
		} else {
			json.insert(key, raw);
		}
	}

	file.endGroup();

	bssh_preset_t preset = bssh_preset_t::from_json(json);
	if (preset.name.isEmpty()) preset.name = section;

	/*
	 * Arrives as a new, editable preset of this installation's own, never
	 * as a shipped one and never claiming an id.
	 */
	preset.id = 0;
	preset.is_builtin = false;

	if (ok) *ok = true;
	return preset;
}
