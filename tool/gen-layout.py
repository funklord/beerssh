#!/usr/bin/env python3
"""Generate a keyboard layout table for keyboard/soft_keyboard_layout.cpp.

    tool/gen-layout.py us            > table.txt
    tool/gen-layout.py se --name Swedish

A layout cannot be read from one file under /usr/share/X11/xkb/symbols.
se(basic) defines ZERO keys -- it is `include "latin(type2)"` plus
`include "se(se)"` -- so the include chain has to be resolved, which is
what setxkbmap and xkbcomp do and what this wraps. That makes this a
DEVELOPER tool: it runs on a machine with xkeyboard-config installed and
its output is committed, so nothing at runtime depends on X.

Nothing here is transcribed. The slot names come from xkb, the characters
from keysymdef.h's own U+ comments, and the dead-key bases from the
system Compose file -- three files this machine already has, rather than
three tables somebody typed from memory and nobody can re-check.
"""

import argparse
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

# The typing block, in the order the geometry walks it. LSGT is the ISO
# key between left Shift and AB01; ANSI has no such key, so it is read
# and reported but not emitted unless asked for.
ROWS = (["TLDE"] + ["AE%02d" % i for i in range(1, 13)]
         + ["AD%02d" % i for i in range(1, 13)] + ["BKSL"]
         + ["AC%02d" % i for i in range(1, 12)]
         + ["AB%02d" % i for i in range(1, 11)])

KEYSYMDEF = Path("/usr/include/X11/keysymdef.h")
COMPOSE = Path("/usr/share/X11/locale/en_US.UTF-8/Compose")


def keysym_table():
	"""name -> character, from keysymdef.h.

	Resolved through the keysym's NUMERIC value rather than straight from
	the U+ comment, because the comment is not on every line. xkb still
	spells several keysyms the old way -- guillemotleft, not
	guillemetleft -- and keysymdef.h carries those as bare aliases whose
	comment says "deprecated alias for ..." and gives no codepoint. Read
	name-to-comment and AltGr+[ silently loses its character; read
	name-to-value-to-codepoint and the alias resolves like anything else.
	"""
	if not KEYSYMDEF.exists():
		sys.exit("gen-layout: %s is missing; install libx11-dev" % KEYSYMDEF)

	text = KEYSYMDEF.read_text()
	define = re.compile(r'^#define\s+XK_(\w+)\s+0x([0-9a-fA-F]+)', re.M)
	unicode_for = re.compile(r'^#define\s+XK_\w+\s+0x([0-9a-fA-F]+)\s*'
	                          r'/\*[^*]*?U\+([0-9A-Fa-f]{4,6})', re.M)

	codepoint = {int(m.group(1), 16): chr(int(m.group(2), 16))
	              for m in unicode_for.finditer(text)}
	table = {}
	for found in define.finditer(text):
		value = int(found.group(2), 16)
		if value in codepoint:
			table[found.group(1)] = codepoint[value]
		elif 0x01000000 <= value <= 0x0110ffff:
			table[found.group(1)] = chr(value - 0x01000000)
	return table


def dead_key_table():
	"""dead_x -> the character it produces with space, from Compose.

	Flattening rather than composing, and the Compose file is what says
	to what: <dead_acute> <space> gives an ASCII apostrophe, not an acute
	accent. That is the right answer for a terminal and it is somebody
	else's decision rather than ours.
	"""
	if not COMPOSE.exists():
		sys.exit("gen-layout: %s is missing" % COMPOSE)

	table = {}
	pattern = re.compile(r'^<(dead_\w+)>\s+<space>\s*:\s*"((?:[^"\\]|\\.)+)"')
	for line in COMPOSE.read_text(errors="replace").splitlines():
		found = pattern.match(line)
		if found:
			text = found.group(2).replace('\\"', '"').replace("\\\\", "\\")
			if len(text) == 1:
				table[found.group(1)] = text
	return table


def resolve(layout, variant):
	"""The layout with every include walked, as an xkb keymap."""
	spec = ["-layout", layout] + (["-variant", variant] if variant else [])
	try:
		printed = subprocess.run(["setxkbmap", *spec, "-print"],
		                          capture_output=True, text=True, check=True)
		compiled = subprocess.run(["xkbcomp", "-xkb", "-", "-"],
		                           input=printed.stdout, capture_output=True,
		                           text=True, check=True)
	except FileNotFoundError as missing:
		sys.exit("gen-layout: %s -- install x11-xkb-utils and xkb-data" % missing)
	except subprocess.CalledProcessError as failed:
		sys.exit("gen-layout: could not resolve %s: %s"
		          % (layout, failed.stderr.strip()[:200]))
	return compiled.stdout


def parse_keys(keymap):
	"""slot -> [level 1..n], from a resolved keymap."""
	keys = {}
	for found in re.finditer(r'key\s+<(\w+)>\s*\{(.*?)\};', keymap, re.S):
		slot, body = found.group(1), found.group(2)
		levels = re.findall(r'symbols\[Group1\]\s*=\s*\[([^\]]*)\]', body)
		if not levels:
			levels = re.findall(r'\[\s*([^\]]*)\s*\]', body)
		if levels:
			keys[slot] = [s.strip() for s in levels[0].split(",")]
	return keys


def character(symbol, keysyms, deads):
	"""What a key sends for this keysym, or "" for nothing it should.

	A COMBINING mark is nothing it should. Flattening a dead key gives
	whatever the Compose file pairs it with space, and for a few that is
	a combining character -- dead_horn gives U+031B, dead_belowdot gives
	U+0323 -- which has no business being sent alone: the terminal will
	stack it on whatever glyph came before, so the key appears to corrupt
	the character to its left rather than to type anything. The spacing
	modifiers next to them in the same table are fine and are kept.
	"""
	if not symbol or symbol == "NoSymbol":
		return ""
	if symbol in deads:
		flattened = deads[symbol]
		return "" if unicodedata.combining(flattened) else flattened
	if symbol in keysyms:
		return keysyms[symbol]
	if len(symbol) == 1:
		return symbol
	found = re.fullmatch(r'U([0-9A-Fa-f]{4,6})', symbol)
	if found:
		return chr(int(found.group(1), 16))
	found = re.fullmatch(r'0x0*1([0-9A-Fa-f]{4,6})', symbol)
	if found:
		return chr(int(found.group(1), 16))
	return ""


def escape(text):
	if not text:
		return '""'
	out = ""
	for ch in text:
		if ch == "\\":
			out += "\\\\"
		elif ch == '"':
			out += '\\"'
		elif ord(ch) < 128:
			out += ch
		else:
			out += "\\u%04x" % ord(ch)
	return '"%s"' % out


def main(argv):
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("layout", help="an xkb layout name, such as us or se")
	parser.add_argument("--variant", default=None)
	parser.add_argument("--altgr-from", default=None, metavar="VARIANT",
	                     help="take levels 3 and 4 from this variant instead "
	                          "(us --altgr-from intl is how the shipped US "
	                          "table keeps its quote keys live)")
	parser.add_argument("--iso", action="store_true",
	                     help="emit LSGT as well, for an ISO geometry")
	args = parser.parse_args(argv[1:])

	keysyms, deads = keysym_table(), dead_key_table()
	base = parse_keys(resolve(args.layout, args.variant))
	upper = parse_keys(resolve(args.layout, args.altgr_from)) if args.altgr_from else base

	slots = ROWS + (["LSGT"] if args.iso else [])
	missing = [s for s in slots if s not in base]
	if missing:
		sys.exit("gen-layout: %s defines no %s -- the geometry would have "
		          "a hole in it" % (args.layout, ", ".join(missing)))

	rows, flattened = [], []
	for slot in slots:
		low, high = base.get(slot, []), upper.get(slot, [])
		for index, symbol in enumerate(low[:2]):
			if symbol.startswith("dead_"):
				flattened.append("%s level %d: %s -> %s"
				                  % (slot, index + 1, symbol,
				                     deads.get(symbol, "?")))

		levels = [character(low[0] if low else "", keysyms, deads),
		           character(low[1] if len(low) > 1 else "", keysyms, deads),
		           character(high[2] if len(high) > 2 else "", keysyms, deads),
		           character(high[3] if len(high) > 3 else "", keysyms, deads)]

		# Where the AltGr source merely repeats the base -- what a variant
		# that deadens levels 1 and 2 leaves behind -- there is no third
		# legend and the key must not offer one twice.
		if levels[2] == levels[0] and levels[3] == levels[1]:
			levels[2] = levels[3] = ""
		# Shift on a letter is a case mapping, computed rather than listed.
		if levels[0].isalpha() and levels[1] == levels[0].upper():
			levels[1] = ""
		if levels[0].isalpha() and levels[3] == levels[0].upper():
			levels[3] = ""

		rows.append("\t{ \"%s\", { %s, %s, %s, %s } },"
		             % (slot, escape(levels[0]), escape(levels[1]),
		                escape(levels[2]), escape(levels[3])))

	for note in flattened:
		print("gen-layout: flattened %s" % note, file=sys.stderr)
	print("\n".join(rows))
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
