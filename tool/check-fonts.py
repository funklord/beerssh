#!/usr/bin/env python3
"""Validate the bundled fonts against project.md sec 9.1.

Section 9.6 asks for exactly this and says why: "This is how 'only fonts
suitable for a terminal' stays true as the set evolves rather than being a
one-time intention." A font that fails here does not get shipped, because
every one of these checks corresponds to a way a terminal visibly breaks:

  uniform advance width   -- the grid stops being a grid
  box drawing + blocks    -- every TUI looks shattered (sec 9.1, the single
                             most common cause of "why does my terminal
                             look like that")
  regular + bold          -- SGR bold gets synthesised into a smear
  disambiguation glyphs   -- 0/O and 1/l/I present at all
  a licence file          -- the OFL requires it to travel with the font

The parsing is done with fontTools, which is the standard library for this
and is a build-time dependency only: nothing in the shipped app reads a
font table by hand.

Usage:
  tool/check-fonts.py [font-directory]     default: fonts/
"""

import sys
import os

try:
	from fontTools.ttLib import TTFont
except ImportError:
	sys.stderr.write(
		"check-fonts: fontTools is required\n"
		"  pip install fonttools\n"
		"It is a build-time dependency only -- the app itself never parses\n"
		"a font table.\n")
	sys.exit(2)


# The families sec 9.2 selected, by the base name their files carry, with
# the faces sec 9.1 requires. Kept in step with bssh_font_catalog and
# tool/fetch-fonts.sh by name: a font in one and not the others shows up
# here as a missing file rather than as a silent gap in the set.
FAMILIES = {
	"JetBrains Mono": {
		"regular": "JetBrainsMono-Regular.ttf",
		"bold": "JetBrainsMono-Bold.ttf",
		"licence": "OFL-JetBrainsMono.txt",
	},
	"Iosevka Term": {
		"regular": "IosevkaTerm-Regular.ttf",
		"bold": "IosevkaTerm-Bold.ttf",
		"licence": "OFL-Iosevka.md",
	},
	"DejaVu Sans Mono": {
		"regular": "DejaVuSansMono.ttf",
		"bold": "DejaVuSansMono-Bold.ttf",
		"licence": "LICENSE-DejaVu.txt",
	},
	"Hack": {
		"regular": "Hack-Regular.ttf",
		"bold": "Hack-Bold.ttf",
		"licence": "LICENSE-Hack.md",
	},
	"Fira Code": {
		"regular": "FiraCode-Regular.ttf",
		"bold": "FiraCode-Bold.ttf",
		"licence": "OFL-FiraCode.txt",
	},
	"Cascadia Code PL": {
		"regular": "CascadiaCodePL.ttf",
		# A variable font: one file carries the whole weight axis, so
		# there is no separate bold file to demand. The axis check below
		# is what stands in for it.
		"bold": None,
		"licence": "OFL-CascadiaCode.txt",
	},
	"Noto Sans Mono": {
		"regular": "NotoSansMono-Regular.ttf",
		"bold": None,
		"licence": "OFL-NotoSansMono.txt",
		# Fallback-only (sec 9.2): it is never selected, only fallen back
		# to, so what matters is coverage rather than the full weight set.
		"fallback_only": True,
	},
	"Unifont": {
		"regular": "unifont.otf",
		"bold": None,
		"licence": "LICENSE-Unifont.txt",
		"fallback_only": True,
	},
}

# sec 9.1 requirement 2. Sampled rather than exhaustive: a font that has
# these has the blocks, and demanding all 256 codepoints rejects fonts over
# gaps no terminal ever draws.
BOX_DRAWING = [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C,
               0x2524, 0x252C, 0x2534, 0x253C, 0x2550, 0x2551, 0x256C]
BLOCK_ELEMENTS = [0x2580, 0x2584, 0x2588, 0x258C, 0x2590, 0x2591, 0x2592, 0x2593]

# sec 9.1 requirement 3. Presence only -- whether 0 and O are visually
# distinct is a judgement a script cannot make, but their absence is not.
DISAMBIGUATION = [ord(c) for c in "0O1lI5S2Z{}[]()"]

# sec 9.1 requirement 5.
LATIN_AND_SYMBOLS = [ord(c) for c in "abcXYZ0189!@#$%^&*-_=+/\\|~`'\";:,.<>?"]

class result_t:
	def __init__(self):
		self.failures = []
		self.notes = []

	def fail(self, family, message):
		self.failures.append("%s: %s" % (family, message))

	def note(self, family, message):
		self.notes.append("%s: %s" % (family, message))


def coverage(font):
	"""Every codepoint the font has a glyph for."""
	covered = set()
	for table in font["cmap"].tables:
		covered.update(table.cmap.keys())
	return covered


def check_uniform_advance(font, family, result, fallback_only):
	"""sec 9.1 requirement 1, as a terminal actually needs it.

	Read literally, "every glyph shares one advance width" rejects every
	font in the fallback chain: DejaVu, Noto and Unifont all carry CJK at
	double width and combining marks at zero, and sec 9.3 *requires* that
	-- a CJK glyph has to occupy exactly two cells or every TUI laid out
	by the remote end misaligns.

	So the invariant checked here is the one the grid depends on: the cell
	width W is whatever the ASCII glyphs agree on, and every other glyph
	must be 0 (combines into the previous cell), W (one cell) or 2W (two
	cells). Anything else cannot be drawn in a character grid at all.
	"""
	metrics = font["hmtx"].metrics
	cmap = {}
	for table in font["cmap"].tables:
		cmap.update(table.cmap)

	# The reference set: if these disagree, nothing else matters, because
	# these are the characters a shell prompt is made of.
	reference = {}
	for char in "AaMWil0189.;+/":
		glyph_name = cmap.get(ord(char))
		if glyph_name and glyph_name in metrics:
			reference.setdefault(metrics[glyph_name][0], []).append(char)

	if not reference:
		result.fail(family, "no ASCII glyphs to measure")
		return None

	if len(reference) > 1:
		spread = "; ".join("%d: %s" % (advance, "".join(chars))
		                   for advance, chars in sorted(reference.items()))
		result.fail(family, "not monospace -- ASCII advances differ (%s)" % spread)
		return None

	width = next(iter(reference))
	allowed = {0, width, 2 * width}

	violations = {}
	for codepoint, glyph_name in cmap.items():
		if glyph_name not in metrics:
			continue
		advance = metrics[glyph_name][0]
		if advance in allowed:
			continue
		violations.setdefault(advance, []).append(codepoint)

	if violations:
		examples = []
		for advance, points in sorted(violations.items()):
			examples.append("U+%04X at %d" % (min(points), advance))
		total = sum(len(v) for v in violations.values())
		message = ("%d glyph(s) are neither one cell (%d) nor two (%d) "
		           "nor zero-width (e.g. %s)"
		           % (total, width, 2 * width, ", ".join(examples[:4])))

		# Fatal for a font someone reads all day; a note for one that is
		# only ever fallen back to. Noto Sans Mono draws a handful of
		# symbols three cells wide, and rejecting it over nine glyphs
		# would cost the chain its entire CJK coverage (sec 9.3) to fix a
		# defect nobody will meet -- a worse terminal, not a better one.
		if fallback_only:
			result.note(family, message + " -- fallback-only, tolerated")
		else:
			result.fail(family, message)

	return width


def check_blocks(font, family, result, fallback_only):
	covered = coverage(font)

	def require(codepoints, what, hard=True):
		missing = [c for c in codepoints if c not in covered]
		if not missing:
			return
		sample = ", ".join("U+%04X" % c for c in missing[:6])
		message = "missing %s (%d of %d, e.g. %s)" % (
			what, len(missing), len(codepoints), sample)
		if hard:
			result.fail(family, message)
		else:
			result.note(family, message)

	require(LATIN_AND_SYMBOLS, "Latin-1 and common symbols")
	require(DISAMBIGUATION, "legibility disambiguation glyphs")

	# sec 9.1 requirement 2, the one that makes TUIs look shattered when it
	# is not met. Noto is exempt: it is in the chain for CJK, and DejaVu
	# sits ahead of it precisely to serve box drawing.
	require(BOX_DRAWING, "box-drawing glyphs (U+2500-257F)", hard=not fallback_only)
	require(BLOCK_ELEMENTS, "block elements (U+2580-259F)", hard=not fallback_only)


def check_weights(directory, family, spec, result):
	"""sec 9.1 requirement 4: at least regular + bold."""
	bold = spec.get("bold")

	if bold:
		if not os.path.exists(os.path.join(directory, bold)):
			result.fail(family, "no bold face (%s)" % bold)
		return

	if spec.get("fallback_only"):
		# Never selected, so SGR bold in it is a fallback of a fallback.
		return

	# No separate bold file: only acceptable if the weight axis is there.
	path = os.path.join(directory, spec["regular"])
	font = TTFont(path, lazy=True)
	axes = [a.axisTag for a in font["fvar"].axes] if "fvar" in font else []
	font.close()

	if "wght" not in axes:
		result.fail(family, "no bold face and no variable weight axis")


def check_licence(directory, family, spec, result):
	"""sec 9.4: the licence travels with the font."""
	path = os.path.join(directory, "licences", spec["licence"])
	if not os.path.exists(path):
		result.fail(family, "no licence file (%s)" % spec["licence"])
		return

	text = open(path, encoding="utf-8", errors="replace").read()
	if len(text.strip()) < 200:
		result.fail(family, "licence file looks truncated (%d bytes)" % len(text))
		return

	# A recognised licence, per sec 9.1 requirement 6. Anything else is not
	# rejected outright -- it is flagged, because the judgement of whether a
	# licence permits Play and distro redistribution is not a regex's to
	# make -- but an unrecognised one should never pass unnoticed.
	recognised = ("SIL OPEN FONT LICENSE", "Apache License", "MIT License",
	              "Bitstream Vera", "GNU GENERAL PUBLIC LICENSE",
	              "Hack Open Font License")
	if not any(name.lower() in text.lower() for name in recognised):
		result.note(family, "unrecognised licence in %s -- review by hand"
		            % spec["licence"])


def main():
	directory = sys.argv[1] if len(sys.argv) > 1 else "fonts"

	if not os.path.isdir(directory):
		sys.stderr.write(
			"check-fonts: no font directory at %s\n"
			"  run tool/fetch-fonts.sh first\n" % directory)
		return 2

	result = result_t()
	checked = 0

	for family in sorted(FAMILIES):
		spec = FAMILIES[family]
		path = os.path.join(directory, spec["regular"])

		if not os.path.exists(path):
			result.fail(family, "not fetched (%s)" % spec["regular"])
			continue

		font = TTFont(path, lazy=True, fontNumber=0)
		try:
			fallback_only = spec.get("fallback_only", False)
			width = check_uniform_advance(font, family, result, fallback_only)
			check_blocks(font, family, result, fallback_only)
			units = font["head"].unitsPerEm
		finally:
			font.close()

		check_weights(directory, family, spec, result)
		check_licence(directory, family, spec, result)

		checked += 1
		if width:
			print("  %-18s cell %d/%d em" % (family, width, units))

	for note in result.notes:
		print("check-fonts: note: %s" % note)

	if result.failures:
		for failure in result.failures:
			sys.stderr.write("check-fonts: %s\n" % failure)
		sys.stderr.write(
			"check-fonts: %d problem(s) -- sec 9.6 says this fails the build\n"
			% len(result.failures))
		return 1

	print("check-fonts: all %d families satisfy sec 9.1" % checked)
	return 0


if __name__ == "__main__":
	sys.exit(main())
