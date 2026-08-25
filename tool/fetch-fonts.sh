#!/bin/sh
# =============================================================================
# tool/fetch-fonts.sh -- fetch the curated terminal font set (project.md sec 9)
#
# The fonts are NOT in the repository, and that is deliberate. They are
# tens of megabytes of binary that never diff usefully, they are already
# published and versioned by their upstreams, and this project has already
# paid once for treating git as a place to keep large binaries. Pinning a
# version and a SHA-256 gives the same reproducibility a committed copy
# would, without the history.
#
# Every archive is pinned by URL *and* checksum: a release asset that
# changes under its own tag is a supply-chain problem, not a curiosity, and
# the whole point of sec 9.1 is that only vetted files end up in the grid.
#
# What lands in fonts/ is what sec 9.2 selected, in the weights sec 9.1
# requires (regular + bold, italic where upstream ships one), plus each
# font's licence text -- the OFL and the Apache licence both require it to
# travel with the font, and sec 9.4 wants an Open Source Licences screen
# built from files rather than from memory.
#
# Usage:
#   tool/fetch-fonts.sh          fetch anything missing
#   tool/fetch-fonts.sh --force  re-fetch everything
#   tool/fetch-fonts.sh --list   show what would be fetched
# =============================================================================

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/fonts"
cache="$out/.cache"
licences="$out/licences"

force=0
list=0
for arg in "$@"; do
	case "$arg" in
		--force) force=1 ;;
		--list) list=1 ;;
		*) echo "fetch-fonts: unknown option $arg" >&2; exit 2 ;;
	esac
done

# -----------------------------------------------------------------------------
# The set. One record per family:
#
#   id | archive-url | sha256 | licence-url | licence-file | members...
#
# where each member is  path/inside/archive>OutputName.ttf , or for a bare
# font file (no archive) the single member  .>OutputName.ttf .
#
# The output names are the ones bssh_font_catalog expects; changing one
# here without changing it there means the font is fetched and then not
# found, which looks exactly like a font that failed to download.
# -----------------------------------------------------------------------------
records() {
	cat <<'EOF'
jetbrains-mono|https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip|6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf|archive:OFL.txt|OFL-JetBrainsMono.txt|fonts/ttf/JetBrainsMono-Regular.ttf>JetBrainsMono-Regular.ttf fonts/ttf/JetBrainsMono-Bold.ttf>JetBrainsMono-Bold.ttf fonts/ttf/JetBrainsMono-Italic.ttf>JetBrainsMono-Italic.ttf fonts/ttf/JetBrainsMono-BoldItalic.ttf>JetBrainsMono-BoldItalic.ttf
iosevka-term|https://github.com/be5invis/Iosevka/releases/download/v34.8.0/PkgTTF-IosevkaTerm-34.8.0.zip|bb58292f7f7172d272fb8d4475237469e4e047d5cdebb795ecc80db8b54e615e|https://raw.githubusercontent.com/be5invis/Iosevka/v34.8.0/LICENSE.md|OFL-Iosevka.md|IosevkaTerm-Regular.ttf>IosevkaTerm-Regular.ttf IosevkaTerm-Bold.ttf>IosevkaTerm-Bold.ttf IosevkaTerm-Italic.ttf>IosevkaTerm-Italic.ttf IosevkaTerm-BoldItalic.ttf>IosevkaTerm-BoldItalic.ttf
dejavu-sans-mono|https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip|7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a|archive:dejavu-fonts-ttf-2.37/LICENSE|LICENSE-DejaVu.txt|dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono.ttf>DejaVuSansMono.ttf dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono-Bold.ttf>DejaVuSansMono-Bold.ttf dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono-Oblique.ttf>DejaVuSansMono-Oblique.ttf dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono-BoldOblique.ttf>DejaVuSansMono-BoldOblique.ttf
hack|https://github.com/source-foundry/Hack/releases/download/v3.003/Hack-v3.003-ttf.zip|0c2604631b1f055041c68a0e09ae4801acab6c5072ba2db6a822f53c3f8290ac|https://raw.githubusercontent.com/source-foundry/Hack/v3.003/LICENSE.md|LICENSE-Hack.md|ttf/Hack-Regular.ttf>Hack-Regular.ttf ttf/Hack-Bold.ttf>Hack-Bold.ttf ttf/Hack-Italic.ttf>Hack-Italic.ttf ttf/Hack-BoldItalic.ttf>Hack-BoldItalic.ttf
fira-code|https://github.com/tonsky/FiraCode/releases/download/6.2/Fira_Code_v6.2.zip|0949915ba8eb24d89fd93d10a7ff623f42830d7c5ffc3ecbf960e4ecad3e3e79|https://raw.githubusercontent.com/tonsky/FiraCode/6.2/LICENSE|OFL-FiraCode.txt|ttf/FiraCode-Regular.ttf>FiraCode-Regular.ttf ttf/FiraCode-Bold.ttf>FiraCode-Bold.ttf
cascadia-code-pl|https://github.com/microsoft/cascadia-code/releases/download/v2404.23/CascadiaCode-2404.23.zip|a911410626c0e09d03fa3fdda827188fda96607df50fecc3c5fee5906e33251b|https://raw.githubusercontent.com/microsoft/cascadia-code/v2404.23/LICENSE|OFL-CascadiaCode.txt|ttf/CascadiaCodePL.ttf>CascadiaCodePL.ttf ttf/CascadiaCodePLItalic.ttf>CascadiaCodePL-Italic.ttf
noto-sans-mono|https://github.com/notofonts/notofonts.github.io/raw/noto-monthly-release-2026.08.01/fonts/NotoSansMono/hinted/ttf/NotoSansMono-Regular.ttf|65b5e2b2c4a1fba9ae8be1f026cb35b03dcb8886d9b2a4147054fde12f7e767d|https://raw.githubusercontent.com/notofonts/notofonts.github.io/noto-monthly-release-2026.08.01/fonts/LICENSE|OFL-NotoSansMono.txt|.>NotoSansMono-Regular.ttf
unifont|https://unifoundry.com/pub/unifont/unifont-15.1.05/font-builds/unifont-15.1.05.otf|7b62b50acbb186689dc30c446ce4367b87d79489e9907b83255f9fbe0dcfb9e1|https://unifoundry.com/LICENSE.txt|LICENSE-Unifont.txt|.>unifont.otf
EOF
}

if [ "$list" = 1 ]; then
	records() { :; }
	echo "curated set (project.md sec 9.2):"
	cat <<'EOF' | sed 's/^/  /'
jetbrains-mono     JetBrains Mono 2.304        OFL-1.1
iosevka-term       Iosevka Term 34.8.0         OFL-1.1
dejavu-sans-mono   DejaVu Sans Mono 2.37       Bitstream Vera / DejaVu
hack               Hack 3.003                  Hack Open Font License
fira-code          Fira Code 6.2               OFL-1.1
cascadia-code-pl   Cascadia Code PL 2404.23    OFL-1.1
noto-sans-mono     Noto Sans Mono 2026.08.01   OFL-1.1  (fallback only)
unifont            Unifont 15.1.05             OFL-1.1  (fallback only)
EOF
	exit 0
fi

for tool in curl unzip; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "fetch-fonts: $tool is required" >&2
		exit 1
	}
done

# sha256sum on Linux, shasum -a 256 on the BSDs and macOS.
if command -v sha256sum >/dev/null 2>&1; then
	checksum() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	checksum() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	echo "fetch-fonts: no sha256 tool found" >&2
	exit 1
fi

mkdir -p "$out" "$cache" "$licences"

fetched=0
skipped=0

records | while IFS='|' read -r id url sha licence_url licence_file members; do
	[ -n "$id" ] || continue

	# Everything this record produces, so a complete set is not re-fetched.
	complete=1
	for member in $members; do
		target="$out/${member##*>}"
		[ -f "$target" ] || complete=0
	done
	[ -f "$licences/$licence_file" ] || complete=0

	if [ "$complete" = 1 ] && [ "$force" = 0 ]; then
		skipped=$((skipped + 1))
		echo "  $id: already present"
		continue
	fi

	archive="$cache/$(basename "$url")"

	if [ ! -f "$archive" ] || [ "$force" = 1 ]; then
		echo "  $id: downloading $(basename "$url")"
		curl -fsSL --retry 3 -o "$archive.part" "$url"
		mv "$archive.part" "$archive"
	fi

	got=$(checksum "$archive")
	if [ "$got" != "$sha" ]; then
		# Not a warning. A release asset whose bytes changed under a
		# fixed tag is either an upstream mistake or something worse,
		# and either way it must not be silently unpacked into an app.
		echo "fetch-fonts: CHECKSUM MISMATCH for $id" >&2
		echo "  expected $sha" >&2
		echo "  got      $got" >&2
		echo "  file     $archive" >&2
		exit 1
	fi

	case "$url" in
		*.zip)
			for member in $members; do
				src="${member%%>*}"
				dst="${member##*>}"
				if ! unzip -p "$archive" "$src" > "$out/$dst.part" 2>/dev/null ||
				   [ ! -s "$out/$dst.part" ]; then
					rm -f "$out/$dst.part"
					echo "fetch-fonts: $id: $src not in the archive" >&2
					exit 1
				fi
				mv "$out/$dst.part" "$out/$dst"
			done
			;;
		*)
			# A bare font file: the archive IS the font.
			dst="${members##*>}"
			cp "$archive" "$out/$dst"
			;;
	esac

	case "$licence_url" in
		archive:*)
			unzip -p "$archive" "${licence_url#archive:}" > "$licences/$licence_file"
			;;
		*)
			curl -fsSL --retry 3 -o "$licences/$licence_file" "$licence_url"
			;;
	esac

	fetched=$((fetched + 1))
	echo "  $id: fetched"
done

echo "fonts in $out"
echo "run tool/check-fonts.py to verify them against sec 9.1"
