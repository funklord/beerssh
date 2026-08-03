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
#   make test         -- build and run the smoke test + unit tests
#   make bench        -- emulator throughput in MB/s (sec 9.5); built by
#                        `test` but not run by it, and not a CI gate
#   make style        -- gate the tree's indentation (tools/check-indent.sh)
#   make check-ci     -- run the Linux CI job in the runner's own image
#                        (needs docker; sec 12.4)
#   make fonts        -- fetch the curated font set (sec 9.2) into fonts/;
#                        not needed to build, see FONTS below
#   make fonts-list   -- show the set and its pinned versions
#   make check-fonts  -- validate the fetched fonts against sec 9.1
#   make android      -- build the Android APK (needs a Qt kit, NDK, SDK
#                        and a JDK; see ANDROID below)
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
#   The dependencies must be cross-compiled first:
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
VERSION ?= 0.1.0

BUILD_DIR         = build
ANDROID_BUILD_DIR = build-android

PREFIX  ?= /usr/local
DESTDIR ?=

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
VTERM_CFLAGS = -std=c99 -O2 -w -I$(VTERM_DIR)/include -I$(VTERM_DIR)/src

# --- vendored libvterm, cross-compiled ------------------------------------
# The host archive cannot be reused: it is x86_64, and the first Android
# link said so in nine identical lines. One build per ABI, in its own
# directory, because "the library" stops being one artifact the moment
# there is more than one target.
ANDROID_ABI ?= arm64-v8a
ANDROID_API ?= 26

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

all: $(TARGET)

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
test: $(TARGET) $(TEST_BIN)
	QT_QPA_PLATFORM=offscreen ./$(TARGET) --self-test
	@echo "test: headless smoke test passed"
	QT_QPA_PLATFORM=offscreen ./$(TEST_BIN)

# Throughput through the emulator, in MB/s (project.md sec 9.5, sec 12.3).
# Built by `test` but not run by it: these take seconds and their numbers
# move with whatever else the machine is doing, which is not a thing to
# fail a build over. Run on an idle machine, and compare a run against
# another run of the same binary rather than against a number in a doc.
bench: $(TEST_BIN)
	@echo "bench: measuring on this machine -- load average is part of the result"
	@uptime
	QT_QPA_PLATFORM=offscreen ./$(TEST_BIN) --bench

style:
	./tools/check-indent.sh

# sec 12.4's runner-parity check. Needs docker; not part of `test`, and
# deliberately not a CI step -- CI already IS the runner. Run it before
# pushing anything that touches a platform or library API, because the
# runner's Qt and libssh are older than any development machine's and an
# API added later compiles here and fails there.
check-ci:
	./tools/check-ci.sh

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
	        BSSH_VERSION=$(VERSION)

# The base font set that ships inside the APK (project.md sec 9.4).
#
# Three families, not eight: the default, the box-drawing fallback, and one
# alternative. Everything else is an asset pack, because the full set is
# 54 MB and Iosevka is 42 MB of that on its own -- the condensed face that
# would otherwise be the phone default (sec 9.2) cannot ship in a base
# download and has to arrive later.
#
# Noto is in the base set despite being fallback-only: it is what stops CJK
# rendering as tofu, and a terminal that cannot draw the output of a
# remote `ls` in a Japanese directory is broken in a way no picker fixes.
# Unifont is not -- 4.9 MB to cover what Noto already covers, for scripts
# a phone will not meet.
ANDROID_BASE_FONTS = \
	JetBrainsMono-Regular.ttf JetBrainsMono-Bold.ttf \
	JetBrainsMono-Italic.ttf JetBrainsMono-BoldItalic.ttf \
	DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf \
	DejaVuSansMono-Oblique.ttf DejaVuSansMono-BoldOblique.ttf \
	Hack-Regular.ttf Hack-Bold.ttf Hack-Italic.ttf Hack-BoldItalic.ttf \
	NotoSansMono-Regular.ttf

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
	      $(ANDROID_ASSET_DIR)/licences/ 2>/dev/null || true; \
	echo "android-fonts: bundled $$(du -sh $(ANDROID_ASSET_DIR) | cut -f1) of fonts"

# The APK is what gets installed on a device or emulator, so it is the
# default. The AAB is what Play wants (sec 2) and is its own target.
android: $(ANDROID_BUILD_DIR)/Makefile android-fonts
	$(MAKE) -C $(ANDROID_BUILD_DIR) apk

android-aab: $(ANDROID_BUILD_DIR)/Makefile android-fonts
	$(MAKE) -C $(ANDROID_BUILD_DIR) aab

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
DEB_DIR      = build-deb
DEB_ARCH    ?= $(shell dpkg --print-architecture)
DEB_MAINTAINER ?= $(shell git config user.name) <$(shell git config user.email)>

deb: all
	rm -rf $(DEB_DIR)
	$(MAKE) install DESTDIR=$(DEB_DIR)/beerssh PREFIX=/usr

	# The fonts move to their own package; the app package keeps none.
	@if [ -d $(DEB_DIR)/beerssh/usr/share/beerssh ]; then \
		mkdir -p $(DEB_DIR)/beerssh-fonts/usr/share; \
		mv $(DEB_DIR)/beerssh/usr/share/beerssh \
		   $(DEB_DIR)/beerssh-fonts/usr/share/beerssh; \
	fi

	mkdir -p $(DEB_DIR)/beerssh/DEBIAN
	@printf 'Package: beerssh\n' > $(DEB_DIR)/beerssh/DEBIAN/control
	@printf 'Version: %s\n' "$(VERSION)" >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf 'Architecture: %s\n' "$(DEB_ARCH)" >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf 'Maintainer: %s\n' "$(DEB_MAINTAINER)" >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf 'Section: net\nPriority: optional\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	# Dependencies from the binary itself rather than a hand-kept list,
	# which would be a list that drifts every time a library is linked or
	# dropped. dpkg-shlibdeps insists on a debian/control relative to the
	# working directory, so it gets a minimal one; its output is FATAL if
	# empty, because a package with no Depends installs happily onto a
	# machine with no Qt and fails at the first launch with a linker
	# error. That is exactly what shipped when this ran with its stderr
	# sent to /dev/null.
	@mkdir -p $(DEB_DIR)/beerssh/debian
	@printf 'Source: beerssh\n\nPackage: beerssh\nArchitecture: %s\nDescription: x\n' \
	        "$(DEB_ARCH)" > $(DEB_DIR)/beerssh/debian/control
	@set -e; \
	depends=$$(cd $(DEB_DIR)/beerssh && \
	        dpkg-shlibdeps -O --ignore-missing-info usr/bin/beerssh 2>/dev/null \
	        | sed 's/^shlibs:Depends=//'); \
	if [ -z "$$depends" ]; then \
		echo "deb: dpkg-shlibdeps produced no dependencies; refusing to build" >&2; \
		echo "deb:   a package with no Depends fails at launch, not at install" >&2; \
		exit 1; \
	fi; \
	printf 'Depends: %s\n' "$$depends" >> $(DEB_DIR)/beerssh/DEBIAN/control
	@rm -rf $(DEB_DIR)/beerssh/debian
	@printf 'Recommends: beerssh-fonts\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf 'Description: SSH terminal for desktop and Android\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf ' An SSH client with a real terminal emulator, built from one\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf ' codebase for the desktop and for Android. Sessions live apart\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf ' from the windows showing them, so tabs, split panes and a phone\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	@printf ' rotating do not disturb a connection.\n' >> $(DEB_DIR)/beerssh/DEBIAN/control
	dpkg-deb --build --root-owner-group $(DEB_DIR)/beerssh \
	        $(DEB_DIR)/beerssh_$(VERSION)_$(DEB_ARCH).deb

	@if [ -d $(DEB_DIR)/beerssh-fonts ]; then \
		mkdir -p $(DEB_DIR)/beerssh-fonts/DEBIAN; \
		printf 'Package: beerssh-fonts\n' > $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Version: %s\n' "$(VERSION)" >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Architecture: all\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Maintainer: %s\n' "$(DEB_MAINTAINER)" >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Section: fonts\nPriority: optional\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Depends: beerssh (>= %s)\n' "$(VERSION)" >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf 'Description: curated terminal fonts for BeerSSH\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf ' The terminal fonts BeerSSH ships with, each vetted for the\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf ' properties a character grid needs, with their licence texts.\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		printf ' Without this package BeerSSH uses the system monospace fonts.\n' >> $(DEB_DIR)/beerssh-fonts/DEBIAN/control; \
		dpkg-deb --build --root-owner-group $(DEB_DIR)/beerssh-fonts \
		        $(DEB_DIR)/beerssh-fonts_$(VERSION)_all.deb; \
	fi

	@ls -lh $(DEB_DIR)/*.deb | awk '{print "  " $$9 "  " $$5}'

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	rm -f $(DESTDIR)$(PREFIX)/share/metainfo/$(APP_ID).metainfo.xml
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg
	rm -rf $(DESTDIR)$(PREFIX)/share/beerssh

# clean removes intermediates only; $(TARGET) survives, matching the split
# every Makefile in the sibling tree uses.
clean:
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi
	@if [ -f $(TEST_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(TEST_BUILD_DIR) clean; fi
	@if [ -f $(ANDROID_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(ANDROID_BUILD_DIR) clean; fi

# The build dirs are ours: this Makefile created them and qmake owns their
# contents, so removing them is a named delete, not a wildcard sweep.
veryclean: clean
	rm -rf $(BUILD_DIR) $(ANDROID_BUILD_DIR) $(TEST_BUILD_DIR) $(VTERM_BUILD_DIR) \
	        build-vterm-android
	@if [ -d $(VTERM_DIR)/.git ] || [ -f $(VTERM_DIR)/.git ]; then \
		git -C $(VTERM_DIR) checkout -- . && echo "vterm: patches reverted"; \
	fi
	rm -f $(TARGET)

distclean: veryclean
	rm -f .qmake.stash
	find . -name '*~' -o -name '*.swp' | xargs -r rm -f

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: all run test bench style check-ci fonts fonts-list check-fonts android-fonts uninstall \
        deb \
        android android-check android-aab android-run install clean veryclean \
        distclean help
