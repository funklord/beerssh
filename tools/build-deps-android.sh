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
# NOT YET RUN. This was written without an NDK on the machine, like the
# manifest and the CI Android job. Treat its first execution as part of the
# work rather than as a regression, and expect to fix it.
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

mkdir -p "$prefix" "$work"
cd "$work"

# ---- OpenSSL --------------------------------------------------------------
# libssh needs a crypto backend, and OpenSSL is the one sec 4 defaults to
# as best-tested with libssh.
if [ ! -f "$prefix/lib/libcrypto.a" ]; then
	echo "==> OpenSSL $openssl_version for $abi"

	[ -f "openssl-$openssl_version.tar.gz" ] ||
		curl -fsSLO "https://www.openssl.org/source/openssl-$openssl_version.tar.gz"
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
