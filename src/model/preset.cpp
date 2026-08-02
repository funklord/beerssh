#include "model/preset.h"

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
	object.insert(QStringLiteral("font_family"), font_family);
	object.insert(QStringLiteral("font_size"), font_size);
	object.insert(QStringLiteral("colour_scheme"), colour_scheme);
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
	preset.scrollback_lines =
	        object.value(QStringLiteral("scrollback_lines")).toInt(preset.scrollback_lines);
	preset.bell_enabled = object.value(QStringLiteral("bell_enabled")).toBool(preset.bell_enabled);
	preset.cursor_style = object.value(QStringLiteral("cursor_style")).toString(preset.cursor_style);
	preset.cursor_blink = object.value(QStringLiteral("cursor_blink")).toBool(preset.cursor_blink);
	preset.font_family = object.value(QStringLiteral("font_family")).toString();
	preset.font_size = object.value(QStringLiteral("font_size")).toInt(preset.font_size);
	preset.colour_scheme = object.value(QStringLiteral("colour_scheme")).toString();
	preset.keepalive_seconds =
	        object.value(QStringLiteral("keepalive_seconds")).toInt(preset.keepalive_seconds);
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
