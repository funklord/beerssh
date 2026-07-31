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
    src/platform/platform.cpp \
    src/ssh/ssh_session.cpp \
    src/ui/main_window.cpp \
    src/ui/terminal_view.cpp

HEADERS += \
    src/platform/platform.h \
    src/ssh/ssh_session.h \
    src/ui/main_window.h \
    src/ui/terminal_view.h

# The libssh implementation of bssh_ssh_session is added here once the
# dependency is in the tree (M1). Nothing else in the project may mention
# libssh -- see src/ssh/ssh_session.h for why that matters.

# The one platform conditional. A second one appearing anywhere else in
# this file is the same smell as an #ifdef in the sources.
android {
    SOURCES += src/platform/platform_android.cpp

    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
    ANDROID_VERSION_CODE = 1
    ANDROID_VERSION_NAME = 0.1.0
} else {
    SOURCES += src/platform/platform_desktop.cpp
}
