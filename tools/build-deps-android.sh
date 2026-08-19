#!/bin/sh
# Cross-compile BeerSSH's native dependencies for Android (project.md
# sec 12.1). Produces deps/android/<abi>/{include,lib} -- the layout
# beerssh.pro expects, and the thing it errors out naming when missing.
#
# Usage:
#   tools/build-deps-android.sh [abi]        # default arm64-v8a
#
# Needs ANDROID_NDK_ROOT. The NDK ships a CMake toolchain file and a full
# clang cross-toolchain, so both libraries are built with their own build
# systems pointed at it rather than with hand-written compiler invocations
# -- pinning versions and reusing upstream's build is the whole point of
# scripting this instead of documenting it.
#
# First run 2026-08-19, against NDK 27.2.12479018 and Qt 6.12.0, and it
# worked unchanged -- OpenSSL 3.5.4 and libssh 0.11.5 cross-compiled to
# arm64-v8a static archives, checked as ELF aarch64 rather than host
# objects, which is the failure a cross-build hides best. What it produced
# then failed to link, and that was the Makefile choosing the wrong NDK
# triple rather than anything here.
set -e

abi=${1:-arm64-v8a}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)

# Qt names the ABI directory by QT_ARCH, which matches these strings.
case "$abi" in
	arm64-v8a|armeabi-v7a|x86_64|x86) ;;
	*)
		echo "unknown ABI: $abi" >&2
		echo "expected one of: arm64-v8a armeabi-v7a x86_64 x86" >&2
		exit 1
		;;
esac

if [ -z "$ANDROID_NDK_ROOT" ]; then
	echo "ANDROID_NDK_ROOT is not set." >&2
	echo "  It is the directory containing build/cmake/android.toolchain.cmake" >&2
	exit 1
fi

toolchain="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
if [ ! -f "$toolchain" ]; then
	echo "no toolchain file at $toolchain -- is ANDROID_NDK_ROOT an NDK?" >&2
	exit 1
fi

# API 26 matches project.md sec 2's suggested minSdk. Raising it here
# without raising it there would produce a library the app cannot load on
# the oldest device it claims to support.
api=${BSSH_ANDROID_API:-26}

prefix="$root/deps/android/$abi"
work="$root/build-deps-android/$abi"

# Pinned. An unpinned dependency makes a build that worked yesterday a
# build that cannot be reproduced tomorrow (sec 12.4).
openssl_version=3.5.4
libssh_version=0.11.5

# And checksummed, which the version pin alone does not do: a pinned
# version says WHICH artifact was wanted, not that the bytes that arrived
# are the ones upstream published. tools/fetch-fonts.sh has verified its
# downloads since it was written; this script fetches the TLS and SSH
# stack and verified nothing, so the cosmetic downloads were checked and
# the cryptography was not.
#
# Where these came from, because a hash nobody can retrace is a hash
# nobody can update:
#
#   OpenSSL  publishes openssl-<version>.tar.gz.sha256 beside the tarball.
#            The download matched it.
#   libssh   publishes a detached GPG signature instead of a sum. Its
#            signing key 88A228D89B07C2C77D0C780903D5DF8CFDD3E8E7 is named
#            on libssh.org/get-it/ -- which is what stops the check being
#            circular, the signature naming its own key otherwise. The key
#            with that fingerprint verifies the .asc as "libssh release
#            key", and this is the sha256 of the tarball it covers.
openssl_sha256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
libssh_sha256=6898ba9dd836d618b71dc7a4bb786a502c173cef5cafbf20fe5e0567ba4ea30c

# sha256sum on Linux, shasum -a 256 on the BSDs and macOS -- the same split
# fetch-fonts.sh handles, for the same reason.
if command -v sha256sum >/dev/null 2>&1; then
	checksum() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	checksum() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	echo "build-deps-android: no sha256 tool found" >&2
	exit 1
fi

# Deletes the archive it rejects. The downloads above are skipped when the
# file is already there, so a mismatched archive left on disk is one every
# later run reuses without fetching -- a check that fails once and is then
# stepped around forever.
verify() {
	got=$(checksum "$1")
	[ "$got" = "$2" ] && return 0
	echo "build-deps-android: $1 does not match its pinned sha256." >&2
	echo "  expected $2" >&2
	echo "  got      $got" >&2
	echo "  Removing it. If upstream reissued the archive, change the pin in" >&2
	echo "  this script deliberately rather than to whatever turned up." >&2
	rm -f "$1"
	exit 1
}

mkdir -p "$prefix" "$work"
cd "$work"

# ---- OpenSSL --------------------------------------------------------------
# libssh needs a crypto backend, and OpenSSL is the one sec 4 defaults to
# as best-tested with libssh.
if [ ! -f "$prefix/lib/libcrypto.a" ]; then
	echo "==> OpenSSL $openssl_version for $abi"

	[ -f "openssl-$openssl_version.tar.gz" ] ||
		curl -fsSLO "https://www.openssl.org/source/openssl-$openssl_version.tar.gz"
	verify "openssl-$openssl_version.tar.gz" "$openssl_sha256"
	rm -rf "openssl-$openssl_version"
	tar xf "openssl-$openssl_version.tar.gz"
	cd "openssl-$openssl_version"

	case "$abi" in
		arm64-v8a)   target=android-arm64 ;;
		armeabi-v7a) target=android-arm ;;
		x86_64)      target=android-x86_64 ;;
		x86)         target=android-x86 ;;
	esac

	PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH" \
		./Configure "$target" -D__ANDROID_API__="$api" no-shared no-tests \
			--prefix="$prefix" --openssldir="$prefix"
	PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH" \
		make -j"$(nproc)" build_libs
	make install_dev
	cd "$work"
fi

# ---- libssh ---------------------------------------------------------------
# Static, which sidesteps sec 4's LGPL relinking question only if the app
# also ships the object files needed to relink -- see sec 4 before shipping
# an AAB built this way.
if [ ! -f "$prefix/lib/libssh.a" ]; then
	echo "==> libssh $libssh_version for $abi"

	[ -f "libssh-$libssh_version.tar.xz" ] ||
		curl -fsSLO "https://www.libssh.org/files/0.11/libssh-$libssh_version.tar.xz"
	verify "libssh-$libssh_version.tar.xz" "$libssh_sha256"
	rm -rf "libssh-$libssh_version"
	tar xf "libssh-$libssh_version.tar.xz"

	# CMAKE_FIND_ROOT_PATH is not redundant with OPENSSL_ROOT_DIR, and
	# leaving it out is how the first attempt at this failed. The NDK
	# toolchain sets CMAKE_FIND_ROOT_PATH_MODE_{INCLUDE,LIBRARY,PACKAGE}
	# to ONLY, so find_package searches the sysroot and NOTHING ELSE --
	# a prefix outside it is silently ignored however it is named, and
	# the error blames a missing OpenSSL that is sitting right there.
	# S_IWRITE is a legacy alias for S_IWUSR that bionic does not define,
	# and libssh's misc.c uses it unguarded -- libssh does not target
	# Android, so this is not a version that can be waited out (checked:
	# 0.11.5 has it too). Supplied as a define rather than a patch to
	# their source, so it survives the next version bump.
	rm -rf libssh-build
	cmake -S "libssh-$libssh_version" -B libssh-build \
		-DCMAKE_C_FLAGS="-DS_IWRITE=S_IWUSR" \
		-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
		-DANDROID_ABI="$abi" \
		-DANDROID_PLATFORM="android-$api" \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		-DOPENSSL_ROOT_DIR="$prefix" \
		-DOPENSSL_USE_STATIC_LIBS=ON \
		-DCMAKE_PREFIX_PATH="$prefix" \
		-DCMAKE_FIND_ROOT_PATH="$prefix" \
		-DBUILD_SHARED_LIBS=OFF \
		-DWITH_EXAMPLES=OFF \
		-DWITH_SERVER=OFF \
		-DUNIT_TESTING=OFF \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build libssh-build -j "$(nproc)"
	cmake --install libssh-build
fi

echo
echo "done: $prefix"
echo "  beerssh.pro picks these up automatically for this ABI."
echo
echo "NOT built here: libsixel (inline images). The Android build sets"
echo "BSSH_NO_SIXEL and declines sixel rather than half-decoding it."
