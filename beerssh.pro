# =============================================================================
# beerssh.pro -- qmake project for BeerSSH, the Qt Widgets SSH terminal
#
# One project file for both shipping platforms (project.md sec 1): a plain
# host build on desktop, and an androiddeployqt build for Android. The only
# platform conditional in here is the scope below that picks the
# bssh_platform implementation -- everything else is shared by
# construction, which is the point (sec 3, core principle 3).
#
# Built through the top-level Makefile, not by hand: `make` wraps qmake the
# same way fuzzypickles' gui/Makefile does, so the Qt subtree and the
# hand-written Makefiles present one interface. See project.md sec 12.1.
#
# No QtQuick/QML, ever -- project.md sec 2, non-negotiable. Nothing above
# requests it; this comment is the tripwire for the next person tempted to
# add `QT += quick` for "just one screen".
# =============================================================================

TEMPLATE = app
TARGET = beerssh

QT += widgets
CONFIG += c++17
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/src

SOURCES += \
    src/main.cpp \
    src/input/input_router.cpp \
    src/platform/platform.cpp \
    src/ssh/ssh_session.cpp \
    src/ssh/ssh_session_libssh.cpp \
    src/term/emulator_vterm.cpp \
    src/term/graphics.cpp \
    src/term/image_store.cpp \
    src/term/scrollback.cpp \
    src/term/screen.cpp \
    src/ui/key_row.cpp \
    src/ui/main_window.cpp \
    src/ui/terminal_view.cpp

HEADERS += \
    src/input/input_router.h \
    src/platform/platform.h \
    src/ssh/ssh_session.h \
    src/ssh/ssh_session_libssh.h \
    src/term/emulator.h \
    src/term/graphics_internal.h \
    src/term/image_store.h \
    src/term/scrollback.h \
    src/term/screen.h \
    src/ui/key_row.h \
    src/ui/main_window.h \
    src/ui/terminal_view.h

# libssh is the only third-party dependency so far. It is named here and
# nowhere else in the project -- src/ssh/ssh_session_libssh.cpp is the one
# file allowed to include its headers, which is what keeps the
# libssh-vs-libssh2 decision (project.md sec 4) genuinely reversible.
CONFIG += link_pkgconfig
PKGCONFIG += libssh libsixel

# Vendored libvterm (project.md sec 7): the parser is proven and assumes no
# PTY, so this project owns only the rendering and the input on top of it.
# Built by the top-level Makefile into a static archive -- `make`, not a
# bare qmake, is what produces a working build.
INCLUDEPATH += $$PWD/vterm/include
LIBS += $$PWD/build-vterm/libvterm.a
PRE_TARGETDEPS += $$PWD/build-vterm/libvterm.a

# The one platform conditional. A second one appearing anywhere else in
# this file is the same smell as an #ifdef in the sources.
android {
    SOURCES += src/platform/platform_android.cpp

    # NOTE: PKGCONFIG above will not resolve for Android -- there is no
    # system libssh in an NDK sysroot. The cross-compiled libssh+OpenSSL
    # (project.md sec 12.1) gets wired in here, and until it is, the
    # Android build stops at configure time rather than silently producing
    # an app with no SSH in it.

    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
    ANDROID_VERSION_CODE = 1
    ANDROID_VERSION_NAME = 0.1.0
} else {
    SOURCES += src/platform/platform_desktop.cpp
}
