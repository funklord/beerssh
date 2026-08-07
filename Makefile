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
#                        bound is in tests/main.cpp, not here, because
#                        the run that filled a disk was the binary
#                        invoked directly.
#   make bench        -- emulator throughput in MB/s (sec 9.5); built by
#                        `test` but not run by it, and not a CI gate.
#                        Refuses a machine too loaded to measure;
#                        BSSH_BENCH_ANYWAY=1 to insist
#   make shots        -- render every screen to build-shots/ for review;
#                        not a test, and it cannot fail a build (sec 12.3)
#   make aab          -- the Android App Bundle Play takes, named and with
#                        its versionCode reported; needs BSSH_KEYSTORE
#   make check-deb    -- build the .debs, then install and run them in a
#                        clean container (needs docker; sec 12.2)
#   make style        -- gate the tree's indentation (tools/check-indent.sh)
#   make check-ci     -- run the Linux CI job in the runner's own image
#                        (needs docker; sec 12.4)
#   make check-ssh    -- the SSH transport against a throwaway sshd
#                        (needs sshd; sec 12.3)
#   make check-keepalive -- dead-peer detection against a black-holed
#                        link (needs docker + NET_ADMIN; sec 5.3)
#   make fonts        -- fetch the curated font set (sec 9.2) into fonts/;
#                        not needed to build, see FONTS below
#   make fonts-list   -- show the set and its pinned versions
#   make check-fonts  -- validate the fetched fonts against sec 9.1
#   make android      -- build the Android APK (needs a Qt kit, NDK, SDK
#                        and a JDK; see ANDROID below)
#   make apk          -- the installable APK, named for its version and ABI;
#                        debug-signed unless BSSH_KEYSTORE is set (sec 12.2)
#   make check-apk    -- install that APK on an attached device and check it
#                        starts (needs adb; sec 12.2)
#   make android-aab  -- the Play bundle instead of an APK
#   make android-run  -- install and launch on the attached device
#   make install      -- install the app, its desktop entry, icon,
#                        AppStream metadata and bundled fonts
#   make uninstall    -- remove what install put there
#   make deb          -- build beerssh and beerssh-fonts Debian packages
#   make clean        -- remove compilation intermediates only; the built
#                        binary is left in place, so `make install` stays
#                        possible without a rebuild
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
#     ANDROID_NDK_ROOT  the NDK (compiles); tools/build-deps-android.sh too
#     ANDROID_SDK_ROOT  the SDK (packages); separate from the NDK
#     a JDK on PATH     Gradle will not run on a JRE
#
#   The kit decides the ABI -- qmake reads it from there and so does this
#   Makefile, so there is one answer rather than two that can disagree.
#
#   The dependencies must be cross-compiled first, for that same ABI:
#     tools/build-deps-android.sh arm64-v8a
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

BUILD_DIR         = build
ANDROID_BUILD_DIR = build-android

PREFIX  ?= /usr/local
DESTDIR ?=

# Section 1, because this is a user command. Generated -- see the rule
# under `all`, and project.md sec 8.6.
MAN_PAGE = $(TARGET).1

SOURCES = $(wildcard src/*.cpp src/*/*.cpp)
HEADERS = $(wildcard src/*.h src/*/*.h)

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
VTERM_SRCS = $(wildcard $(VTERM_DIR)/src/*.c)
VTERM_OBJS = $(patsubst $(VTERM_DIR)/src/%.c,$(VTERM_BUILD_DIR)/%.o,$(VTERM_SRCS))
VTERM_BUILD_DIR = build-vterm
VTERM_LIB  = $(VTERM_BUILD_DIR)/libvterm.a

# Patches applied to the vendored copy at build time (vterm-patches/), kept
# as files rather than as edits committed into the submodule: a submodule
# pinned to a commit that exists only locally is a submodule nobody else can
# clone. Applying them here means an upstream update that conflicts fails
# loudly at the patch step instead of silently dropping the fix.
VTERM_PATCHES = $(sort $(wildcard vterm-patches/*.patch))
VTERM_STAMP   = $(VTERM_BUILD_DIR)/.patched

# -w: this is third-party code built to someone else's warning standard, and
# a wall of warnings nobody will act on is a wall that hides ours.
VTERM_CFLAGS = -std=c99 -Os -w -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src

# --- vendored libvterm, cross-compiled ------------------------------------
# The host archive cannot be reused: it is x86_64, and the first Android
# link said so in nine identical lines. One build per ABI, in its own
# directory, because "the library" stops being one artifact the moment
# there is more than one target.
# The ABI is the Qt KIT's, and is read from it rather than chosen again.
#
# beerssh.pro takes QT_ARCH from the kit, and everything real follows from
# that: the dependency directory, the vterm archive it links, and the list
# of Qt libraries deployed into the package. This variable used to default
# to arm64-v8a independently, so the two could disagree and nothing said
# so -- building against the x86_64 kit produced an APK named
# beerssh-<version>-arm64-v8a.apk with lib/x86_64 inside it, built the
# vterm archive into a directory the .pro never looks in, and checked the
# dependencies of an ABI it was not building. Only the last two fail
# loudly. The name is the one that ships, and the one nobody can check by
# looking at it.
#
# qdevice.pri is where qmake gets the answer itself, so reading that file
# is the same answer rather than a second one. Setting ANDROID_ABI by hand
# still works, and android-check refuses it if it contradicts the kit.
ifdef QT_ANDROID_ROOT
ANDROID_KIT_ABI := $(shell sed -n 's/^DEFAULT_ANDROID_ABIS *= *//p' \
                            $(QT_ANDROID_ROOT)/mkspecs/qdevice.pri 2>/dev/null)

# Second best: the kit's own directory name, which the Qt installer spells
# android_arm64_v8a, android_armv7, android_x86 and android_x86_64.
#
# Here because making an unreadable qdevice.pri a hard ERROR would break a
# build that used to work -- CI installs its kit with an action rather than
# from this machine, and a layout that differs there would stop the Android
# job dead. This is a fallback and not a second opinion: it is consulted
# only when the authoritative file says nothing, and the disagreement check
# below still applies to whatever comes out.
ifeq ($(ANDROID_KIT_ABI),)
ANDROID_KIT_ABI := $(patsubst android_%,%,$(notdir $(patsubst %/,%,$(QT_ANDROID_ROOT))))
ANDROID_KIT_ABI := $(subst arm64_v8a,arm64-v8a,$(ANDROID_KIT_ABI))
ANDROID_KIT_ABI := $(subst armv7,armeabi-v7a,$(ANDROID_KIT_ABI))
ANDROID_KIT_ABI := $(filter arm64-v8a armeabi-v7a x86 x86_64,$(ANDROID_KIT_ABI))
endif
endif
ANDROID_ABI ?= $(ANDROID_KIT_ABI)
ANDROID_API ?= 26

# The Play versionCode, computed from VERSION rather than written twice.
#
# Play accepts an upload only if this is strictly greater than the last
# one it took, so the number that was hardcoded in beerssh.pro would have
# allowed exactly one upload and rejected every update after it. Derived
# as major*10000 + minor*100 + patch: 0.1.0 is 100, 1.2.3 is 10203, and it
# rises for as long as the version does.
#
# Override it to re-upload the SAME version after Play has already taken
# one -- which is what a rejected release leaves you needing:
#   make aab ANDROID_VERSION_CODE=101
ANDROID_VERSION_CODE ?= $(shell echo $(VERSION) | awk -F. \
        '{printf "%d", ($$1 * 10000) + ($$2 * 100) + $$3}')

ifeq ($(ANDROID_ABI),arm64-v8a)
    ANDROID_TRIPLE = aarch64-linux-android
else ifeq ($(ANDROID_ABI),armeabi-v7a)
    ANDROID_TRIPLE = armv7a-linux-androideabi
else ifeq ($(ANDROID_ABI),x86_64)
    ANDROID_TRIPLE = x86_64-linux-android
else
    ANDROID_TRIPLE = i686-linux-android
endif

NDK_BIN = $(ANDROID_NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin
VTERM_ANDROID_DIR  = build-vterm-android/$(ANDROID_ABI)
VTERM_ANDROID_LIB  = $(VTERM_ANDROID_DIR)/libvterm.a
VTERM_ANDROID_OBJS = $(patsubst $(VTERM_DIR)/src/%.c,$(VTERM_ANDROID_DIR)/%.o,$(VTERM_SRCS))

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

all: $(TARGET) $(MAN_PAGE)

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

$(VTERM_STAMP): $(VTERM_PATCHES)
	@mkdir -p $(VTERM_BUILD_DIR)
	@echo "applying $(words $(VTERM_PATCHES)) patch(es) to vendored libvterm"
	@for patch in $(VTERM_PATCHES); do \
		if ! git -C $(VTERM_DIR) apply --check "$(CURDIR)/$$patch" 2>/dev/null; then \
			if git -C $(VTERM_DIR) apply --reverse --check "$(CURDIR)/$$patch" 2>/dev/null; then \
				echo "  $$patch: already applied"; \
				continue; \
			fi; \
			echo "  $$patch: DOES NOT APPLY -- upstream changed under it."; \
			echo "  Rebase the patch against the current submodule before building."; \
			exit 1; \
		fi; \
		git -C $(VTERM_DIR) apply "$(CURDIR)/$$patch" || exit 1; \
		echo "  $$patch: applied"; \
	done
	@touch $@

$(VTERM_BUILD_DIR)/%.o: $(VTERM_DIR)/src/%.c $(VTERM_STAMP)
	@mkdir -p $(VTERM_BUILD_DIR)
	$(CC) $(VTERM_CFLAGS) -c $< -o $@

$(VTERM_LIB): $(VTERM_OBJS)
	$(AR) rcs $@ $(VTERM_OBJS)

# A missing submodule is the one failure worth catching by hand: without
# this, make reports "no rule to make target vterm/src/vterm.c" at whoever
# forgot --recurse-submodules, which names the symptom and not the fix.
$(VTERM_SRCS):
	@if [ ! -f $(VTERM_DIR)/src/vterm.c ]; then \
		echo "make: the vterm submodule is not checked out."; \
		echo "make:   run: git submodule update --init"; \
		exit 1; \
	fi

$(BUILD_DIR)/Makefile: beerssh.pro
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && $(QMAKE) ../beerssh.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX) \
	        BSSH_VERSION=$(VERSION)

$(TARGET): $(VTERM_LIB) $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
	$(MAKE) -C $(BUILD_DIR)
	cp $(BUILD_DIR)/$(TARGET) $(TARGET)

run: $(TARGET)
	./$(TARGET)

# Built in its own directory from its own .pro: the test binary needs the
# app's sources but not src/main.cpp, or it would have two main()s.
TEST_BUILD_DIR = build-tests
TEST_BIN       = $(TEST_BUILD_DIR)/beerssh_test

$(TEST_BUILD_DIR)/Makefile: tests/tests.pro
	mkdir -p $(TEST_BUILD_DIR)
	cd $(TEST_BUILD_DIR) && $(QMAKE) ../tests/tests.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX)

$(TEST_BIN): $(VTERM_LIB) $(TEST_BUILD_DIR)/Makefile $(SOURCES) $(HEADERS) $(wildcard tests/*.cpp tests/*.h)
	$(MAKE) -C $(TEST_BUILD_DIR)

# The offscreen QPA plugin is what makes both of these runnable in CI at
# all -- a GUI that only starts with a display is a GUI CI never exercises.
# The smoke test starts the REAL app, which owns a real profile store and
# writes a real host list -- into ~/.ssh now (sec 10.6). A build step does
# not get to edit the config of whoever ran it, so it runs against a
# throwaway HOME. The unit tests sandbox themselves for the same reason;
# see tests/main.cpp.
test: $(TARGET) $(TEST_BIN)
	@sandbox=$$(mktemp -d) && \
	 HOME=$$sandbox XDG_CONFIG_HOME=$$sandbox/config XDG_DATA_HOME=$$sandbox/data \
	 QT_QPA_PLATFORM=offscreen ./$(TARGET) --self-test; \
	 status=$$?; rm -rf $$sandbox; exit $$status
	@echo "test: headless smoke test passed"
	@# Room to run in, checked before starting rather than discovered as
	@# sixteen assertion failures.
	@#
	@# The suite sandboxes HOME and XDG_* into a temporary directory
	@# (tests/main.cpp) so it cannot write into whoever ran it. On a full
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
# DEB_DIR is passed rather than left to the script's default, which was
# `build-deb` and has been wrong since the packaging moved to debhelper:
# `make deb` puts its output in $(BUILD_DIR), so the check refused to run
# with "no packages in build-deb. Run: make deb". It failed loudly rather
# than passing vacuously, which is the right direction -- but the message
# reads as user error, so nobody read it as a gate that had stopped
# running.
check-deb: deb
	DEB_DIR=$(BUILD_DIR) ./tools/check-deb.sh

style: style-source style-docs

style-source:
	python3 tools/style_gate.py check

# project.md is authoritative, so it is held to the tree: a heading
# that appears twice means whichever one you find, the other is the
# one with the answer.
style-docs:
	python3 tools/style_gate.py docs

# sec 12.4's runner-parity check. Needs docker; not part of `test`, and
# deliberately not a CI step -- CI already IS the runner. Run it before
# pushing anything that touches a platform or library API, because the
# runner's Qt and libssh are older than any development machine's and an
# API added later compiles here and fails there.
check-ci:
	./tools/check-ci.sh

# sec 12.3's one test against a real server. Needs sshd; not part of
# `test`, which runs entirely against the fake transport. The libssh
# binding is the only layer a fake cannot exercise.
check-ssh: $(TEST_BIN)
	./tools/check-ssh.sh

# sec 5.3's dead-peer detection against a link whose packets vanish.
# Needs docker and NET_ADMIN -- a closed socket is not a dropped link, and
# only iptables can make packets disappear rather than be refused. Slow by
# construction: the budget it verifies is about a minute.
check-keepalive:
	./tools/check-keepalive.sh

# --- the curated font set (project.md sec 9) --------------------------------
# Not a build dependency: the app runs without it, falling back to whatever
# monospace families the system has (sec 9.4). Making `all` depend on a
# 300 MB download would be a rude first build for a change that touches one
# line of the emulator.
fonts:
	./tools/fetch-fonts.sh

fonts-list:
	@./tools/fetch-fonts.sh --list

# sec 9.6's gate. Separate from `style` because it needs the fonts present
# and fontTools installed, neither of which a plain checkout has.
check-fonts:
	@python3 tools/check-fonts.py $(FONT_DIR)

$(VTERM_ANDROID_DIR)/%.o: $(VTERM_DIR)/src/%.c $(VTERM_STAMP)
	@mkdir -p $(VTERM_ANDROID_DIR)
	$(NDK_BIN)/clang --target=$(ANDROID_TRIPLE)$(ANDROID_API) $(VTERM_CFLAGS) -c $< -o $@

$(VTERM_ANDROID_LIB): $(VTERM_ANDROID_OBJS)
	$(NDK_BIN)/llvm-ar rcs $@ $(VTERM_ANDROID_OBJS)

# Everything the Android build needs, checked by name. Each of these fails
# unhelpfully when missing: a bare qmake error, a wall of missing symbols,
# a Gradle stack trace about JAVA_COMPILER, or a "platforms does not
# exist" from a path nobody set.
android-check:
	@if [ -z "$(QT_ANDROID_ROOT)" ]; then \
		echo "make android: QT_ANDROID_ROOT is not set."; \
		echo "make android:   point it at a Qt-for-Android kit, e.g."; \
		echo "make android:   make android QT_ANDROID_ROOT=\$$HOME/Qt/6.11.1/android_arm64_v8a"; \
		exit 1; \
	fi
	@if [ -z "$(ANDROID_ABI)" ]; then \
		echo "make android: cannot tell which ABI this kit builds."; \
		echo "make android:   Neither $(QT_ANDROID_ROOT)/mkspecs/qdevice.pri"; \
		echo "make android:   (no DEFAULT_ANDROID_ABIS line) nor the kit's own"; \
		echo "make android:   directory name says. Is that a Qt-for-Android"; \
		echo "make android:   kit? Otherwise name the ABI yourself:"; \
		echo "make android:   make apk ANDROID_ABI=arm64-v8a"; \
		exit 1; \
	fi
	@if [ -n "$(ANDROID_KIT_ABI)" ] && [ "$(ANDROID_ABI)" != "$(ANDROID_KIT_ABI)" ]; then \
		echo "make android: ANDROID_ABI is $(ANDROID_ABI), the kit builds $(ANDROID_KIT_ABI)."; \
		echo "make android:   qmake follows the kit, so building on would name"; \
		echo "make android:   the artifact after an ABI it does not contain."; \
		echo "make android:   Point QT_ANDROID_ROOT at the $(ANDROID_ABI) kit, or"; \
		echo "make android:   drop ANDROID_ABI and let the kit say."; \
		exit 1; \
	fi
	@if [ -z "$(ANDROID_NDK_ROOT)" ]; then \
		echo "make android: ANDROID_NDK_ROOT is not set (needed to compile)."; \
		exit 1; \
	fi
	@if [ -z "$(ANDROID_SDK_ROOT)" ] && [ ! -d "$$HOME/Android/Sdk" ]; then \
		echo "make android: no Android SDK found. Set ANDROID_SDK_ROOT."; \
		echo "make android:   the SDK is SEPARATE from the NDK -- Gradle needs"; \
		echo "make android:   platform-tools, a platform, and build-tools."; \
		exit 1; \
	fi
	@if ! command -v javac >/dev/null 2>&1 && [ -z "$(JAVA_HOME)" ]; then \
		echo "make android: no javac on PATH and JAVA_HOME is unset."; \
		echo "make android:   Gradle needs a JDK; a JRE alone will not do, and"; \
		echo "make android:   it says so only as a toolchain capability error."; \
		exit 1; \
	fi
	@if [ ! -f deps/android/$(ANDROID_ABI)/lib/libssh.a ]; then \
		echo "make android: dependencies for $(ANDROID_ABI) are not built."; \
		echo "make android:   run: tools/build-deps-android.sh $(ANDROID_ABI)"; \
		exit 1; \
	fi

$(ANDROID_BUILD_DIR)/Makefile: android-check $(VTERM_ANDROID_LIB) beerssh.pro
	mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && $(QT_ANDROID_ROOT)/bin/qmake ../beerssh.pro $(QMAKE_CONFIG) \
	        BSSH_VERSION=$(VERSION) BSSH_VERSION_CODE=$(ANDROID_VERSION_CODE)

# The base font set that ships inside the APK (project.md sec 9.4).
#
# Three families, not eight: the default, the box-drawing fallback, and one
# alternative. Everything else is an asset pack, because the full set is
# 54 MB and Iosevka is 42 MB of that on its own -- the condensed face that
# would otherwise be the phone default (sec 9.2) cannot ship in a base
# download and has to arrive later.
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
# meet. Everything else stays an asset pack: the full set is 374 MB and
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
android: $(ANDROID_BUILD_DIR)/Makefile android-fonts
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk

android-aab: $(ANDROID_BUILD_DIR)/Makefile android-fonts
	$(MAKE) -C $(ANDROID_BUILD_DIR) aab

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
AAB_NAME = beerssh-$(VERSION).aab

aab: android-aab
	@src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs -name '*.aab' -print -quit); \
	if [ -z "$$src" ]; then echo "aab: no .aab was produced" >&2; exit 1; fi; \
	cp "$$src" $(ANDROID_BUILD_DIR)/$(AAB_NAME); \
	echo "aab: $(ANDROID_BUILD_DIR)/$(AAB_NAME)  ($$(du -h $(ANDROID_BUILD_DIR)/$(AAB_NAME) | cut -f1))"
	@echo "aab: versionName $(VERSION), versionCode $(ANDROID_VERSION_CODE)"
	@if [ -z "$(BSSH_KEYSTORE)" ]; then \
		echo "aab: DEBUG-SIGNED, which Play will not accept." >&2; \
		echo "aab:   Set BSSH_KEYSTORE and BSSH_KEY_ALIAS to sign it with your" >&2; \
		echo "aab:   upload key. Play App Signing holds the key that actually" >&2; \
		echo "aab:   signs what users install; this one only proves the upload" >&2; \
		echo "aab:   is yours, and losing it is recoverable where losing the" >&2; \
		echo "aab:   other once was not." >&2; \
		exit 1; \
	fi

# The installable artifact, under a name that says what it is (sec 12.2).
#
# `make android` leaves it at android-build/build/outputs/apk/debug/
# android-build-debug.apk, which is Gradle's name for it and tells nobody
# which app, which version or which ABI they are holding. This copies it
# out as beerssh-<version>-<abi>.apk.
#
# Signing. Set BSSH_KEYSTORE (and BSSH_KEY_ALIAS, plus BSSH_KEYSTORE_PASS
# and BSSH_KEY_PASS in the environment) for a release build signed with
# your own key. WITHOUT it the artifact is the debug-signed one, which
# installs on any device and is exactly what you want for evaluation --
# and is said out loud, because a debug-signed APK that gets mistaken for
# a release build is a bad afternoon: Play rejects it, and every install
# of it conflicts with the real one's signature.
APK_NAME = beerssh-$(VERSION)-$(ANDROID_ABI).apk

apk: $(ANDROID_BUILD_DIR)/Makefile android-fonts
ifneq ($(BSSH_KEYSTORE),)
	@# The flags go to androiddeployqt itself, because Qt's generated
	@# Makefile hardcodes its command line and accepts nothing from here.
	@# Passing them to that Makefile as a variable -- which is what this
	@# did first -- drops them silently and produces a DEBUG-SIGNED apk
	@# under a message announcing a release build. Signed with the debug
	@# key is the one packaging mistake nobody catches by looking.
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk_install_target
	@set -e; \
	deploy=$$(sed -n '/^apk: apk_install_target/,+1p' $(ANDROID_BUILD_DIR)/Makefile | tail -1); \
	if [ -z "$$deploy" ]; then echo "apk: cannot find Qt's deploy command" >&2; exit 1; fi; \
	$$deploy --release --sign $(BSSH_KEYSTORE) $(BSSH_KEY_ALIAS)
else
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk
	@echo "apk: DEBUG-SIGNED. Installs anywhere, and is not a release artifact:"
	@echo "apk:   Play will reject it, and it conflicts with a real build's signature."
	@echo "apk:   Set BSSH_KEYSTORE and BSSH_KEY_ALIAS for a signed release build."
endif
	@src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk -name '*.apk' \
	        -newer $(ANDROID_BUILD_DIR)/Makefile -print -quit); \
	if [ -z "$$src" ]; then \
		src=$$(find $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk -name '*.apk' -print -quit); \
	fi; \
	if [ -z "$$src" ]; then echo "apk: no .apk was produced" >&2; exit 1; fi; \
	cp "$$src" $(ANDROID_BUILD_DIR)/$(APK_NAME); \
	echo "apk: $(ANDROID_BUILD_DIR)/$(APK_NAME)  ($$(du -h $(ANDROID_BUILD_DIR)/$(APK_NAME) | cut -f1))"
	@# And which ABI is in it, read from the package rather than trusted
	@# from the variable that named the file. The two came apart once
	@# already, and an APK whose name says arm64-v8a over x86_64 libraries
	@# installs on nothing it claims to target -- with no way to tell
	@# short of unzipping it, which is what this does.
	@if ! command -v unzip >/dev/null 2>&1; then \
		echo "apk: no unzip; cannot say which ABI is inside this"; \
	else \
		abis=$$(unzip -Z1 $(ANDROID_BUILD_DIR)/$(APK_NAME) 'lib/*' 2>/dev/null \
		        | cut -d/ -f2 | sort -u | tr '\n' ' ' | sed 's/ *$$//'); \
		if [ -z "$$abis" ]; then \
			echo "apk: no native libraries in the package" >&2; \
			exit 1; \
		fi; \
		if [ "$$abis" != "$(ANDROID_ABI)" ]; then \
			echo "apk: named $(ANDROID_ABI), carries $$abis" >&2; \
			exit 1; \
		fi; \
		echo "apk: carries $$abis, which is what the name says"; \
	fi
	@# Who signed it, checked rather than announced. This target claimed a
	@# release build while shipping the Android debug key, and the only
	@# reason that was noticed is that somebody ran apksigner by hand.
	@signer=$$(ls $(ANDROID_SDK_ROOT)/build-tools/*/apksigner 2>/dev/null | tail -1); \
	if [ -z "$$signer" ]; then \
		echo "apk: no apksigner in the SDK; cannot say who signed this"; \
	else \
		dn=$$($$signer verify --print-certs $(ANDROID_BUILD_DIR)/$(APK_NAME) 2>/dev/null \
		      | sed -n 's/^Signer #1 certificate DN: //p'); \
		echo "apk: signed by $$dn"; \
		if [ -n "$(BSSH_KEYSTORE)" ] && echo "$$dn" | grep -q "Android Debug"; then \
			echo "apk: a keystore was given but the artifact is DEBUG-signed" >&2; \
			exit 1; \
		fi; \
	fi

# sec 12.2's other half for Android: does the artifact install and run?
# Needs a device or emulator on adb. Installs over whatever is there
# (-r), which keeps its data -- a check that wiped the host list would be
# a check nobody runs twice.
check-apk: apk
	./tools/check-apk.sh $(ANDROID_BUILD_DIR)/$(APK_NAME)

# Installs onto whatever single device adb sees, and launches it.
android-run: android
	$(ANDROID_SDK_ROOT)/platform-tools/adb install -r \
	        $(ANDROID_BUILD_DIR)/android-build/build/outputs/apk/debug/android-build-debug.apk
	$(ANDROID_SDK_ROOT)/platform-tools/adb shell am start -n \
	        se.vibes.beerssh/org.qtproject.qt.android.bindings.QtActivity

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
# Built with dpkg-deb from a staged install rather than through
# debhelper: this repo's convention is plain Makefiles, `make install`
# already lays down exactly the right tree, and dpkg-deb turns that tree
# into a package without a second build system to keep working. Proper
# debian/ packaging for upstream Debian is a different job with different
# rules, and this does not pretend to be it.
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
deb: version-check
	dpkg-buildpackage -b -us -uc
	@mkdir -p $(BUILD_DIR)
	@for f in ../beerssh_$(VERSION)_*.deb ../beerssh-fonts_$(VERSION)_all.deb \
	          ../beerssh_$(VERSION)_*.buildinfo ../beerssh_$(VERSION)_*.changes; do \
		[ -e "$$f" ] && mv -f "$$f" $(BUILD_DIR)/ || true; \
	done
	@ls -1 $(BUILD_DIR)/*.deb

# The VERSION file is the source; everything else is checked against it.
version-check:
	@file=$$(cat VERSION); \
	changelog=$$(dpkg-parsechangelog -SVersion 2>/dev/null); \
	if [ -z "$$changelog" ]; then \
		echo "version-check: skipped (dpkg-parsechangelog unavailable)"; \
	elif [ "$$file" != "$$changelog" ]; then \
		echo "version-check: VERSION says $$file but"; \
		echo "               debian/changelog says $$changelog"; \
		echo "version-check:   add a changelog entry for $$file"; \
		exit 1; \
	else \
		echo "version-check: $$file, in step"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/$(MAN_PAGE)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	rm -f $(DESTDIR)$(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg
	rm -rf $(DESTDIR)$(PREFIX)/share/beerssh

# clean removes intermediates only; $(TARGET) survives, matching the split
# every Makefile in the sibling tree uses.
clean:
	rm -f $(MAN_PAGE)
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi
	@if [ -f $(TEST_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(TEST_BUILD_DIR) clean; fi
	@if [ -f $(ANDROID_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(ANDROID_BUILD_DIR) clean; fi

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

veryclean: clean
	$(call bssh_remove_tree,$(BUILD_DIR) $(ANDROID_BUILD_DIR) $(TEST_BUILD_DIR) \
	        $(VTERM_BUILD_DIR) build-vterm-android $(SHOT_DIR))
	@if [ -d $(VTERM_DIR)/.git ] || [ -f $(VTERM_DIR)/.git ]; then \
		git -C $(VTERM_DIR) checkout -- . && echo "vterm: patches reverted"; \
	fi
	rm -f $(TARGET)

distclean: veryclean
	rm -f .qmake.stash
	find . -name '*~' -o -name '*.swp' | xargs -r rm -f

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: all run test bench style check-ci check-ssh check-keepalive fonts fonts-list check-fonts android-fonts uninstall \
        deb \
        android android-check android-aab aab android-run install clean veryclean \
        distclean help check style-source style-docs hooks version-check

# `check` is everything that must pass before committing; `test` is the suite
# alone. GNU's meaning of check, and what most of these projects already did.
check: style test

# The commit-msg hook lives in the tree so it is reviewable, survives a
# clone, and can be kept in sync. .git/hooks is untracked, so a hook that
# exists only there enforces a rule nobody can see and vanishes silently on
# a fresh clone.
hooks:
	@test -d .git || { echo "hooks: not a git repository" >&2; exit 1; }
	@install -m 0755 tools/hooks/commit-msg .git/hooks/commit-msg
	@echo "hooks: commit-msg installed from tools/hooks/"
