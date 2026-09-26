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
# The set, from tool/font-sources.txt -- one file the app reads as well, so
# that a phone fetching one font and a build fetching the whole set cannot
# disagree about what the set is. Its header gives the format.
#
# The output names are the ones bssh_font_catalog expects; changing one
# there without changing it in the catalog means the font is fetched and
# then not found, which looks exactly like a font that failed to download.
# -----------------------------------------------------------------------------
sources="$root/tool/font-sources.txt"
records() {
	sed -n 's/^archive|//p' "$sources"
}

# The pin for one output on its own, which the phone checks it against.
file_pin() {
	sed -n "s/^file|$(printf '%s' "$1" | sed 's/[.[\*^$/]/\\&/g')|//p" "$sources"
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

# Each output against its own pin, after the archive hash has vouched for
# the archive. A mismatch here means font-sources.txt's file lines no longer
# describe this archive -- and a phone would then refuse fonts a build
# accepts, which is the drift one file was meant to prevent.
check_pin() {
	pin=$(file_pin "$2")
	if [ -z "$pin" ]; then
		echo "fetch-fonts: $1: no file| pin for $2 in $sources" >&2
		exit 1
	fi
	got=$(checksum "$3")
	if [ "$got" != "$pin" ]; then
		echo "fetch-fonts: $1: $2 does not match its file| pin" >&2
		echo "  expected $pin" >&2
		echo "  got      $got" >&2
		exit 1
	fi
}

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
				check_pin "$id" "$dst" "$out/$dst"
			done
			;;
		*)
			# A bare font file: the archive IS the font.
			dst="${members##*>}"
			cp "$archive" "$out/$dst"
			check_pin "$id" "$dst" "$out/$dst"
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

	check_pin "$id" "licences/$licence_file" "$licences/$licence_file"

	fetched=$((fetched + 1))
	echo "  $id: fetched"
done

echo "fonts in $out"
echo "run tool/check-fonts.py to verify them against sec 9.1"
