# =============================================================================
# Top-level Makefile -- BeerSSH
#
# PURPOSE
#   The single entry point for the whole tree. The Qt app itself is built
#   by qmake (beerssh.pro) because moc/uic/rcc do not fit hand-written
#   pattern rules; everything else in this repo is plain Makefiles. This
#   file is the wrapper that makes those two look like one interface --
#   the same split fuzzypickles uses for its gui/ subtree.
#
# TARGETS
#   make              -- build the desktop app (optimized, no sanitizers)
#                        and beersshd, the SFTP-only server (sec 19)
#   make DEBUG=1      -- unoptimized, symbol-rich build. Checked for being
#                        SET, not for a value -- see BUILD FLAGS.
#   make SANITIZE=1   -- add ASan+UBSan, independent of DEBUG
#   make run          -- build and run it
#   make test         -- build and run the smoke test + unit tests.
#                        A crashing test does NOT fork gdb; ask for a
#                        backtrace with BSSH_TEST_STACK_DUMP=1.
#                        A test that never returns is stopped by the
#                        suite itself -- BSSH_TEST_TIMEOUT (seconds per
#                        class, default 300) and BSSH_TEST_MAX_OUTPUT
#                        (MB, default 256), either off with 0. The
#                        bound is in test/main.cpp, not here, because
#                        the run that filled a disk was the binary
#                        invoked directly.
#   make bench        -- emulator throughput in MB/s (sec 9.5); built by
#                        `test` but not run by it, and not a CI gate.
#                        Refuses a machine too loaded to measure;
#                        BSSH_BENCH_ANYWAY=1 to insist
#   make shots        -- render every screen to build-shots/ for review;
#                        not a test, and it cannot fail a build (sec 12.3)
#   make check-deb    -- build the .debs, then install and run them in a
#                        clean container (needs docker; sec 12.2)
#   make style        -- gate the tree's indentation (tool/check-indent.sh)
#   make check        -- style and test, the pair to run before any commit
#   make check-ci     -- run the Linux CI job in the runner's own image
#                        (needs docker; sec 12.4). By hand only: GitHub
#                        runs the real job on every push
#   make check-startup -- start the packaged binary as a user would, on
#                        Xvfb, and see it come up
#   make check-x11-drag -- a pane dropped on another BeerSSH is refused on
#                        X11 (needs Xvfb, xdotool; sec 8.4)
#   make sabotage     -- break the code on purpose and see each named test
#                        fail (tool/sabotage.py). Every spec by default,
#                        which is hours; SPECS=tool/sabotage/x.toml for some
#   make check-ssh    -- the SSH transport against a throwaway sshd
#   make check-handoff -- can a live session move between processes
#                        (needs sshd; sec 12.3)
#   make check-keepalive -- dead-peer detection against a black-holed
#                        link (needs docker + NET_ADMIN; sec 5.3)
#   make fonts        -- fetch the curated font set (sec 9.2) into fonts/;
#                        not needed to build, see FONTS below
#   make fonts-list   -- show the set and its pinned versions
#   make check-fonts  -- validate the fetched fonts against sec 9.1
#   make check-java   -- compile the app's Java (sec 5.2) against the SDK
#                        and the Qt kit; nothing else in the tree does.
#                        Refuses rather than skipping without them
#   make android      -- the installable APK, named for its version and ABI
#                        and checked for both; debug-signed unless
#                        ANDROID_KEYSTORE is set (sec 12.2). Needs a Qt kit,
#                        NDK, SDK and a JDK; see ANDROID below
#   make check-apk    -- install that APK on an attached device and check it
#                        starts (needs adb; sec 12.2)
#   make android-aab  -- the Android App Bundle Play takes, named and with
#                        its versionCode reported; needs ANDROID_KEYSTORE
#   make android-check -- the Android preflight: kit, NDK, SDK, JDK
#   make android-play-check -- read the bundle as Play would: signer,
#                        targetSdk, versionCode, 16 KB alignment
#   make android-screenshots -- the store listing's pictures from this
#                        build, on an attached device or emulator
#   make android-store-screenshots -- the same, on the shared `store`
#                        emulator it starts and stops (tool/android.mk),
#                        so the pictures do not depend on whose phone is in
#   make android-run  -- install and launch on the attached device
#   make install      -- install the app, its desktop entry, icon,
#                        AppStream metadata and bundled fonts
#   make uninstall    -- remove what install put there
#   make deb          -- build beerssh and beerssh-fonts Debian packages
#   make clean        -- remove compilation intermediates only; the built
#                        binary is left in place, so `make install` stays
#                        possible without a rebuild
#   make deb-clean    -- the desktop build dirs only; what `make deb`
#                        cleans with, so the Android build survives it
#   make veryclean    -- clean, plus the built binary and the build dirs
#   make distclean    -- veryclean, plus stray editor files
#
# BUILD FLAGS
#   DEBUG and SANITIZE are never given a `?=` default: every check is
#   `ifdef` (set vs unset), so a `?= 0` default would make them
#   permanently "set" and impossible to turn off.
#
# TOOLCHAIN INJECTION
#   Every other variable is `?=`, so the command line and the environment
#   always win over the defaults here:
#     make CXX=clang++ DEBUG=1
#     make QMAKE=/opt/qt6/bin/qmake
#
# ANDROID
#   `make android` needs a Qt-for-Android installation and an NDK, neither
#   of which the desktop build requires. Point QT_ANDROID_ROOT at the Qt
#   kit (the one containing bin/qmake for e.g. android_arm64_v8a) and have
#   ANDROID_NDK_ROOT/ANDROID_SDK_ROOT set as Qt's tooling expects:
#     make android QT_ANDROID_ROOT=$HOME/Qt/6.11.1/android_arm64_v8a
#
#   Four things are needed and each is checked for by name, because every
#   one of them fails unhelpfully when missing:
#     QT_ANDROID_ROOT   the Qt kit for the target ABI
#     ANDROID_NDK_ROOT  the NDK (compiles); tool/build-deps-android.sh too
#     ANDROID_SDK_ROOT  the SDK (packages); separate from the NDK
#     a JDK on PATH     Gradle will not run on a JRE
#
#   The kit decides the ABI -- qmake reads it from there and so does this
#   Makefile, so there is one answer rather than two that can disagree.
#
#   The dependencies must be cross-compiled first, for that same ABI:
#     tool/build-deps-android.sh arm64-v8a
#
# FONTS
#   The curated set (sec 9.2) is fetched, not committed: `make fonts` pulls
#   each family from a pinned release and verifies its SHA-256. The app
#   builds and runs without it, falling back to whatever monospace families
#   the system already has, so this is never a build dependency.
#
#   `make check-fonts` is sec 9.6's gate -- uniform advance width, box
#   drawing, weights, licence -- and needs fontTools (pip install fonttools).
# =============================================================================

CC    ?= gcc
CXX   ?= g++
QMAKE ?= qmake6

TARGET = beerssh

# beersshd, the SFTP-only server (sec 19), named HERE rather than beside
# its rules below: `all: $(ARTIFACTS)` expands its prerequisites as make
# reads that line, and with the name defined after it `all` never built
# beersshd at all -- `make -pn all` listed beerssh, its manual page and
# its completion, and nothing else (found 2026-10-23).
SSHD_TARGET = beersshd
SSHD_MAN_PAGE = $(SSHD_TARGET).1

# The app id from sec 0.1. Every packaging file is named after it, and a
# mismatch shows up as an app that installs, runs, and cannot be found.
APP_ID = se.vibes.beerssh

# One version, passed into the build rather than repeated. It was written
# in main.cpp and again in beerssh.pro for the Android package name, which
# is two places to forget; a Debian package would have made it three, and
# a package claiming a version the app denies is a support question nobody
# can answer.
# The one place the version is stated. debian/changelog and any other
# declaration are checked against it by `make version-check`.
VERSION ?= $(shell cat VERSION)

BUILD_DIR         ?= build

# Where the finished packages land. `dpkg-buildpackage` writes to the PARENT
# directory, so they have to be moved somewhere regardless; this names it.
#
# A `deb/` subdirectory of the build tree rather than the build tree itself,
# which is what five of the ten packaging projects already did and is now the
# settled answer. Two reasons beyond consistency: it keeps a set of artifacts
# separable from object files, so `ls` and `clean` can both speak about them
# by name; and it is the only spelling that still works when BUILD_DIR is `.`,
# as openmlx4 sets it, where the build tree and the source tree are one.
DEB_DIR ?= $(BUILD_DIR)/deb

# ABI-suffixed, the settled spelling (harmonization.md): the same `?=`
# line every Android sibling carries, because the fragment deliberately
# defaults nothing. The plain `build-android` this used to say meant an
# emulator build and a device build clobbered each other in silence.
# ANDROID_ABI acquires its value in tool/android.mk, included below `all`;
# this expands late, so stating it here is safe.
ANDROID_BUILD_DIR ?= build-android-$(ANDROID_ABI)

PREFIX  ?= /usr/local
DESTDIR ?=

# Section 1, because this is a user command. Generated -- see the rule
# under `all`, and project.md sec 8.6.
MAN_PAGE = $(TARGET).1

# The bash completion, generated by the same binary from the same option
# table (project.md sec 8.6). Named for the command rather than with a
# .bash suffix, because that is the filename bash-completion loads on
# demand: it looks for a file called exactly the command being completed.
COMPLETION = $(TARGET).bash

# softkeys/ is the on-screen keyboard, vendored (project.md sec 6.2), so
# it is named here too. These are the qmake build's PREREQUISITES rather
# than its inputs -- miss a directory and editing a file in it quietly
# fails to trigger a rebuild, which is a stale binary and not an error.
# Sabotage edits softkeys/src/ in place, so it matters here as much as
# for src/.
SOURCES = $(wildcard src/*.cpp src/*/*.cpp softkeys/src/*.cpp)
HEADERS = $(wildcard src/*.h src/*/*.h softkeys/include/softkeys/*.h)

# --- vendored libvterm ------------------------------------------------------
# Built here, by hand, rather than through its own autotools: this repo's
# convention is that everything except the Qt app is a plain Makefile, and
# compiling nine C files needs no more than that. It also means the Android
# build gets the same rule with a cross-compiler in CC, instead of a second
# build system to make work under the NDK.
#
# The submodule is upstream, untouched, and stays that way -- vendored code
# keeps its own style and its own history (code-style.md).
VTERM_DIR  = vterm

# A missing submodule is fetched rather than complained about, and this
# has to happen while the Makefile is being READ.
#
# VTERM_SRCS below is a wildcard, and a wildcard is expanded at parse
# time. A checkout done from a recipe would therefore come too late for
# the run that needed it: the source list would still be empty, and make
# would go on to build an archive with no members in it. So the fetch
# is a parse-time $(shell), placed above the wildcard that depends on it.
#
# An ordinary build pays nothing for this. The $(wildcard) test below
# spawns no process, so the shell runs only in a tree that actually lacks
# the submodule -- once, and then never again.
#
# Targets that cannot need libvterm are excluded, because a style check
# has no business reaching the network. Goals not on this list are
# assumed to need it, which is the safe direction: the cost of being
# wrong is one unnecessary fetch, against a build that fails.
BSSH_VTERM_FREE_GOALS = help style hooks clean deb-clean veryclean version-check

ifneq ($(filter-out $(BSSH_VTERM_FREE_GOALS),$(or $(MAKECMDGOALS),all)),)
ifeq ($(wildcard $(VTERM_DIR)/src/vterm.c),)
BSSH_VTERM_FETCH := $(shell \
	if [ ! -e .git ]; then \
		echo "make: $(VTERM_DIR)/ is empty and this is not a git checkout," >&2; \
		echo "make:   so there is nothing to fetch it from." >&2; \
	elif ! command -v git >/dev/null 2>&1; then \
		echo "make: $(VTERM_DIR)/ is empty and git is not installed." >&2; \
	else \
		echo "make: fetching the $(VTERM_DIR) submodule" >&2; \
		git submodule update --init --recursive $(VTERM_DIR) >&2; \
	fi)
endif
endif

# softkeys the same way and for the same reason, and then REFUSED if it
# is still missing: qmake reads softkeys.pri as it generates the build,
# and a build that went on without it would fail later naming a header
# rather than the submodule that was never fetched.
SOFTKEYS_DIR = softkeys

ifneq ($(filter-out $(BSSH_VTERM_FREE_GOALS),$(or $(MAKECMDGOALS),all)),)
ifeq ($(wildcard $(SOFTKEYS_DIR)/softkeys.pri),)
BSSH_SOFTKEYS_FETCH := $(shell \
	if [ ! -e .git ]; then \
		echo "make: $(SOFTKEYS_DIR)/ is empty and this is not a git checkout," >&2; \
		echo "make:   so there is nothing to fetch it from." >&2; \
	elif ! command -v git >/dev/null 2>&1; then \
		echo "make: $(SOFTKEYS_DIR)/ is empty and git is not installed." >&2; \
	else \
		echo "make: fetching the $(SOFTKEYS_DIR) submodule" >&2; \
		git submodule update --init $(SOFTKEYS_DIR) >&2; \
	fi)
ifeq ($(wildcard $(SOFTKEYS_DIR)/softkeys.pri),)
$(error the $(SOFTKEYS_DIR) submodule is missing and could not be fetched)
endif
endif
endif

VTERM_SRCS = $(wildcard $(VTERM_DIR)/src/*.c)
VTERM_OBJS = $(patsubst $(VTERM_DIR)/src/%.c,$(VTERM_BUILD_DIR)/%.o,$(VTERM_SRCS))
VTERM_BUILD_DIR = build-vterm
VTERM_LIB  = $(VTERM_BUILD_DIR)/libvterm.a

# Patches applied to the vendored copy at build time (vterm-patch/), kept
# as files rather than as edits committed into the submodule: a submodule
# pinned to a commit that exists only locally is a submodule nobody else can
# clone. Applying them here means an upstream update that conflicts fails
# loudly at the patch step instead of silently dropping the fix.
VTERM_PATCHES = $(sort $(wildcard vterm-patch/*.patch))

# One stamp per build tree, not one for both. There used to be a single
# stamp here gating the host AND the Android objects, which put the
# Android build's correctness in the host tree's hands: removing
# build-vterm/ re-patched nothing the Android objects could see, and the
# Android archive was gated on a file that said nothing about it.
VTERM_STAMP = $(VTERM_BUILD_DIR)/.patched

# -w: this is third-party code built to someone else's warning standard, and
# a wall of warnings nobody will act on is a wall that hides ours.
VTERM_CFLAGS = -std=c99 -Os -w -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src

# --- vendored libvterm, cross-compiled ------------------------------------
# The host archive cannot be reused: it is x86_64, and the first Android
# link said so in nine identical lines. One build per ABI, in its own
# directory, because "the library" stops being one artifact the moment
# there is more than one target.
# The ABI, the API levels, the versionCode, the SDK and NDK paths, the adb
# location and the signature check all come from tool/android.mk, included
# below `all`. They used to be stated here, and the fragment was written from
# this file -- so what is gone is this project's own text, moved to where the
# other three Android projects read the same words.

NDK_BIN = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin
VTERM_ANDROID_BASE  = build-vterm-android
VTERM_ANDROID_DIR   = $(VTERM_ANDROID_BASE)/$(ANDROID_ABI)
VTERM_ANDROID_LIB   = $(VTERM_ANDROID_DIR)/libvterm.a
VTERM_ANDROID_OBJS  = $(patsubst $(VTERM_DIR)/src/%.c,$(VTERM_ANDROID_DIR)/%.o,$(VTERM_SRCS))
VTERM_ANDROID_STAMP = $(VTERM_ANDROID_DIR)/.patched

# The stamps say the sources WERE patched, and nothing more. `git -C
# vterm checkout -- .` or `git submodule update` rewrites the sources
# while every stamp survives, and the next build then compiles each
# object from UNPATCHED source without a word -- no CSI-leader fallback,
# no DEC-mode fallbacks, no deferred-damage flush. Measured in this tree
# by restoring exactly that way with the archive already built.
#
# So the stamps are witnessed at parse time, before any rule is
# considered: one grep per patched file for a token only the patches put
# there (XTQMODKEYS arrives in state.c with the same edit whose
# four-wide leader check is the `0xfc` mask in the compiled object;
# "pre-scroll rectangle" is 0003's comment in screen.c,
# pushline_without_cells is 0007's declaration in vterm.h, and 0008's
# export puts vterm_export_state in vterm.c, vterm_encoding_index in
# encoding.c and VTermWriter in vterm_internal.h; 0010 puts sb_popline4
# in vterm.h, REFLOW_PULL_MAX in screen.c and fields.at_phantom in
# state.c; 0011's SGR 2 is "Faint on" in pen.c; 0012 adds reflow_take
# to screen.c, and 0013's comment there says a glyph has "no second"
# cell). Per file rather
# than one marker for both, so a partial restore cannot hide behind the
# file that kept its patch. A marker missing while any stamp exists
# means the sources were restored out of band: every stamp goes -- all
# ABIs, since all are equally stale -- and the stamp rules re-patch
# before anything compiles. Three processes, spawned only when a stamp
# exists to check; a clean tree pays one `ls`.
BSSH_VTERM_WITNESS := $(shell \
	stamps=$$(ls $(VTERM_BUILD_DIR)/.patched \
	           $(VTERM_ANDROID_BASE)/*/.patched 2>/dev/null); \
	if [ -n "$$stamps" ] \
	   && ! { grep -q XTQMODKEYS $(VTERM_DIR)/src/state.c 2>/dev/null \
	          && grep -q 'pre-scroll rectangle' $(VTERM_DIR)/src/screen.c \
	             2>/dev/null \
	          && grep -q pushline_without_cells $(VTERM_DIR)/include/vterm.h \
	             2>/dev/null \
	          && grep -q pushline_with_cells $(VTERM_DIR)/include/vterm.h \
	             2>/dev/null \
	          && grep -q sb_popline4 $(VTERM_DIR)/include/vterm.h 2>/dev/null \
	          && grep -q REFLOW_PULL_MAX $(VTERM_DIR)/src/screen.c 2>/dev/null \
	          && grep -q fields.at_phantom $(VTERM_DIR)/src/state.c 2>/dev/null \
	          && grep -q 'Faint on' $(VTERM_DIR)/src/pen.c 2>/dev/null \
	          && grep -q reflow_take $(VTERM_DIR)/src/screen.c 2>/dev/null \
	          && grep -q 'no second' $(VTERM_DIR)/src/screen.c 2>/dev/null \
	          && grep -q vterm_export_state $(VTERM_DIR)/src/vterm.c 2>/dev/null \
	          && grep -q vterm_encoding_index $(VTERM_DIR)/src/encoding.c 2>/dev/null \
	          && grep -q VTermWriter $(VTERM_DIR)/src/vterm_internal.h 2>/dev/null; }; then \
		echo "make: vterm sources were restored under their patch stamps;" >&2; \
		echo "make:   discarding the stamps so the patches re-apply." >&2; \
		rm -f $$stamps; \
	fi)

# The default goal, named rather than left to whichever rule comes first.
# libssh-source below is a real rule ABOVE `all`, and make takes the first
# one it reads: from f6a3525 until this line, a plain `make` patched libssh
# and built nothing else -- unnoticed because every build here named its
# target, and found when dpkg-buildpackage's `make` produced no program.
.DEFAULT_GOAL := all

# --- vendored libssh ---------------------------------------------------------
# A submodule pinned to the libssh-0.11.5 tag, built static by its own
# cmake, with this tree's patches (libssh-patch/) applied first. Vendored
# rather than Debian's because a live session moving between processes
# (project.md sec 8.4) needs a transport export and import that libssh
# does not have, carried here as patches the way vterm-patch/ is and
# offered upstream whole -- the holder's direction, 2026-10-22, and a
# static libssh in the .deb is fine by them (2026-10-23). Every build --
# beerssh, beersshd, the tests, Android -- links this one copy.
#
# The tag was checked against the release tarball that
# tool/build-deps-android.sh verified by checksum and signature: all 477
# files identical, the tag adding only its git and CI metadata.
LIBSSH_DIR       = libssh
LIBSSH_BUILD_DIR = build-libssh
LIBSSH_PREFIX    = $(LIBSSH_BUILD_DIR)/prefix
LIBSSH_LIB       = $(LIBSSH_PREFIX)/lib/libssh.a
LIBSSH_PATCHES   = $(sort $(wildcard libssh-patch/*.patch))
LIBSSH_JOBS     ?= $(shell nproc 2>/dev/null || echo 2)

LIBSSH_STAMP     = $(LIBSSH_BUILD_DIR)/.patched

# The stamp says the sources WERE patched, the way vterm's does, and is
# witnessed at parse time the same way: `git -C libssh checkout -- .` or
# `git submodule update` rewrites the sources under a stamp that
# survives, and the next build would compile an unpatched libssh in
# silence. One grep per patched file for a token only the patches put
# there. A missing token discards the stamp, and the file the patch
# CREATES goes with it -- a checkout leaves an untracked file alone, and
# the patch cannot re-apply over one that is already there.
#
# A stamp rather than re-checking the patches on every build, so that a
# sabotage edit to a patched file (tool/sabotage.py) is compiled as
# edited instead of being refused as a patch that no longer applies.
BSSH_LIBSSH_WITNESS := $(shell \
	if [ -f $(LIBSSH_STAMP) ] \
	   && ! { grep -q ssh_session_import_transport $(LIBSSH_DIR)/include/libssh/libssh.h 2>/dev/null \
	          && grep -q ssh_socket_import_fd $(LIBSSH_DIR)/src/socket.c 2>/dev/null \
	          && grep -q ssh_socket_import_fd $(LIBSSH_DIR)/include/libssh/socket.h 2>/dev/null \
	          && grep -q '^void ssh_client_connection_callback' $(LIBSSH_DIR)/src/client.c 2>/dev/null \
	          && grep -q ssh_client_connection_callback $(LIBSSH_DIR)/include/libssh/session.h 2>/dev/null \
	          && grep -q transport_export.c $(LIBSSH_DIR)/src/CMakeLists.txt 2>/dev/null \
	          && grep -q ssh_session_export_transport $(LIBSSH_DIR)/src/libssh.map 2>/dev/null \
	          && [ -f $(LIBSSH_DIR)/src/transport_export.c ]; }; then \
		echo "make: libssh sources were restored under their patch stamp;" >&2; \
		echo "make:   discarding the stamp so the patches re-apply." >&2; \
		rm -f $(LIBSSH_STAMP) $(LIBSSH_DIR)/src/transport_export.c; \
	fi)

# The sources, fetched and patched, and nothing built: shared by the
# desktop archive below and tool/build-deps-android.sh, so the two cannot
# build different libssh.
libssh-source: $(LIBSSH_STAMP)

# Patching is idempotent -- a patch already in the tree is recognised and
# left -- and a patch that no longer applies fails here, naming upstream,
# rather than compiling a libssh without it. The proof that the set still
# applies from clean is check-ci's, which builds from the gitlink.
$(LIBSSH_STAMP): $(LIBSSH_PATCHES)
	@if [ ! -f $(LIBSSH_DIR)/CMakeLists.txt ]; then \
		if ! command -v git >/dev/null 2>&1; then \
			echo "make: $(LIBSSH_DIR)/ is empty and git is not installed." >&2; exit 1; \
		fi; \
		if ! git rev-parse --git-dir >/dev/null 2>&1; then \
			echo "make: $(LIBSSH_DIR)/ is empty and this is not a git checkout," >&2; \
			echo "make:   so there is nothing to fetch it from." >&2; exit 1; \
		fi; \
		echo "make: fetching the $(LIBSSH_DIR) submodule" >&2; \
		git submodule update --init $(LIBSSH_DIR) >&2 || exit 1; \
		[ -f $(LIBSSH_DIR)/CMakeLists.txt ] || { \
			echo "make: the $(LIBSSH_DIR) submodule could not be fetched" >&2; exit 1; }; \
	fi
	@mkdir -p $(@D)
	@echo "applying $(words $(LIBSSH_PATCHES)) patch(es) to vendored libssh"
	@for patch in $(LIBSSH_PATCHES); do \
		if git -C $(LIBSSH_DIR) apply --check "$(CURDIR)/$$patch" 2>/dev/null; then \
			git -C $(LIBSSH_DIR) apply "$(CURDIR)/$$patch" || exit 1; \
			echo "  $$patch: applied"; \
		elif git -C $(LIBSSH_DIR) apply --reverse --check "$(CURDIR)/$$patch" 2>/dev/null; then \
			echo "  $$patch: already applied"; \
		else \
			echo "  $$patch: DOES NOT APPLY, and is not already applied. Either" >&2; \
			echo "  upstream changed under it -- rebase the patch -- or the submodule" >&2; \
			echo "  was partly restored, leaving some of the patch's files patched:" >&2; \
			echo "  git -C $(LIBSSH_DIR) checkout -- . and build again." >&2; \
			exit 1; \
		fi; \
	done
	@touch $@

# FORCE, because only cmake knows whether its sources moved; an unchanged
# tree is a no-op build, and `cmake --install` leaves an unchanged archive
# alone, so nothing downstream relinks for nothing. -Os per
# build-and-commit.md (MinSizeRel). The options match
# tool/build-deps-android.sh's: static, with the server half beersshd
# needs, without examples or tests.
$(LIBSSH_LIB): $(LIBSSH_STAMP) FORCE
	@mkdir -p $(LIBSSH_BUILD_DIR)
	@if [ ! -f $(LIBSSH_BUILD_DIR)/build/CMakeCache.txt ]; then \
		cmake -S $(LIBSSH_DIR) -B $(LIBSSH_BUILD_DIR)/build \
			-DCMAKE_INSTALL_PREFIX="$(CURDIR)/$(LIBSSH_PREFIX)" \
			-DCMAKE_INSTALL_LIBDIR=lib \
			-DCMAKE_BUILD_TYPE=MinSizeRel \
			-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
			-DBUILD_SHARED_LIBS=OFF \
			-DWITH_EXAMPLES=OFF \
			-DWITH_SERVER=ON \
			-DWITH_GSSAPI=OFF \
			-DUNIT_TESTING=OFF \
			-DCLIENT_TESTING=OFF \
			-DSERVER_TESTING=OFF > $(LIBSSH_BUILD_DIR)/configure.log 2>&1 \
		|| { cat $(LIBSSH_BUILD_DIR)/configure.log >&2; rm -f $(LIBSSH_BUILD_DIR)/build/CMakeCache.txt; exit 1; }; \
	fi
	@cmake --build $(LIBSSH_BUILD_DIR)/build --parallel $(LIBSSH_JOBS) > $(LIBSSH_BUILD_DIR)/build.log 2>&1 \
		|| { tail -40 $(LIBSSH_BUILD_DIR)/build.log >&2; exit 1; }
	@# DESTDIR unset: `make install DESTDIR=...` exports it, cmake honours
	@# it, and the private archive went into the package's staging tree
	@# instead of build-libssh/prefix, where the link looks for it.
	@env -u DESTDIR cmake --install $(LIBSSH_BUILD_DIR)/build > $(LIBSSH_BUILD_DIR)/install.log 2>&1 \
		|| { cat $(LIBSSH_BUILD_DIR)/install.log >&2; exit 1; }

ifdef DEBUG
    QMAKE_CONFIG = CONFIG+=debug CONFIG-=release
else
    QMAKE_CONFIG = CONFIG+=release CONFIG-=debug
endif

ifdef SANITIZE
    QMAKE_CONFIG += QMAKE_CXXFLAGS+=-fsanitize=address,undefined \
                    QMAKE_CXXFLAGS+=-fno-omit-frame-pointer \
                    QMAKE_LFLAGS+=-fsanitize=address,undefined
endif

# What a plain `make` puts in the tree, named ONCE.
#
# `all` builds this list and `clean` removes it, from the same variable,
# so the two cannot drift apart. That is the whole fix rather than the
# missing line it replaces: a clean must always cause a rebuild -- that
# is what it is for -- and it can only promise that if it removes
# everything the default build produces. Adding a product to `all` now
# makes clean remove it, without anybody remembering to.
ARTIFACTS = $(TARGET) $(MAN_PAGE) $(COMPLETION) $(SSHD_TARGET) $(SSHD_MAN_PAGE)

all: $(ARTIFACTS)

# The shared Android vocabulary: android, android-aab, android-install,
# android-run, android-log, android-uninstall, android-check, and every
# variable they need. Copied verbatim from ~/.claude/tool/android.mk.
#
# Included AFTER `all` deliberately. `include` is where make first sees a
# target, so pulling it in above would make android-check the default goal:
# a plain `make` would run the preflight, fail for want of QT_ANDROID_ROOT,
# and build nothing. bbq-predictor did exactly that and did not notice for
# four sessions.
# The bundle android-play-check reads (play-store skill). Named before the
# include, because the fragment's own `ANDROID_AAB ?=` defines it empty and
# a `?=` after that would find it already set.
ANDROID_AAB ?= $(ANDROID_BUILD_DIR)/beerssh-$(VERSION).aab

# The listing's screenshots, in listing order (`make android-screenshots`,
# the play-store skill). Each is a view src/ui/store_view.cpp builds from
# made-up hosts and sessions, never the user's.
STORE_VIEWS ?= hosts terminal split settings

include tool/android.mk

# The NDK triple for the ABI, chosen BELOW the include because that is
# where ANDROID_ABI acquires a value.
#
# `ifeq` is evaluated as make READS the file, not when the variable is
# used. This chain sat above the include, so it tested an ANDROID_ABI that
# was still empty, matched nothing, and took the else -- handing every
# Android build of libvterm the 32-bit x86 triple. The objects compiled
# and archived without complaint and the failure surfaced two steps later
# as a wall of
#
#     ld.lld: error: libvterm.a(screen.o) is incompatible with aarch64linux
#
# which names neither the ABI nor this chain. Found on the first Android
# build that ever got as far as linking.
#
# The else no longer guesses. An unrecognised ABI leaves the triple empty
# and the compile rule refuses, because the cost of guessing here is an
# archive that looks built and is for the wrong machine -- the same reason
# tool/android.mk refuses a kit that will not say what it is. It cannot
# be an $(error): this file is read by every plain `make`, where
# ANDROID_ABI is legitimately empty and nothing Android is being asked
# for.
ifeq ($(ANDROID_ABI),arm64-v8a)
    ANDROID_TRIPLE = aarch64-linux-android
else ifeq ($(ANDROID_ABI),armeabi-v7a)
    ANDROID_TRIPLE = armv7a-linux-androideabi
else ifeq ($(ANDROID_ABI),x86_64)
    ANDROID_TRIPLE = x86_64-linux-android
else ifeq ($(ANDROID_ABI),x86)
    ANDROID_TRIPLE = i686-linux-android
else
    ANDROID_TRIPLE =
endif

# --- the manual page (project.md sec 8.6) -----------------------------------
# Generated by the binary from the same table its options are parsed from,
# which is how fmake does it and for the same reason: a manual page kept
# as a file is a second copy of the option list, and it is exactly where
# nobody notices the drift -- it documents a flag that no longer exists
# and omits the one you wanted, and neither is an error.
#
# Not committed, for that reason. `--man` needs no display (it is answered
# before QApplication is built), so this works in a build chroot.
$(MAN_PAGE): $(TARGET)
	./$(TARGET) --man > $@

# beersshd's, on the same argument: its --help and this page are written
# from one option table in src/sshd/main.cpp. It is a package of its own
# (sec 19), and a package's command without a page is what lintian warns
# about.
$(SSHD_MAN_PAGE): $(SSHD_TARGET)
	./$(SSHD_TARGET) --man > $@

# The same argument one file over, and the same rule shape. `bash -n`
# because a completion that does not parse is sourced by every new shell
# of everybody who installs it, and the failure lands in their prompt
# rather than in this build -- so it is checked here, where somebody is
# looking, and the file is not left behind when it fails.
$(COMPLETION): $(TARGET)
	./$(TARGET) --completion > $@
	@if command -v bash >/dev/null 2>&1; then \
		bash -n $@ || { rm -f $@; exit 1; }; \
	fi

# One rule, two stamps: both trees compile the same patched source, so
# both run the same recipe, and each stamp records only that ITS tree's
# objects were built from patched source. The loop is idempotent -- a
# patch already in the tree is recognised and skipped -- so whichever
# stamp is made first applies and the other confirms. Building both
# archives stale under -j can race two apply loops; the loser fails
# loudly at `git apply` and a rerun succeeds, which is the right
# direction to fail in.
$(VTERM_STAMP) $(VTERM_ANDROID_STAMP): $(VTERM_PATCHES)
	@mkdir -p $(@D)
	@echo "applying $(words $(VTERM_PATCHES)) patch(es) to vendored libvterm"
	@# As a series, not one patch at a time: two patches touching the
	@# same lines defeat a per-patch "applies in reverse" check, which is
	@# what reported a fully patched tree as partly restored once 0011
	@# edited 0010's lines. tool/apply-patches.sh says how.
	@sh tool/apply-patches.sh $(VTERM_DIR) $(addprefix $(CURDIR)/,$(VTERM_PATCHES))
	@touch $@

$(VTERM_BUILD_DIR)/%.o: $(VTERM_DIR)/src/%.c $(VTERM_STAMP)
	@mkdir -p $(VTERM_BUILD_DIR)
	$(CC) $(VTERM_CFLAGS) -MMD -MP -c $< -o $@

# Read back what -MMD wrote, or it wrote nothing worth having: a .d file
# nobody includes is dependency tracking that looks on and is off, and
# an object stale against a header surfaces as nonsense far from here.
# These objects include vterm_internal.h; before this line, touching it
# rebuilt nothing.
-include $(VTERM_OBJS:.o=.d)

# A missing submodule is the one failure worth catching by hand, and the
# guard that used to sit here could not do it.
#
# It hung off $(VTERM_SRCS), which comes from a wildcard: when the
# submodule is absent that list is EMPTY, so the rule had no targets and
# never fired -- a check over an empty file list, which reports success
# exactly as loudly as a real pass. With the submodule present the list
# holds only files that already exist, so it could not fire then either.
# It was dead in both directions.
#
# The empty list does something worse than nothing, though: it strips this
# rule's prerequisites, and a rule with none fires whenever its target is
# missing. So make ran `ar rcs build-vterm/libvterm.a` with no members, and
# nothing had created the directory either, giving
#
#     ar: build-vterm/libvterm.a: No such file or directory
#
# which names neither the submodule nor the fix. The check therefore lives
# in the recipe, where it runs whether or not the list is empty, and the
# mkdir is here rather than only in the object rule, which does not run
# when there are no objects to build.
$(VTERM_LIB): $(VTERM_OBJS)
	@mkdir -p $(VTERM_BUILD_DIR)
	@if [ -z "$(strip $(VTERM_OBJS))" ]; then \
		echo "make: there are no $(VTERM_DIR) sources to put in $@."; \
		echo "make:   The submodule is fetched automatically when it is"; \
		echo "make:   missing, so reaching here means that failed -- no"; \
		echo "make:   network, or not a git checkout. Recover it with:"; \
		echo "make:     git submodule update --init"; \
		exit 1; \
	fi
	$(AR) rcs $@ $(VTERM_OBJS)

# A qmake Makefile regenerates ITSELF from the project file it was made
# from, before doing anything else -- including `clean`. So a build tree
# that outlives a rename of its .pro cannot even be cleaned: build-tests/
# made before 1080cee moved tests/ to test/ answered every `make -C` with
# "Cannot find file: ../tests/tests.pro", and `make deb` died in
# dh_auto_clean on a machine that had built the tests once.
#
# qmake records the file on its fourth line, `# Project:  <path>`, relative
# to the build directory. Non-empty when DIR/Makefile names a project file
# that no longer exists; empty when it is current, or when there is no
# Makefile at all. `.` rather than a literal hash in the pattern, because
# make reads that as a comment before the shell ever sees it -- and no
# `case`, whose unbalanced `)` would end the $(shell) call early and make
# every tree read as orphaned.
bssh_qmake_orphaned = $(shell \
	pro=$$(sed -n 's/^. Project: *//p' $(1)/Makefile 2>/dev/null); \
	[ -n "$$pro" ] || exit 0; \
	path="$$pro"; [ "$${pro#/}" != "$$pro" ] || path="$(1)/$$pro"; \
	[ -f "$$path" ] || echo "$$pro")

FORCE:

$(BUILD_DIR)/Makefile: beerssh.pro
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && $(QMAKE) ../beerssh.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX) \
	        BSSH_VERSION=$(VERSION)

$(TARGET): $(VTERM_LIB) $(LIBSSH_LIB) $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(BUILD_DIR)
	cp $(BUILD_DIR)/$(TARGET) $(TARGET)

run: $(TARGET)
	./$(TARGET)

# beersshd, the SFTP-only server (project.md sec 19): its own program from
# its own .pro, sharing the libssh build and none of the terminal. In the
# default build so check-ci compiles it against the runner's older Qt and
# libssh, which is where an API it leans on would first be missing.
SSHD_BUILD_DIR = build-sshd
SSHD_SOURCES   = $(wildcard src/sshd/*.cpp src/sshd/*.h)

$(SSHD_BUILD_DIR)/Makefile: beersshd.pro \
                            $(if $(call bssh_qmake_orphaned,$(SSHD_BUILD_DIR)),FORCE)
	mkdir -p $(SSHD_BUILD_DIR)
	cd $(SSHD_BUILD_DIR) && $(QMAKE) ../beersshd.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX) \
	        BSSH_VERSION=$(VERSION)

$(SSHD_TARGET): $(LIBSSH_LIB) $(SSHD_BUILD_DIR)/Makefile $(SSHD_SOURCES)
	$(MAKE) -C $(SSHD_BUILD_DIR)
	cp $(SSHD_BUILD_DIR)/$(SSHD_TARGET) $(SSHD_TARGET)

# Built in its own directory from its own .pro: the test binary needs the
# app's sources but not src/main.cpp, or it would have two main()s.
TEST_BUILD_DIR = build-tests
TEST_BIN       = $(TEST_BUILD_DIR)/beerssh_test

# FORCE only when the tree was generated from somewhere else: re-running
# qmake is the one thing that repairs it, and the stale Makefile is always
# newer than test/tests.pro, so the timestamp alone never would.
$(TEST_BUILD_DIR)/Makefile: test/tests.pro \
                            $(if $(call bssh_qmake_orphaned,$(TEST_BUILD_DIR)),FORCE)
	mkdir -p $(TEST_BUILD_DIR)
	cd $(TEST_BUILD_DIR) && $(QMAKE) ../test/tests.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX) \
	        BSSH_VERSION=$(VERSION)

$(TEST_BIN): $(VTERM_LIB) $(LIBSSH_LIB) $(TEST_BUILD_DIR)/Makefile $(SOURCES) $(HEADERS) $(wildcard test/*.cpp test/*.h)
	$(MAKE) -C $(TEST_BUILD_DIR)

# The offscreen QPA plugin is what makes both of these runnable in CI at
# all -- a GUI that only starts with a display is a GUI CI never exercises.
# The smoke test starts the REAL app, which owns a real profile store and
# writes a real host list -- into ~/.ssh now (sec 10.6). A build step does
# not get to edit the config of whoever ran it, so it runs against a
# throwaway HOME. The unit tests sandbox themselves for the same reason;
# see test/main.cpp.
# A generated source whose header has MOVED looks current to make, and
# stays that way until somebody deletes its object file.
#
# Measured: `keyboard: move the keyboard to its own directory` (6a03c7c,
# 2026-09-06) was a rename, so keyboard/key_row.h kept its old mtime of
# 2026-08-07. The moc generated back when the header was at
# src/ui/key_row.h is dated 2026-09-04 -- NEWER than its own header --
# so make correctly saw nothing to do. Two of these sat in build/ and
# build-tests/ for six days, and both only surfaced when an out-of-memory
# kill made a rebuild delete the objects and compile the stale sources
# for the first time. The error then blames a missing header and names
# neither the move nor the fix.
#
# Timestamps cannot see this: make compares mtimes and never content, so
# the check has to read what the generated file actually includes. One
# line each, no false positives across the 86 in this tree.
#
# It says HOW MANY it read, because a gate over an empty list reports
# success exactly as loudly as a real pass and the empty case is
# reachable -- a clone that has never been built has no moc anywhere.
#
# The count earned itself immediately by correcting a guess. Without it
# the container run was assumed to have inspected nothing, on the
# reasoning that check-ci builds from `git archive HEAD`; with it, the
# container reports 31, because it builds the application before running
# the suite and that populates $(BUILD_DIR) first. A number is the
# difference between knowing and assuming what a check looked at, which
# is the whole argument for printing one.
check-build-tree:
	@bad=0; seen=0; \
	for dir in $(BUILD_DIR) build-tests; do \
		[ -d "$$dir" ] || continue; \
		for moc in "$$dir"/moc_*.cpp; do \
			[ -e "$$moc" ] || continue; \
			seen=$$((seen + 1)); \
			header=$$(grep -m1 -oE '#include "[^"]+\.h"' "$$moc" \
			          | sed 's/#include "//; s/"//'); \
			[ -n "$$header" ] || continue; \
			if [ ! -f "$$dir/$$header" ]; then \
				echo "check-build-tree: $$moc was generated from $$header," >&2; \
				echo "check-build-tree:   which no longer exists -- the header moved" >&2; \
				echo "check-build-tree:   and this file is newer than its own source," >&2; \
				echo "check-build-tree:   so make cannot see it. Remove it:" >&2; \
				echo "check-build-tree:     rm $$moc $${moc%.cpp}.o" >&2; \
				bad=1; \
			fi; \
		done; \
	done; \
	[ $$bad -eq 0 ] || exit 1; \
	if [ $$seen -eq 0 ]; then \
		echo "check-build-tree: no build tree here yet, so nothing was checked"; \
	else \
		echo "check-build-tree: $$seen generated source(s), every one names a header that exists"; \
	fi

test: check-build-tree $(TARGET) $(TEST_BIN)
	@sandbox=$$(mktemp -d) && \
	 HOME=$$sandbox XDG_CONFIG_HOME=$$sandbox/config XDG_DATA_HOME=$$sandbox/data \
	 QT_QPA_PLATFORM=offscreen ./$(TARGET) --self-test; \
	 status=$$?; rm -rf $$sandbox; exit $$status
	@echo "test: headless smoke test passed"
	@# Room to run in, checked before starting rather than discovered as
	@# sixteen assertion failures.
	@#
	@# The suite sandboxes HOME and XDG_* into a temporary directory
	@# (test/main.cpp) so it cannot write into whoever ran it. On a full
	@# filesystem those writes fail, and the suite reports things like
	@# "write_ini() returned FALSE" and settings that do not persist --
	@# which reads exactly like a regression in the code under test. That
	@# cost an afternoon once. It writes only small INI files, so a small
	@# floor is enough to tell "no room" from "broken".
	@#
	@# TMPDIR moves the sandbox, since QDir::tempPath honours it: that is
	@# the fix when /tmp is a tmpfs somebody else is filling.
	@tmp=$${TMPDIR:-/tmp}; 	free=$$(df -Pm "$$tmp" 2>/dev/null | awk 'NR==2 {print $$4}'); 	if [ -n "$$free" ] && [ "$$free" -lt 64 ]; then 		echo "test: only $${free}MB free on $$tmp -- refusing to run." >&2; 		echo "test:   The suite sandboxes into a temporary directory, so a full" >&2; 		echo "test:   filesystem surfaces as failing assertions rather than as a" >&2; 		echo "test:   disk error, and looks like a regression that is not there." >&2; 		echo "test:   Free some space, or point TMPDIR somewhere with room." >&2; 		exit 1; 	fi
	QT_QPA_PLATFORM=offscreen $(BSSH_TEST_CRASH_ENV) ./$(TEST_BIN)

# Throughput through the emulator, in MB/s (project.md sec 9.5, sec 12.3).
# Built by `test` but not run by it: these take seconds and their numbers
# move with whatever else the machine is doing, which is not a thing to
# fail a build over. Run on an idle machine, and compare a run against
# another run of the same binary rather than against a number in a doc.
#
# And it REFUSES when the machine cannot answer, rather than answering
# wrongly. "Load average is part of the result" was printed above the
# numbers and did not help: this target reported a full-screen repaint at
# 35 ms against the 2.3 ms in sec 9.5 -- fifteen times worse, and nothing
# in the output distinguished a real regression from a box at load 81 on
# twelve cores with 23 GB in swap, which is what it actually was. A
# printed number is a number somebody quotes later.
# QtTest forks gdb when a test crashes, and that is a poor trade in a tree
# whose method is to break the code on purpose and watch a test fail.
#
# What happened: a crashing test left `gdb --batch --pid <self>` attached,
# the binary stopped in state `t`, and neither ever exited. Because a
# tracer becomes the reaping parent, the dead test showed as a zombie that
# nothing could clear. Five of them accumulated over one session holding
# **15 GB** of RSS between them, on a machine whose load and swap pressure
# were then blamed on somebody else's build -- including in this file's own
# `bench` guard. A stopped process also ignores SIGTERM, so the obvious
# `pkill` cleaned up nothing.
#
# The signal number and the test that raised it are still reported, which
# is enough to know where to look. Ask for the backtrace explicitly when a
# crash is worth one:
#   make test BSSH_TEST_STACK_DUMP=1
ifdef BSSH_TEST_STACK_DUMP
    BSSH_TEST_CRASH_ENV =
else
    BSSH_TEST_CRASH_ENV = QTEST_DISABLE_STACK_DUMP=1 QTEST_DISABLE_CORE_DUMP=1
endif

bench: $(TEST_BIN)
	@echo "bench: measuring on this machine -- load average is part of the result"
	@uptime
	@# Skipped where there is no /proc/loadavg to read, rather than
	@# guessed at: refusing to measure on a machine whose load cannot be
	@# read would make the check worse than useless off Linux.
	@if [ -r /proc/loadavg ] && [ -z "$(BSSH_BENCH_ANYWAY)" ]; then \
		load=$$(cut -d' ' -f1 /proc/loadavg | cut -d. -f1); \
		cores=$$(nproc 2>/dev/null || echo 1); \
		if [ "$$load" -gt $$((cores / 2 + 1)) ]; then \
			echo "bench: load is $$load on $$cores cores; these numbers would not mean" >&2; \
			echo "bench:   anything, and a wrong number outlives the run that made it." >&2; \
			echo "bench:   Wait for the machine, or BSSH_BENCH_ANYWAY=1 to measure" >&2; \
			echo "bench:   regardless -- which is right for an A/B where both sides" >&2; \
			echo "bench:   are run interleaved and only the ratio is claimed." >&2; \
			exit 1; \
		fi; \
	fi
	QT_QPA_PLATFORM=offscreen $(BSSH_TEST_CRASH_ENV) ./$(TEST_BIN) --bench

# Renders every screen to a PNG so somebody can look at them (sec 12.3).
#
# Not a test and it cannot fail: it writes pictures and prints the minimum
# size each surface demands. It is here because looking is what kept
# working -- every layout defect in this project so far was found by
# rendering a screen and looking at it, and several had passed every test
# that existed at the time. The tests came after the picture.
#
# Offscreen, so it needs no display and runs the same on a CI box as on a
# laptop. Point BSSH_SHOT_DIR somewhere else to keep a set for comparison.
SHOT_DIR ?= build-shots

shots: $(TEST_BIN)
	@echo "shots: rendering every screen into $(SHOT_DIR)/"
	@mkdir -p $(SHOT_DIR)
	@# Last run's pictures go first, or the directory accumulates
	@# screens the app no longer has. It held desktop-page-ssh.png
	@# after that category was removed, desktop-page-window.png after
	@# it was renamed, and two Solarized terminals after those schemes
	@# were deleted -- so a reader browsing it would have concluded all
	@# three still existed. A picture of something the app never does is
	@# worse than no picture, which is this tool's own rule.
	@#
	@# A wildcard, because the names are not knowable: the set changes
	@# whenever a surface is added or dropped, which is the case
	@# build-and-commit.md allows it for. The directory is checked
	@# first all the same -- non-empty and relative, and one this build
	@# made.
	@case "$(SHOT_DIR)" in \
		"" ) echo "shots: SHOT_DIR is empty" >&2; exit 1 ;; \
		/* ) echo "shots: refusing to clear the absolute path $(SHOT_DIR)" >&2; exit 1 ;; \
		*..* ) echo "shots: refusing to clear $(SHOT_DIR) -- it escapes the tree" >&2; exit 1 ;; \
	esac
	@rm -f $(SHOT_DIR)/*.png
	@# Through a FILE rather than a pipe: a pipeline reports the exit
	@# status of its last command, so `binary | grep` would report grep's
	@# and swallow a crash into a clean-looking run.
	@# BSSH_FONT_DIR because the candidate paths are relative to the
	@# BINARY, and the test binary lives in build-tests/ -- so it looks
	@# one level above the checkout and finds nothing. Without it the
	@# terminal shots are drawn in whatever monospace the machine has,
	@# which is not what the app does. The renderer says which font it
	@# used either way.
	@if BSSH_SHOT_DIR=$(SHOT_DIR) BSSH_FONT_DIR=$(CURDIR)/fonts \
	     QT_QPA_PLATFORM=offscreen \
	     ./$(TEST_BIN) --shots > $(SHOT_DIR)/.run.log 2>&1; then \
		grep -vE '^(PASS|QWARN|Config|\*\*|Totals)' $(SHOT_DIR)/.run.log; \
		rm -f $(SHOT_DIR)/.run.log; \
		echo "shots: $$(ls $(SHOT_DIR)/*.png | wc -l) images in $(SHOT_DIR)/"; \
	else \
		cat $(SHOT_DIR)/.run.log; \
		rm -f $(SHOT_DIR)/.run.log; \
		echo "shots: rendering failed" >&2; \
		exit 1; \
	fi

# sec 12.2's other half: a package that BUILDS is not a package that
# works. Installs both .debs into a clean container, lets apt resolve the
# dependencies the binary itself declared, and then runs the app -- which
# is where a missing library, a missing Qt plugin or an unreadable font
# path actually shows up. Needs docker.
#
# No `deb` prerequisite and no DEB_DIR: the script exports HEAD to a
# throwaway tree and builds there, so what it installs is a commit rather
# than whatever the working tree held while dpkg-buildpackage ran. That
# closes the last of the three staging gaps -- check-ci and
# check-keepalive have the same footing -- and it retires this line's
# habit of naming the output directory, which had twice drifted away from
# where `deb` actually writes.
#
# Building here instead would be faster and is still available when the
# question is about the container half rather than the sources:
#   make deb && DEB_DIR=$(DEB_DIR) ./tool/check-deb.sh
check-deb:
	./tool/check-deb.sh

# The gate's own suite runs first, then the gate's verdict.
#
# tool/style_gate.py is copied verbatim into every private project from
# ~/.claude/tool/, and tool/test_style_gate.py is its suite, copied the
# same way. Measured 2026-09-04 across eight trees -- beerssh, hydra,
# situ, fmake, netcfgd, apt-emerge, fuzzypickles and raidcfgd -- every
# one carried the suite and not one ran it. The tests had travelled
# everywhere and were executed only in the tree they came from, so a
# defect in the shared gate would have been caught in one place out of
# nine.
#
# It runs ahead of style-source and style-docs because a gate's pass is
# worth reading only once the gate is known to work: a checker that has
# stopped inspecting anything reports success exactly as loudly as one
# that discriminates. Seconds rather than minutes -- 9 and 16 on two
# runs of this machine -- and the cheapest positive control here.
#
# Wiring it here rather than under `test` is deliberate and reaches
# further: check-ci.sh runs `make style` inside the container against a
# tree exported from `git archive HEAD`, so the suite is proved on a
# clean checkout as well as on a developer machine. `make test` never
# leaves this machine.
style: style-tool style-source style-docs style-sabotage

style-tool:
	python3 tool/test_style_gate.py


style-source:
	python3 tool/style_gate.py check

# project.md is authoritative, so it is held to the tree: a heading
# that appears twice means whichever one you find, the other is the
# one with the answer.
style-docs:
	python3 tool/style_gate.py docs

# The sabotage specs are held to the tree too, and for a sharper reason
# than the docs are: a sabotage whose anchor no longer matches is
# indistinguishable from a check that cannot fail, and NOTHING in a build
# or a test run says so. Fifteen of them had rotted that way before
# anybody thought to look (sec 12.3), and the only thing that would have
# found them sooner is running the specs -- one suite run per edit, which
# is why nobody did.
#
# Under `style` rather than `test` because it reads and builds nothing, so
# it costs a second and runs everywhere; and because check-ci.sh runs
# `make style` in the container against `git archive HEAD`, which puts it
# in front of every push. It is the same argument that put the style
# gate's own suite here.
#
# What it does NOT establish is in tool/sabotage.py, at length, because a
# cheap check's green line gets quoted for the expensive one's guarantee.
style-sabotage:
	python3 tool/sabotage.py --check

# sec 12.4's runner-parity check. Needs docker; not part of `test`, and
# deliberately not a CI step -- CI already IS the runner. Run it before
# pushing anything that touches a platform or library API, because the
# runner's Qt and libssh are older than any development machine's and an
# API added later compiles here and fails there.
check-ci:
	./tool/check-ci.sh

# sec 12.3's one test against a real server. Needs sshd; not part of
# `test`, which runs entirely against the fake transport. The libssh
# binding is the only layer a fake cannot exercise.
# BSSH_TEST_BIN is passed rather than defaulted in the script, so the
# path to the binary is written once. It was written twice and they
# disagreed: the script said build-test and this says build-tests, so
# the target could not run at all between 2026-08-25 and 2026-09-07.
check-ssh: $(TEST_BIN)
	BSSH_TEST_BIN=$(TEST_BIN) ./tool/check-ssh.sh

# Sec 8.4's process migration, and the one part of it with no prior art to
# copy: moving a LIVE connection between processes without the peer
# noticing. CRIU does the adjacent thing in the kernel with TCP_REPAIR;
# nothing does it at library level, so there is no existing design to read.
#
# Not part of `check`, and deliberately: it answers a design question that
# is still open rather than guarding behaviour the app has. It is kept
# runnable because what it measures depends on libssh internals that will
# move under it -- a finding about 0.11.5 is not a finding about whatever
# Debian ships next, and the only honest way to know is to run it again.
check-handoff: $(LIBSSH_LIB)
	LIBSSH_PREFIX=$(CURDIR)/$(LIBSSH_PREFIX) ./tool/handoff/run.sh

# sec 11's startup without a keyring: the first window within 5 seconds with
# no session bus, and with one whose secrets service never comes up. Both
# were 27 seconds. Needs Xvfb and dbus-run-session.
check-startup: $(TARGET)
	./tool/check-startup.sh

# sec 8.4's refusal of a drop on another BeerSSH process, on X11, with a
# control that the same drag onto nothing tears out. Needs Xvfb, xdotool
# and dbus-run-session; drives a real pointer on a private X server.
check-x11-drag: $(TARGET)
	./tool/check-x11-drag.sh

# sec 5.3's dead-peer detection against a link whose packets vanish.
# Needs docker and NET_ADMIN -- a closed socket is not a dropped link, and
# only iptables can make packets disappear rather than be refused. Slow by
# construction: the budget it verifies is about a minute.
check-keepalive:
	./tool/check-keepalive.sh

# --- the curated font set (project.md sec 9) --------------------------------
# Not a build dependency: the app runs without it, falling back to whatever
# monospace families the system has (sec 9.4). Making `all` depend on a
# 300 MB download would be a rude first build for a change that touches one
# line of the emulator.
fonts:
	./tool/fetch-fonts.sh

fonts-list:
	@./tool/fetch-fonts.sh --list

# sec 9.6's gate. Separate from `style` because it needs the fonts present
# and fontTools installed, neither of which a plain checkout has.
check-fonts:
	@python3 tool/check-fonts.py $(FONT_DIR)

# Compile the app's Java. Nothing else in the tree does (sec 5.2).
#
# android/src holds three files -- the foreground service, the biometric
# prompt and the keystore bridge -- and they are built ONLY by `make
# android`, which needs an NDK, a JDK, a Qt kit and ten minutes. So a
# change to any of them was unchecked until somebody built for Android,
# and the commit that fixed the service's START_STICKY said so in as many
# words: "no routine gate here sees it".
#
# This is the cheap half of that. It needs no NDK and no device: javac
# against the SDK's android.jar for the platform classes, Qt's Qt6Android
# jar for what its bindings expose, and Qt's own java sources on the
# sourcepath for QtActivity, which the service names. -implicit:none so
# Qt's sources are read for types and not compiled into our output.
#
# It REFUSES rather than skipping when the SDK or the kit is absent. A
# gate that needs the thing it checks for is weakest on the machines that
# lack it, and one that skips quietly there is a green light nobody
# earned -- so an absent SDK is an error with the reason named, and
# whoever has no SDK simply does not run this target.
ANDROID_JAVA_SRC = $(wildcard android/src/se/vibes/beerssh/*.java \
                              android-sshd/src/se/vibes/beersshd/*.java)

check-java:
	@test -n "$(ANDROID_JAVA_SRC)" || { \
		echo "check-java: no java under android/src -- nothing to compile," >&2; \
		echo "check-java:   which is not a pass. Has the tree moved?" >&2; \
		exit 1; \
	}
	@sdk="$(ANDROID_SDK_ROOT)"; \
	test -n "$$sdk" || sdk="$$HOME/Android/Sdk"; \
	plat=$$(ls -d "$$sdk"/platforms/android-* 2>/dev/null | sort -V | tail -1); \
	test -n "$$plat" || { \
		echo "check-java: no SDK platform under $$sdk/platforms." >&2; \
		echo "check-java:   Set ANDROID_SDK_ROOT. Refusing rather than skipping:" >&2; \
		echo "check-java:   a check that passes for want of its own tools is worse" >&2; \
		echo "check-java:   than one nobody ran." >&2; \
		exit 1; \
	}; \
	qt="$(QT_ANDROID_ROOT)"; \
	test -n "$$qt" -a -d "$$qt/jar" || { \
		echo "check-java: QT_ANDROID_ROOT does not name a Qt Android kit." >&2; \
		echo "check-java:   Needed for Qt6Android.jar and QtActivity's source." >&2; \
		exit 1; \
	}; \
	out=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$out"' EXIT INT TERM; \
	javac -nowarn -implicit:none \
	      -cp "$$plat/android.jar:$$qt/jar/Qt6Android.jar" \
	      -sourcepath "$$qt/src/android/java/src" \
	      -d "$$out" $(ANDROID_JAVA_SRC) || exit 1; \
	echo "check-java: $(words $(ANDROID_JAVA_SRC)) file(s) compile against $$(basename $$plat)"

.PHONY: check-java

$(VTERM_ANDROID_DIR)/%.o: $(VTERM_DIR)/src/%.c $(VTERM_ANDROID_STAMP)
	@mkdir -p $(VTERM_ANDROID_DIR)
	@test -n "$(ANDROID_TRIPLE)" || { \
		echo "android: ANDROID_ABI is '$(ANDROID_ABI)', which names no NDK triple." >&2; \
		echo "android:   Refusing rather than defaulting: a guessed triple builds" >&2; \
		echo "android:   an archive for the wrong machine that only fails at link." >&2; \
		exit 1; \
	}
	$(NDK_BIN)/clang --target=$(ANDROID_TRIPLE)$(ANDROID_API) $(VTERM_CFLAGS) \
	        -MMD -MP -c $< -o $@

# The host tree's .d files are read back beside the host rule; these are
# this tree's, for the same reason and with the same failure behind it.
-include $(VTERM_ANDROID_OBJS:.o=.d)

$(VTERM_ANDROID_LIB): $(VTERM_ANDROID_OBJS)
	$(NDK_BIN)/llvm-ar rcs $@ $(VTERM_ANDROID_OBJS)

# Everything the Android build needs, checked by name. Each of these fails
# unhelpfully when missing: a bare qmake error, a wall of missing symbols,
# a Gradle stack trace about JAVA_COMPILER, or a "platforms does not
# exist" from a path nobody set.
# This project's own preflight, run by android-check in tool/android.mk.
#
# Named through ANDROID_CHECK_LOCAL rather than by redefining android-check:
# make takes the LAST definition of a target it reads, silently, so a second
# android-check here would replace the shared one and quietly retire every
# check the fragment carries. The kit, ABI, NDK, SDK and JDK checks that used
# to live here are all in the fragment now, stated in the same words -- they
# were written here first.
ANDROID_CHECK_LOCAL = android-check-beerssh

android-check-beerssh:
	@if [ ! -f deps/android/$(ANDROID_ABI)/lib/libssh.a ]; then \
		echo "android: dependencies for $(ANDROID_ABI) are not built." >&2; \
		echo "android:   run: tool/build-deps-android.sh $(ANDROID_ABI)" >&2; \
		exit 1; \
	fi
	@# An archive built before beersshd has no server half, and that
	@# surfaces as undefined ssh_bind_* at its link rather than here.
	@if ! grep -q 'server=on' deps/android/$(ANDROID_ABI)/lib/libssh.config 2>/dev/null; then \
		echo "android: deps/android/$(ANDROID_ABI) has libssh without its server half." >&2; \
		echo "android:   run: tool/build-deps-android.sh $(ANDROID_ABI), which rebuilds it" >&2; \
		exit 1; \
	fi

.PHONY: android-check-beerssh

# Which project an Android build is of, and what it bundles beside its
# code. BeerSSH's by default; android-sshd below names beersshd's.
ANDROID_PRO    ?= beerssh.pro
ANDROID_ASSETS ?= android-fonts android-terminfo

$(ANDROID_BUILD_DIR)/Makefile: android-check $(VTERM_ANDROID_LIB) $(ANDROID_PRO)
	mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && $(QT_ANDROID_ROOT)/bin/qmake ../$(ANDROID_PRO) $(QMAKE_CONFIG) \
	        BSSH_VERSION=$(VERSION) BSSH_VERSION_CODE=$(ANDROID_VERSION_CODE)

# The base font set that ships inside the APK (project.md sec 9.4).
#
# Three families, not eight: the default, the box-drawing fallback, and one
# alternative. Everything else is downloaded on the phone, because the
# full set is 54 MB and Iosevka is 42 MB of that on its own -- the condensed
# face that would otherwise be the phone default (sec 9.2) cannot ship in a
# base download and has to arrive later.
#
# Noto Sans Mono is in the base set as the box-drawing and symbol fallback.
# It was put there to stop CJK rendering as tofu, which it does not do:
# Noto Sans *Mono* is Latin, Greek and Cyrillic, and CJK lives in the
# separate Noto Sans CJK families that are tens of megabytes each. Checked
# with fc-query, and then on a device, where a Japanese `ls` came back as
# a row of boxes -- which is precisely the failure that comment said could
# not be allowed.
#
# So Unifont IS in the base set. It is the last resort sec 9.3's chain
# already names, and leaving it out silently truncated that chain on the
# one platform where the set is reduced. 4.9 MB covers the whole BMP,
# which is a remarkable price for "no terminal output is ever unreadable"
# -- and Japanese, Chinese and Korean are not scripts a phone will not
# meet. Everything else is downloaded when chosen: the full set is 374 MB and
# Iosevka alone is 10 MB of it.
ANDROID_BASE_FONTS = \
	JetBrainsMono-Regular.ttf JetBrainsMono-Bold.ttf \
	JetBrainsMono-Italic.ttf JetBrainsMono-BoldItalic.ttf \
	DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf \
	DejaVuSansMono-Oblique.ttf DejaVuSansMono-BoldOblique.ttf \
	Hack-Regular.ttf Hack-Bold.ttf Hack-Italic.ttf Hack-BoldItalic.ttf \
	NotoSansMono-Regular.ttf \
	unifont.otf

ANDROID_ASSET_DIR = android/assets/fonts
ANDROID_TERMINFO_DIR = android/assets/terminfo

# The terminfo entry a device with no terminfo database needs
# (project.md sec 12.3p).
#
# xterm-256color and nothing else, because that is the TERM this app
# already sends -- BSSH_TERM in the libssh backend, and the default
# preset's. Shipping it adds no claim; it lets the device look up the
# claim we make everywhere else. Shipping a DIFFERENT entry would be a
# new claim about what the terminal can do, which sec 7.1 owns.
#
# Generated from the build machine's own ncurses rather than checked in,
# which is what fonts/ does and for the same reason: it is somebody
# else's data with somebody else's terms, and a tree that generates it
# carries neither. `infocmp` then `tic` rather than copying the compiled
# file, so the provenance is a command anybody can re-run.
BSSH_TERMINFO_ENTRY = xterm-256color

terminfo:
	@if ! command -v infocmp >/dev/null 2>&1 || ! command -v tic >/dev/null 2>&1; then \
		echo "terminfo: no infocmp/tic here -- skipping"; \
		echo "terminfo:   a device without a terminfo database keeps the"; \
		echo "terminfo:   behaviour it has now (sec 12.3p)"; \
		exit 0; \
	fi; \
	mkdir -p terminfo; \
	infocmp -x $(BSSH_TERMINFO_ENTRY) > terminfo/$(BSSH_TERMINFO_ENTRY).src; \
	tic -x -o terminfo terminfo/$(BSSH_TERMINFO_ENTRY).src; \
	rm -f terminfo/$(BSSH_TERMINFO_ENTRY).src; \
	echo "terminfo: $$(find terminfo -type f | wc -l) entry(ies), $$(du -sh terminfo | cut -f1)"

# Copied rather than symlinked, for android-fonts' reason: androiddeployqt
# packages what it FINDS in the asset directory.
#
# Skipped silently when terminfo/ is absent, so `make android` keeps
# working on a machine with no ncurses -- the app then behaves exactly as
# it did before this existed, which is what an empty
# terminfo_directory() means.
android-terminfo: terminfo
	@if [ ! -d terminfo ]; then \
		echo "android-terminfo: no terminfo/ -- APK ships none"; \
		exit 0; \
	fi; \
	mkdir -p $(ANDROID_TERMINFO_DIR); \
	cp -r terminfo/. $(ANDROID_TERMINFO_DIR)/; \
	echo "android-terminfo: bundled $$(find $(ANDROID_TERMINFO_DIR) -type f | wc -l) entry(ies)"


# Copied rather than symlinked: androiddeployqt packages what it finds in
# the asset directory, and a symlink into a directory that is not there on
# a build machine packages nothing while looking like it worked.
#
# Silently skipped when fonts/ is absent. `make android` has to keep
# working on a checkout that has not run `make fonts` -- the app falls back
# to the system's own monospace font (sec 9.4), which is exactly what it
# did before this milestone.
android-fonts:
	@if [ ! -d fonts ]; then \
		echo "android-fonts: no fonts/ -- APK will use system fonts only"; \
		echo "android-fonts:   run 'make fonts' first to bundle the base set"; \
		exit 0; \
	fi; \
	mkdir -p $(ANDROID_ASSET_DIR); \
	mkdir -p $(ANDROID_ASSET_DIR)/licences; \
	for font in $(ANDROID_BASE_FONTS); do \
		if [ -f "fonts/$$font" ]; then \
			cp -u "fonts/$$font" $(ANDROID_ASSET_DIR)/; \
		else \
			echo "android-fonts: missing fonts/$$font"; exit 1; \
		fi; \
	done; \
	cp -u fonts/licences/OFL-JetBrainsMono.txt \
	      fonts/licences/LICENSE-DejaVu.txt \
	      fonts/licences/LICENSE-Hack.md \
	      fonts/licences/OFL-NotoSansMono.txt \
	      fonts/licences/LICENSE-Unifont.txt \
	      $(ANDROID_ASSET_DIR)/licences/ 2>/dev/null || true; \
	echo "android-fonts: bundled $$(du -sh $(ANDROID_ASSET_DIR) | cut -f1) of fonts"

# The APK is what gets installed on a device or emulator, so it is the
# default. The AAB is what Play wants (sec 2) and is its own target.


# The artifact Play actually takes (sec 13), named and checked like the
# APK next door.
#
# `android-aab` leaves Gradle's own name somewhere under build/outputs,
# which says nothing about which app or which version it holds -- and an
# AAB matters more than an APK here, because it is the one that gets
# uploaded to a store and cannot be taken back once a version code has
# been consumed.
#
# The versionCode is echoed rather than assumed. Play refuses an upload
# that does not exceed the last one it accepted, and finding that out
# from the Console after a twenty-minute upload is a poor way to learn
# what number you built.
AAB_NAME = $(notdir $(ANDROID_AAB))

android-aab: $(ANDROID_BUILD_DIR)/Makefile android-fonts android-terminfo
	$(MAKE) -C $(ANDROID_BUILD_DIR) aab
	@src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs -name '*.aab' -print -quit); \
	if [ -z "$$src" ]; then echo "android-aab: no .aab was produced" >&2; exit 1; fi; \
	cp "$$src" $(ANDROID_BUILD_DIR)/$(AAB_NAME); \
	echo "android-aab: $(ANDROID_BUILD_DIR)/$(AAB_NAME)  ($$(du -h $(ANDROID_BUILD_DIR)/$(AAB_NAME) | cut -f1))"
	@echo "android-aab: versionName $(VERSION), versionCode $(ANDROID_VERSION_CODE)"
	@if [ -z "$(ANDROID_KEYSTORE)" ]; then \
		echo "android-aab: UNSIGNED, which Play will not accept -- Gradle signs a" >&2; \
		echo "android-aab:   release bundle with nothing when no key is given;" >&2; \
		echo "android-aab:   keytool and jarsigner both read it so (2026-10-22)." >&2; \
		echo "android-aab:   Set ANDROID_KEYSTORE and ANDROID_KEY_ALIAS to sign it with your" >&2; \
		echo "android-aab:   upload key. Play App Signing holds the key that actually" >&2; \
		echo "android-aab:   signs what users install; this one only proves the upload" >&2; \
		echo "android-aab:   is yours, and losing it is recoverable where losing the" >&2; \
		echo "android-aab:   other once was not." >&2; \
		exit 1; \
	fi
	@# And read back as Play would, so a bundle nobody has checked is not
	@# what this leaves behind: signer, targetSdk, versionCode, alignment.
	$(MAKE) --no-print-directory android-play-check ANDROID_AAB=$(ANDROID_BUILD_DIR)/$(AAB_NAME)

# The installable artifact, under a name that says what it is (sec 12.2).
#
# `make android` leaves it at android-build/build/outputs/apk/debug/
# android-build-debug.apk, which is Gradle's name for it and tells nobody
# which app, which version or which ABI they are holding. This copies it
# out as beerssh-<version>-<abi>.apk.
#
# Signing. Set ANDROID_KEYSTORE (and ANDROID_KEY_ALIAS, plus ANDROID_KEYSTORE_PASS
# and ANDROID_KEY_PASS in the environment) for a release build signed with
# your own key. WITHOUT it the artifact is the debug-signed one, which
# installs on any device and is exactly what you want for evaluation --
# and is said out loud, because a debug-signed APK that gets mistaken for
# a release build is a bad afternoon: Play rejects it, and every install
# of it conflicts with the real one's signature.
#
# What android-install and android-run in tool/android.mk reach for, and
# the settled spelling every Android sibling uses (harmonization.md):
# <program>-<version>-<abi>.apk, in the android build directory. One
# variable rather than an APK_NAME plus the directory it lands in -- two
# halves recombined per recipe is how a name and a path drift apart.
ANDROID_PROGRAM ?= $(TARGET)
ANDROID_ARTIFACT = $(ANDROID_BUILD_DIR)/$(ANDROID_PROGRAM)-$(VERSION)-$(ANDROID_ABI).apk

android: $(ANDROID_BUILD_DIR)/Makefile $(ANDROID_ASSETS)
ifneq ($(ANDROID_KEYSTORE),)
	@# The flags go to androiddeployqt itself, because Qt's generated
	@# Makefile hardcodes its command line and accepts nothing from here.
	@# Passing them to that Makefile as a variable -- which is what this
	@# did first -- drops them silently and produces a DEBUG-SIGNED apk
	@# under a message announcing a release build. Signed with the debug
	@# key is the one packaging mistake nobody catches by looking.
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk_install_target
	@# `apk` here is QT'S generated target, not one of ours -- ours is
	@# `android` now, and this line reads Qt's Makefile to lift the
	@# androiddeployqt command out of it.
	@#
	@# Above the recipe rather than inside it, and that is not a matter of
	@# taste. A recipe line ending in a backslash CONTINUES, and make hands
	@# the joined text to the shell without looking at it -- so a `@#` on
	@# the next line is not a comment, it is an argument. This sat between
	@# `@set -e; \` and the sed below, where the apostrophe in QT'S opened a
	@# quote that nothing closed, and every release build died on
	@# "Unterminated quoted string" before androiddeployqt ever ran.
	@#
	@# It survived because the cheap check clears it: `make -n` prints this
	@# recipe expanded and `sh -n` on what it prints exits 0. The error
	@# exists only in the string make actually passes to the shell.
	@set -e; \
	deploy=$$(sed -n '/^apk: apk_install_target/,+1p' $(ANDROID_BUILD_DIR)/Makefile | tail -1); \
	if [ -z "$$deploy" ]; then echo "android: cannot find Qt's deploy command" >&2; exit 1; fi; \
	$$deploy --release --sign $(ANDROID_KEYSTORE) $(ANDROID_KEY_ALIAS)
else
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk
	@echo "android: DEBUG-SIGNED. Installs anywhere, and is not a release artifact:"
	@echo "android:   Play will reject it, and it conflicts with a real build's signature."
	@echo "android:   Set ANDROID_KEYSTORE and ANDROID_KEY_ALIAS for a signed release build."
endif
	@src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk -name '*.apk' \
	        -newer $(ANDROID_BUILD_DIR)/Makefile -print -quit); \
	if [ -z "$$src" ]; then \
		src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk -name '*.apk' -print -quit); \
	fi; \
	if [ -z "$$src" ]; then echo "android: no .apk was produced" >&2; exit 1; fi; \
	cp "$$src" $(ANDROID_ARTIFACT); \
	echo "android: $(ANDROID_ARTIFACT)  ($$(du -h $(ANDROID_ARTIFACT) | cut -f1))"
	@# And which ABI is in it, read from the package rather than trusted
	@# from the variable that named the file. The two came apart once
	@# already, and an APK whose name says arm64-v8a over x86_64 libraries
	@# installs on nothing it claims to target -- with no way to tell
	@# short of unzipping it, which is what this does.
	@if ! command -v unzip >/dev/null 2>&1; then \
		echo "android: no unzip; cannot say which ABI is inside this"; \
	else \
		abis=$$(unzip -Z1 $(ANDROID_ARTIFACT) 'lib/*' 2>/dev/null \
		        | cut -d/ -f2 | sort -u | tr '\n' ' ' | sed 's/ *$$//'); \
		if [ -z "$$abis" ]; then \
			echo "android: no native libraries in the package" >&2; \
			exit 1; \
		fi; \
		if [ "$$abis" != "$(ANDROID_ABI)" ]; then \
			echo "android: named $(ANDROID_ABI), carries $$abis" >&2; \
			exit 1; \
		fi; \
		echo "android: carries $$abis, which is what the name says"; \
	fi
	@# Who signed it, checked rather than announced. This target claimed a
	@# release build while shipping the Android debug key, and the only
	@# reason that was noticed is that somebody ran apksigner by hand.
	@#
	@# The check itself now comes from tool/android.mk. The copy that used
	@# to live here read only "Signer #1 certificate DN:", and build-tools 37
	@# prints "V2 Signer: certificate DN:" -- so on any current SDK it
	@# matched nothing, printed "signed by" with a blank after it, and the
	@# debug-key guard below it could never fire. Measured against a real
	@# package with build-tools 37.0.0: the old expression yields an empty
	@# string where the shared one yields
	@# "C=US, O=Android, CN=Android Debug". That is this project's own
	@# failure, re-armed by a tool update and invisible because an
	@# unreadable signature read as a pass.
	$(call android_verify_signature,$(ANDROID_ARTIFACT))

# beersshd's APK (sec 19), by the rules above rather than a copy of them:
# the same Makefile again, told the other program's name, id, project and
# build directory. So the signature, the ABI and the artifact name are
# checked for it by the code that checks them for BeerSSH, and a fix to
# either is a fix to both. It bundles no fonts and no terminfo.
#
# ANDROID_PROGRAM and not TARGET. A variable set on make's command line
# is passed to every sub-make, and the `make -C` into qmake's generated
# Makefile is one: TARGET=beersshd replaced the TARGET qmake had written
# there, and the library was linked as `beersshd` while the install step
# went looking for libbeersshd_x86_64.so. So every name passed below is
# one no generated Makefile uses.
SSHD_APP_ID = se.vibes.beersshd
SSHD_ANDROID = ANDROID_PROGRAM=$(SSHD_TARGET) APP_ID=$(SSHD_APP_ID) ANDROID_PRO=beersshd.pro \
               ANDROID_BUILD_DIR=build-sshd-android-$(ANDROID_ABI) ANDROID_ASSETS=

android-sshd android-sshd-install android-sshd-run android-sshd-log android-sshd-uninstall:
	$(MAKE) --no-print-directory $(patsubst android-sshd%,android%,$@) $(SSHD_ANDROID)

.PHONY: android-sshd android-sshd-install android-sshd-run android-sshd-log android-sshd-uninstall

# sec 12.2's other half for Android: is it signed by who you think, and
# does the artifact install and run?
#
# Needs a device or emulator on adb for the second half. Installs over
# whatever is there (-r), which keeps its data -- a check that wiped the
# host list would be a check nobody runs twice.
#
# The signature is verified here as well as in `apk`, and that is not
# redundancy for its own sake: this target's NAME is the one somebody
# reaches for when they want to know an artifact is sound before it goes
# anywhere, and a name that promises a check should perform it rather than
# inherit it from a prerequisite that may or may not have rerun.
check-apk: android
	$(call android_verify_signature,$(ANDROID_ARTIFACT))
	./tool/check-apk.sh $(ANDROID_ARTIFACT)

# android-run, android-install, android-log and android-uninstall come from
# tool/android.mk. The versions that were here hardcoded the Gradle output
# path and the application id as literals; the fragment reads ANDROID_ARTIFACT
# and APP_ID, which this file already states once each.

# sec 12.2's Linux baseline. The binary alone is not an installation: the
# app looks for its fonts in $(PREFIX)/share/beerssh/fonts (that is what
# bssh_platform_desktop::font_directory checks after the build tree), and
# without the .desktop entry, the AppStream metadata and an icon the app
# is invisible to every software centre and most launchers.
install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

	mkdir -p $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0644 $(MAN_PAGE) $(DESTDIR)$(PREFIX)/share/man/man1/$(MAN_PAGE)

	# beersshd, which debian/ packages apart from beerssh (sec 19).
	install -m 0755 $(SSHD_TARGET) $(DESTDIR)$(PREFIX)/bin/$(SSHD_TARGET)
	install -m 0644 $(SSHD_MAN_PAGE) $(DESTDIR)$(PREFIX)/share/man/man1/$(SSHD_MAN_PAGE)

	# Where bash-completion looks for a per-command file, and it loads it
	# on demand rather than at every shell start -- which is why the name
	# is the command's and carries no suffix.
	mkdir -p $(DESTDIR)$(PREFIX)/share/bash-completion/completions
	install -m 0644 $(COMPLETION) \
	        $(DESTDIR)$(PREFIX)/share/bash-completion/completions/$(TARGET)

	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 packaging/$(APP_ID).desktop \
	        $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop

	mkdir -p $(DESTDIR)$(PREFIX)/share/metainfo
	install -m 0644 packaging/$(APP_ID).metainfo.xml \
	        $(DESTDIR)$(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml

	mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 packaging/$(APP_ID).svg \
	        $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg

	# The fonts, if they were fetched. Not an error when they were not:
	# `make install` must work on a checkout that never ran `make fonts`,
	# and the app falls back to system families (sec 9.4).
	@if [ -d fonts ]; then \
		mkdir -p $(DESTDIR)$(PREFIX)/share/beerssh/fonts/licences; \
		for face in fonts/*.ttf fonts/*.otf; do \
			[ -f "$$face" ] || continue; \
			install -m 0644 "$$face" $(DESTDIR)$(PREFIX)/share/beerssh/fonts/; \
		done; \
		for text in fonts/licences/*; do \
			[ -f "$$text" ] || continue; \
			install -m 0644 "$$text" \
			        $(DESTDIR)$(PREFIX)/share/beerssh/fonts/licences/; \
		done; \
		echo "install: bundled $$(ls fonts/*.ttf fonts/*.otf 2>/dev/null | wc -l) font faces"; \
	else \
		echo "install: no fonts/ -- the app will use system monospace families"; \
	fi

# --- Debian packaging (project.md sec 12.2) --------------------------------
#
# Built with dpkg-buildpackage driving debian/rules, which is three lines
# of dh. The `deb` recipe below is the whole of it.
#
# This said the opposite until the comment was read against the recipe it
# sits above: dpkg-deb over a staged `make install` tree, chosen so there
# was no second build system to keep working and no pretence at upstream
# Debian packaging. That argument lost to `dh_shlibdeps`. The Depends for
# a Qt and libssh binary is not a list worth maintaining by hand, and it
# is the one thing a staged tree and dpkg-deb cannot work out -- which is
# why debian/rules says so where the dependencies would otherwise be.
#
# `make install` is still the only thing that decides what lands where;
# dh_auto_install calls it with DESTDIR, so the packages contain exactly
# what installing by hand does.
#
# TWO packages, because the fonts are 54 MB and the app is not. A terminal
# client that costs 54 MB to install is a terminal client people do not
# install; the curated set is genuinely optional (sec 9.4 -- the app falls
# back to system monospace families), so it is Recommends rather than
# Depends and lives in its own arch-independent package, which is how
# Debian ships fonts anyway.
DEB_ARCH    ?= $(shell dpkg --print-architecture)
DEB_MAINTAINER ?= $(shell git config user.name) <$(shell git config user.email)>

# Native Debian packaging: debian/ holds the metadata, debhelper does the
# work. dh_shlibdeps computes Depends from the linked binary; the previous
# hand-rolled version had to be taught that the hard way, after a package
# with an empty Depends shipped and failed at first launch rather than at
# install.
# The fonts are a prerequisite of the PACKAGE even though they are not one
# of `make install`, and the two are not in conflict. install is
# deliberately tolerant of a checkout that never ran `make fonts` -- sec
# 9.4 has the app fall back to system families -- while debian/beerssh-
# fonts.install names usr/share/beerssh outright, so the packaging is not
# tolerant at all. Without them the build does not produce a smaller set
# of packages, it produces none:
#
#     dh_install: warning: Cannot find (any matches for) "usr/share/beerssh"
#     dh_install: error: missing files, aborting
#
# and it does that at dh_install, after the whole app has been compiled.
#
# Fetching is a no-op when they are present: fetch-fonts.sh decides by
# whether every member of a family is already on disk, so it neither
# re-downloads nor re-hashes.
deb: version-check fonts
	@if ! ls fonts/*.ttf fonts/*.otf >/dev/null 2>&1; then \
		echo "deb: fonts/ holds no faces, so beerssh-fonts would be empty"; \
		echo "deb:   and dh_install would abort after building everything."; \
		echo "deb:   Fetch them first: make fonts"; \
		exit 1; \
	fi
	dpkg-buildpackage -b -us -uc
	@mkdir -p $(DEB_DIR)
	@for f in ../beerssh_$(VERSION)_*.deb ../beerssh-fonts_$(VERSION)_all.deb \
	          ../beerssh-dbgsym_$(VERSION)_*.deb \
	          ../beersshd_$(VERSION)_*.deb ../beersshd-dbgsym_$(VERSION)_*.deb \
	          ../beerssh_$(VERSION)_*.buildinfo ../beerssh_$(VERSION)_*.changes; do \
		[ -e "$$f" ] && mv -f "$$f" $(DEB_DIR)/ || true; \
	done
	@ls -1 $(DEB_DIR)/*.deb

# The VERSION file is the source; everything else is checked against it.
#
# **An input this cannot read is a failure, not a skip.** The condition here
# tested whether dpkg-parsechangelog printed anything and then announced
# "skipped (dpkg-parsechangelog unavailable)" -- a cause it never tested.
# Measured, with the tool installed: exit 25 and empty output when there is
# no debian/changelog, exit 255 when there is one nothing can parse. Both
# are indistinguishable from an absent tool at that condition, so a
# changelog a bad merge left unparseable produced a green skip naming the
# wrong reason, and `deb` went on to build a package whose version had been
# compared against nothing. That is the shape raidcfgd records at its own
# version-check, reached from the other end: there, two empty files
# compared equal.
#
# So there is no skip left. An absent dpkg-parsechangelog is handled by
# reading the file directly instead -- which is the only case the old
# message actually described -- and everything else stops the build.
#
# The fallback is NOT raidcfgd's line. That one strips a Debian revision
# with `\(.*\)-[^-]*` and so requires a hyphen; this package is native
# (`beerssh (1.0)`, no revision), and copying it across would have turned a
# false skip into a false failure on every machine without dpkg-dev.
version-check:
	@file=$$(cat VERSION 2>/dev/null); \
	if [ -z "$$file" ]; then \
		echo "version-check: VERSION is empty or unreadable;" >&2; \
		echo "version-check:   there is nothing to compare against." >&2; \
		exit 1; \
	fi; \
	changelog=$$(dpkg-parsechangelog -SVersion 2>/dev/null \
	              || sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' debian/changelog 2>/dev/null); \
	if [ -z "$$changelog" ]; then \
		echo "version-check: no version could be read from debian/changelog." >&2; \
		echo "version-check:   dpkg-parsechangelog and the direct read both" >&2; \
		echo "version-check:   came back empty, so nothing was compared." >&2; \
		echo "version-check:   what the parser makes of it:" >&2; \
		dpkg-parsechangelog -SVersion >/dev/null || true; \
		exit 1; \
	fi; \
	if [ "$$file" != "$$changelog" ]; then \
		echo "version-check: VERSION says $$file but"; \
		echo "               debian/changelog says $$changelog"; \
		echo "version-check:   add a changelog entry for $$file"; \
		exit 1; \
	fi; \
	echo "version-check: $$file, in step"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/$(MAN_PAGE)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(SSHD_TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/$(SSHD_MAN_PAGE)
	rm -f $(DESTDIR)$(PREFIX)/share/bash-completion/completions/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	rm -f $(DESTDIR)$(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg
	rm -rf $(DESTDIR)$(PREFIX)/share/beerssh

# clean removes intermediates only; $(TARGET) survives, matching the split
# every Makefile in the sibling tree uses.
# A clean always causes a rebuild. That is what it is for.
#
# It did not. $(TARGET) was missing from the list, so `make clean && make`
# built a man page and nothing else: the binary at the root is a COPY of
# $(BUILD_DIR)/$(TARGET) whose prerequisites are the SOURCES rather than
# the thing it was copied from, so removing the objects left the copy in
# place, newer than every source, and make correctly decided there was
# nothing to do. The man page rebuilt only because clean had removed it
# -- from the stale binary.
#
# The tell was inside this file. veryclean carried `rm -f $(TARGET)` and
# clean did not, so the two disagreed about whether the binary is a build
# artifact; veryclean now gets it by depending on clean, like everything
# else here.
clean:
	rm -f $(ARTIFACTS)
	$(call bssh_qmake_clean,$(BUILD_DIR))
	$(call bssh_qmake_clean,$(TEST_BUILD_DIR))
	$(call bssh_qmake_clean,$(SSHD_BUILD_DIR))
	$(call bssh_qmake_clean,$(ANDROID_BUILD_DIR))

# qmake's own clean, in a tree that has one -- skipped, and said so, when
# the tree names a project file that is gone (see bssh_qmake_orphaned):
# recursing would only fail, and would take veryclean down with it, since
# veryclean runs clean first and is the target that removes such a tree.
define bssh_qmake_clean
	@if [ -n "$(call bssh_qmake_orphaned,$(1))" ]; then \
		echo "clean: skipping $(1)/ -- its Makefile was generated from" \
		     "$(call bssh_qmake_orphaned,$(1)), which no longer exists;" \
		     "make veryclean removes it"; \
	elif [ -f $(1)/Makefile ]; then \
		$(MAKE) -C $(1) clean; \
	fi
endef

# Removes a whole directory, having first checked it is one we could
# plausibly have made.
#
# The build dirs are ours -- this Makefile created them and qmake owns
# their contents -- so removing them wholesale is a named delete rather
# than a wildcard sweep. That argument holds only for the path actually
# passed, and `rm -rf $(VAR)` is exactly how a clean target eats something
# it should not: a variable that is unset, mistyped, or overridden on the
# command line with an absolute path. `make veryclean BUILD_DIR=$$HOME`
# should not be a way to lose a home directory.
#
# So each one is checked before it goes: relative, and with no parent
# escape. An empty variable disappears in the shell's word splitting and
# removes nothing, which is the safe direction to fail in.
define bssh_remove_tree
	@for dir in $(1); do \
		case "$$dir" in \
			/*) echo "veryclean: refusing to remove the absolute path $$dir" >&2; exit 1 ;; \
			*..*) echo "veryclean: refusing to remove $$dir -- it escapes the tree" >&2; exit 1 ;; \
			.|./) echo "veryclean: refusing to remove the working directory" >&2; exit 1 ;; \
		esac; \
		rm -rf "$$dir"; \
	done
endef

# What the package build cleans with (debian/rules: dh_auto_clean runs
# this rather than distclean). Everything veryclean removes except the
# Android trees: `make deb` ran distclean, so building the desktop package
# deleted the APK and the whole Android build with it (holder, 2026-10-29:
# "make deb shouldn't wipe the android build"). Nothing in the package is
# built there, so nothing in it can be stale from there.
#
# Not by way of `clean`, which runs qmake's own clean in the Android tree
# too and would leave it to rebuild every object. The vendored sources are
# still put back and re-patched, so the package is built from exactly the
# committed patches; the Android build then recompiles only what includes
# them.
deb-clean:
	rm -f $(ARTIFACTS)
	$(call bssh_remove_tree,$(BUILD_DIR) $(TEST_BUILD_DIR) $(SSHD_BUILD_DIR) \
	        $(VTERM_BUILD_DIR) $(LIBSSH_BUILD_DIR) $(SHOT_DIR))
	@if [ -d $(VTERM_DIR)/.git ] || [ -f $(VTERM_DIR)/.git ]; then \
		git -C $(VTERM_DIR) checkout -- . && echo "vterm: patches reverted"; \
	fi
	@if [ -d $(LIBSSH_DIR)/.git ] || [ -f $(LIBSSH_DIR)/.git ]; then \
		git -C $(LIBSSH_DIR) checkout -- . && echo "libssh: patches reverted"; \
	fi
	@# The file libssh-patch/0001 creates, by name: checkout restores
	@# tracked files and leaves an untracked one, and the patch cannot be
	@# applied again over a file that is already there -- which is what
	@# `make deb` met, dh_auto_clean running this before the build.
	@rm -f $(LIBSSH_DIR)/src/transport_export.c
	rm -f .qmake.stash

# `build-android-*` beside $(ANDROID_BUILD_DIR): the variable names only
# the current ABI's tree, and which other ABIs were built is exactly the
# unknowable-names case -- the same shape .gitignore already declares as
# this build's own. Each expansion still goes through the checks above.
veryclean: clean deb-clean
	$(call bssh_remove_tree,$(ANDROID_BUILD_DIR) build-android-* build-sshd-android-* \
	        $(VTERM_ANDROID_BASE))

# `distclean` removes what the build generated and `veryclean` does not.
# `.qmake.stash` is qmake's, is named, and is disposable by construction.
#
# **It no longer sweeps the tree for editor droppings.** `*~` and `*.swp` are
# not build output: they belong to somebody's editor, and the build system
# has no business deleting files it did not create. The sweep was also
# unbounded -- `find .` from here walks `.git`, `vterm/` and
# `vterm-patch/`, and it was measured deleting files inside a submodule
# and inside `.git` itself, neither of which this project owns. `git clean
# -xdn` lists that class of file and is the person's call, not the build's.
distclean: veryclean
	rm -f .qmake.stash

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: deb-clean all run test libssh-source bench style sabotage check-ci check-ssh check-handoff check-keepalive fonts fonts-list check-fonts android-fonts terminfo android-terminfo check-build-tree uninstall \
        deb \
        android android-check android-aab android-run install clean veryclean \
        distclean help check style-tool style-source style-docs hooks version-check

# `check` is everything that must pass before committing; `test` is the suite
# alone. GNU's meaning of check, and what most of these projects already did.
check: style test

# The full sabotage run: each spec's edit applied, the suite run, the edit
# reverted. `make style` only checks the specs' anchors; this is the run
# that says the tests still catch what they claim to. Manual and long.
SPECS ?=
sabotage:
	python3 tool/sabotage.py $(SPECS)

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently on
# a fresh clone.
#
# **Where the hooks live is git's question, not the filesystem's.** This
# asked `test -d .git`, which is false in a linked worktree and in a
# submodule checkout: there `.git` is a regular FILE naming the real gitdir.
# So `make hooks` refused with "not a git repository" inside one that is.
# This tree's own vterm/ submodule is exactly that shape -- a 30-byte gitdir
# file -- so the case is reachable from a checkout of this repository, not
# only from somebody's worktree.
#
# Testing -e instead would only move the failure one line down: the installs
# write into .git/hooks/, which is not a directory there either. Asking git
# answers both halves at once, and from a worktree it returns the MAIN
# repository's hooks directory -- which is the one git actually runs.
#
# git's absence is reported as its own thing rather than as "not a git
# repository", which would be a message naming a cause nothing tested. The
# submodule fetch at the top of this file already draws the same
# distinction.
hooks:
	@if ! command -v git >/dev/null 2>&1; then \
		echo "hooks: git is not installed, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	dir=$$(git rev-parse --git-common-dir 2>/dev/null); \
	if [ -z "$$dir" ]; then \
		echo "hooks: not a git repository, so there is nowhere to install to." >&2; \
		exit 1; \
	fi; \
	mkdir -p "$$dir/hooks"; \
	install -m 0755 tool/hooks/commit-msg "$$dir/hooks/commit-msg"; \
	for old in pre-push pre-commit; do \
		if grep -q 'check-ci' "$$dir/hooks/$$old" 2>/dev/null; then \
			rm -f "$$dir/hooks/$$old"; \
			echo "hooks: removed $$dir/hooks/$$old, the retired check-ci gate (sec 12.4)"; \
		fi; \
	done; \
	echo "hooks: commit-msg installed from tool/hooks/ into $$dir/hooks/"
