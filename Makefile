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
#   make style        -- gate the tree's indentation (tools/check-indent.sh)
#   make android      -- build the Android AAB (needs a Qt-for-Android kit,
#                        see ANDROID below)
#   make install      -- install the binary to $(DESTDIR)$(PREFIX)/bin
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
#     make android QT_ANDROID_ROOT=$HOME/Qt/6.8.2/android_arm64_v8a
#   The recipe checks for it and says so rather than failing obscurely
#   halfway through a cross build.
# =============================================================================

CXX   ?= g++
QMAKE ?= qmake6

TARGET = beerssh

BUILD_DIR         = build
ANDROID_BUILD_DIR = build-android

PREFIX  ?= /usr/local
DESTDIR ?=

SOURCES = $(wildcard src/*.cpp src/*/*.cpp)
HEADERS = $(wildcard src/*.h src/*/*.h)

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

$(BUILD_DIR)/Makefile: beerssh.pro
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && $(QMAKE) ../beerssh.pro $(QMAKE_CONFIG) QMAKE_CXX=$(CXX)

$(TARGET): $(BUILD_DIR)/Makefile $(SOURCES) $(HEADERS)
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

$(TEST_BIN): $(TEST_BUILD_DIR)/Makefile $(SOURCES) $(HEADERS) $(wildcard tests/*.cpp tests/*.h)
	$(MAKE) -C $(TEST_BUILD_DIR)

# The offscreen QPA plugin is what makes both of these runnable in CI at
# all -- a GUI that only starts with a display is a GUI CI never exercises.
test: $(TARGET) $(TEST_BIN)
	QT_QPA_PLATFORM=offscreen ./$(TARGET) --self-test
	@echo "test: headless smoke test passed"
	QT_QPA_PLATFORM=offscreen ./$(TEST_BIN)

style:
	./tools/check-indent.sh

android:
	@if [ -z "$(QT_ANDROID_ROOT)" ]; then \
		echo "make android: QT_ANDROID_ROOT is not set."; \
		echo "make android:   point it at a Qt-for-Android kit, e.g."; \
		echo "make android:   make android QT_ANDROID_ROOT=\$$HOME/Qt/6.8.2/android_arm64_v8a"; \
		exit 1; \
	fi
	mkdir -p $(ANDROID_BUILD_DIR)
	cd $(ANDROID_BUILD_DIR) && $(QT_ANDROID_ROOT)/bin/qmake ../beerssh.pro $(QMAKE_CONFIG)
	$(MAKE) -C $(ANDROID_BUILD_DIR) aab

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

# clean removes intermediates only; $(TARGET) survives, matching the split
# every Makefile in the sibling tree uses.
clean:
	@if [ -f $(BUILD_DIR)/Makefile ]; then $(MAKE) -C $(BUILD_DIR) clean; fi
	@if [ -f $(TEST_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(TEST_BUILD_DIR) clean; fi
	@if [ -f $(ANDROID_BUILD_DIR)/Makefile ]; then $(MAKE) -C $(ANDROID_BUILD_DIR) clean; fi

# The build dirs are ours: this Makefile created them and qmake owns their
# contents, so removing them is a named delete, not a wildcard sweep.
veryclean: clean
	rm -rf $(BUILD_DIR) $(ANDROID_BUILD_DIR) $(TEST_BUILD_DIR)
	rm -f $(TARGET)

distclean: veryclean
	rm -f .qmake.stash
	find . -name '*~' -o -name '*.swp' | xargs -r rm -f

help:
	@sed -n '/^# TARGETS/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

.PHONY: all run test style android install clean veryclean distclean help
