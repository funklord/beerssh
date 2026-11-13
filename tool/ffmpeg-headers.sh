#!/bin/sh
# Name the FFmpeg headers an Android build compiles snurra against, and
# fetch them when nothing on the machine matches the Qt kit's FFmpeg.
#
#   tool/ffmpeg-headers.sh <kit libavformat.so> <given dir> <desktop dir> <cache dir>
#
# Prints the chosen directory on stdout; everything else goes to stderr,
# and any failure exits 1 naming its cause.
#
# The kit carries FFmpeg's libraries without headers, and a header from
# one release against a library from another compiles, links and reads
# the wrong struct field at run time -- so whatever is chosen has the
# kit's libavformat major. In order:
#
#   1. the directory given (FFMPEG_HEADERS), refused if it does not match;
#   2. the desktop's headers, when they match;
#   3. the public headers of the release the kit was built from, out of
#      ffmpeg.org's tarball against a pinned sum, into the cache directory.
#      Nothing of the download runs: the four header directories are
#      extracted and avconfig.h, the one header configure writes, is
#      written here (little-endian, fast unaligned loads, as for arm64
#      and x86_64).
#
# The third is what a machine with the released Qt 6.12.0 kit needs: its
# FFmpeg is 9.0 and Debian's is 7.1. A kit whose major is not in the
# table below is refused by name -- add its release and sum together.

set -u

lib=$1 given=$2 desktop=$3 cache=$4
dirs="libavformat libavcodec libavutil libswresample"

major_of() {
	sed -n 's/^#define LIBAVFORMAT_VERSION_MAJOR *\([0-9]*\).*/\1/p' \
		"$1/libavformat/version_major.h" 2>/dev/null
}

complete() {
	for d in $dirs; do [ -d "$1/$d" ] || return 1; done
}

kit=$(strings "$lib" 2>/dev/null | sed -n 's|^Lavf/\{0,1\}\([0-9][0-9]*\)\..*|\1|p' | head -1)
if [ -z "$kit" ]; then
	echo "android: the Qt kit has no FFmpeg ($lib);" >&2
	echo "android:   it comes with Qt Multimedia -- install that module for this kit" >&2
	exit 1
fi

if [ -n "$given" ]; then
	if ! complete "$given"; then
		echo "android: FFMPEG_HEADERS='$given' lacks one of: $dirs" >&2
		exit 1
	fi
	have=$(major_of "$given")
	if [ "$have" != "$kit" ]; then
		echo "android: FFmpeg headers are libavformat $have, the kit's library $kit" >&2
		echo "android:   point FFMPEG_HEADERS at the kit's release's headers, or leave it unset" >&2
		exit 1
	fi
	echo "$given"
	exit 0
fi

if [ -n "$desktop" ] && complete "$desktop" && [ "$(major_of "$desktop")" = "$kit" ]; then
	echo "$desktop"
	exit 0
fi

case $kit in
	61) version=7.1 sum=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6 ;;
	63) version=9.0 sum=7f607a00dd0d28a729d5a4811205812eef01cf6ef6155025febb6f36a9062d52 ;;
	*)
		echo "android: the kit's FFmpeg is libavformat $kit, and no release is pinned for it" >&2
		echo "android:   add it to tool/ffmpeg-headers.sh, or point FFMPEG_HEADERS at its headers" >&2
		exit 1 ;;
esac

case $cache in
	''|/*|*..*)
		echo "android: the FFmpeg header cache must be a relative path inside the tree, not '$cache'" >&2
		exit 1 ;;
esac
dir=$cache/ffmpeg-$version
if [ -f "$dir/.complete" ]; then
	echo "$dir"
	exit 0
fi

tarball=$cache/ffmpeg-$version.tar.xz
url=https://ffmpeg.org/releases/ffmpeg-$version.tar.xz
echo "android: fetching FFmpeg $version's headers (the kit's libavformat $kit) from $url" >&2
mkdir -p "$cache" || exit 1
rm -f "$tarball"
if ! curl -sSfL -o "$tarball" "$url"; then
	rm -f "$tarball"
	echo "android: could not fetch $url" >&2
	exit 1
fi
if ! echo "$sum  $tarball" | sha256sum -c - >/dev/null 2>&1; then
	rm -f "$tarball"
	echo "android: $url does not match its pinned sha256 $sum" >&2
	exit 1
fi
rm -rf "$dir"
set --
for d in $dirs; do set -- "$@" "ffmpeg-$version/$d/*.h"; done
if ! tar -xJf "$tarball" -C "$cache" --wildcards "$@"; then
	rm -f "$tarball"
	echo "android: could not extract the headers from $tarball" >&2
	exit 1
fi
rm -f "$tarball"
printf '%s\n' '#ifndef AVUTIL_AVCONFIG_H' '#define AVUTIL_AVCONFIG_H' \
	'#define AV_HAVE_BIGENDIAN 0' '#define AV_HAVE_FAST_UNALIGNED 1' '#endif' \
	> "$dir/libavutil/avconfig.h"
if [ "$(major_of "$dir")" != "$kit" ]; then
	echo "android: FFmpeg $version's headers are not libavformat $kit; the table is wrong" >&2
	exit 1
fi
touch "$dir/.complete"
echo "$dir"
