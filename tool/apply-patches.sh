#!/bin/sh
# Apply a patch series to a vendored source tree, idempotently and as a
# SERIES: tool/apply-patches.sh <tree> <patch>...
#
# Checking each patch on its own -- applies forward, or else is already
# in because it applies in reverse -- stops working the moment two
# patches touch the same lines. With 0011 in, 0010's own lines have been
# edited again, so 0010 neither applies nor reverses, and a tree with the
# whole series in it was reported as "partly restored" (2026-10-29).
#
# So this asks which PREFIX of the series the tree is at. The expected
# tree after k patches is built in a scratch directory from the pristine
# source -- the submodule's own commit -- and compared, file by file, with
# every file the series touches. The patches after the matching prefix
# are applied; a tree matching no prefix is refused and left alone, since
# that is a tree somebody has edited or half restored, and a build that
# overwrote it could destroy the edit.
#
# Where the tree is not a git checkout (check-ci and the package builds
# extract it from an archive), its files ARE the pristine source, so they
# are the baseline and the whole series is applied.
#
# Terminates: one pass over the series to build the prefixes, one to
# apply; the scratch directory is removed on exit.
set -eu

[ $# -ge 2 ] || { echo "usage: $0 <tree> <patch>..." >&2; exit 2; }
tree=$1
shift

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT INT TERM

pristine=$scratch/pristine
mkdir "$pristine"
if git -C "$tree" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
   [ "$(git -C "$tree" rev-parse --show-toplevel)" = "$(cd "$tree" && pwd -P)" ]; then
	git -C "$tree" archive HEAD | tar -x -C "$pristine"
else
	cp -R "$tree"/. "$pristine"/
fi

# Every file any patch in the series names.
files=$(sed -n 's|^+++ b/||p; s|^--- a/||p' "$@" | sort -u)

matches() {
	for file in $files; do
		if [ -e "$1/$file" ] || [ -e "$tree/$file" ]; then
			cmp -s "$1/$file" "$tree/$file" || return 1
		fi
	done
	return 0
}

# The longest prefix the tree is at. A later prefix wins, so a patch
# that changes nothing in the files compared cannot hide the ones after.
count=0
at=-1
matches "$pristine" && at=0
for patch in "$@"; do
	(cd "$pristine" && git apply "$patch") || {
		echo "  $patch: DOES NOT APPLY to the pristine source after the" >&2
		echo "  patches before it -- upstream changed under it; rebase it." >&2
		exit 1
	}
	count=$((count + 1))
	matches "$pristine" && at=$count
done

if [ "$at" -lt 0 ]; then
	echo "  $tree matches no prefix of the series: it has been edited or" >&2
	echo "  partly restored. Left as it is. If nothing there is yours:" >&2
	echo "  git -C $tree checkout -- . and build again." >&2
	exit 1
fi

index=0
for patch in "$@"; do
	index=$((index + 1))
	if [ "$index" -le "$at" ]; then
		echo "  $patch: already applied"
		continue
	fi
	git -C "$tree" apply "$patch"
	echo "  $patch: applied"
done
