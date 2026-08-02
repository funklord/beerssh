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

QT += widgets network sql
CONFIG += c++17
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/src

SOURCES += \
    src/main.cpp \
    src/model/conn_manager.cpp \
    src/model/preset.cpp \
    src/model/profile_store.cpp \
    src/model/settings_resolver.cpp \
    src/model/session.cpp \
    src/model/session_registry.cpp \
    src/input/input_router.cpp \
    src/platform/platform.cpp \
    src/platform/secret_store.cpp \
    src/ssh/ssh_session.cpp \
    src/ssh/ssh_session_libssh.cpp \
    src/term/emulator_vterm.cpp \
    src/term/font_catalog.cpp \
    src/term/graphics.cpp \
    src/term/image_store.cpp \
    src/term/scrollback.cpp \
    src/term/screen.cpp \
    src/ui/host_editor.cpp \
    src/ui/host_list.cpp \
    src/ui/layout_manager.cpp \
    src/ui/pane_container.cpp \
    src/ui/key_row.cpp \
    src/ui/main_window.cpp \
    src/ui/session_dialog.cpp \
    src/ui/tab_status.cpp \
    src/ui/terminal_view.cpp

HEADERS += \
    src/input/input_router.h \
    src/model/profile.h \
    src/model/conn_manager.h \
    src/model/preset.h \
    src/model/profile_store.h \
    src/model/settings_resolver.h \
    src/model/session.h \
    src/model/session_registry.h \
    src/platform/platform.h \
    src/platform/secret_store.h \
    src/ssh/ssh_session.h \
    src/ssh/ssh_session_libssh.h \
    src/term/emulator.h \
    src/term/font_catalog.h \
    src/term/graphics_internal.h \
    src/term/image_store.h \
    src/term/scrollback.h \
    src/term/screen.h \
    src/ui/host_editor.h \
    src/ui/host_list.h \
    src/ui/layout_manager.h \
    src/ui/pane_container.h \
    src/ui/key_row.h \
    src/ui/main_window.h \
    src/ui/session_dialog.h \
    src/ui/tab_status.h \
    src/ui/terminal_view.h

# libssh is the only third-party dependency so far. It is named here and
# nowhere else in the project -- src/ssh/ssh_session_libssh.cpp is the one
# file allowed to include its headers, which is what keeps the
# libssh-vs-libssh2 decision (project.md sec 4) genuinely reversible.
# Vendored libvterm (project.md sec 7): the parser is proven and assumes no
# PTY, so this project owns only the rendering and the input on top of it.
# Built by the top-level Makefile into a static archive -- `make`, not a
# bare qmake, is what produces a working build.
INCLUDEPATH += $$PWD/vterm/include

# One archive per target: the host build is x86_64 and cannot go into an
# arm64 library, which the first Android link demonstrated at length.
android {
    BSSH_VTERM_LIB = $$PWD/build-vterm-android/$$QT_ARCH/libvterm.a
} else {
    BSSH_VTERM_LIB = $$PWD/build-vterm/libvterm.a
}
LIBS += $$BSSH_VTERM_LIB
PRE_TARGETDEPS += $$BSSH_VTERM_LIB

# The one platform conditional, and everything platform-shaped lives inside
# it: the implementation of bssh_platform, the secret-store backends and
# their selector, and where the third-party libraries come from. A second
# conditional appearing elsewhere in this file is the same smell as an
# #ifdef in the sources (project.md sec 3, core principle 3).
android {
    SOURCES += \
        src/platform/platform_android.cpp \
        src/platform/secret_store_file.cpp \
        src/platform/secret_store_select_android.cpp

    # Cross-compiled, not system: an NDK sysroot has none of these.
    # tools/build-deps-android.sh builds them into deps/android/<abi>.
    BSSH_ANDROID_DEPS = $$PWD/deps/android/$$QT_ARCH
    INCLUDEPATH += $$BSSH_ANDROID_DEPS/include
    LIBS += -L$$BSSH_ANDROID_DEPS/lib -lssh -lssl -lcrypto

    # Fails HERE, naming the fix, rather than as a pile of missing symbols
    # at link time -- which is what the first Android build would otherwise
    # produce and spend an afternoon on.
    !exists($$BSSH_ANDROID_DEPS/lib/libssh.a) {
        error("Android dependencies are missing. Run: tools/build-deps-android.sh $$QT_ARCH")
    }

    # Sixel is NOT built for Android yet: libsixel needs its own cross
    # build, and inline images are the least of what a first device build
    # has to prove. bssh_parse_sixel declines when it is absent, which is
    # the same honest path it took before libsixel existed at all.
    DEFINES += BSSH_NO_SIXEL

    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
    ANDROID_VERSION_CODE = 1
    ANDROID_VERSION_NAME = 0.1.0

    # Set here rather than as <uses-sdk> in the manifest: the Gradle
    # plugin takes these from the build file and warns about (or
    # overrides) a manifest that also declares them. project.md sec 2:
    # minSdk 26, targetSdk 35 for Play.
    ANDROID_MIN_SDK_VERSION = 26
    ANDROID_TARGET_SDK_VERSION = 35

    # Deploy exactly these, and nothing else.
    #
    # Left to scan for itself, androiddeployqt bundles Qt's virtual
    # keyboard plugin, which pulls in QtQuick, QtQml, Lottie and
    # QuickShapes: seven megabytes of QML runtime in a Widgets-only app
    # that project.md sec 2 forbids from using QML at all. Android's soft
    # keyboard is the system IME, reached through the qtforandroid
    # platform plugin, so Qt's own on-screen keyboard is not merely
    # unused here, it is the wrong component.
    #
    # The cost of this list is that a genuinely needed plugin left out of
    # it fails at RUNTIME rather than at link time. Each line below says
    # what needs it.
    ANDROID_DEPLOYMENT_DEPENDENCIES = \
        jar/Qt6Android.jar \
        jar/Qt6AndroidNetwork.jar \
        lib/libQt6Core_$${QT_ARCH}.so \
        lib/libQt6Gui_$${QT_ARCH}.so \
        lib/libQt6Widgets_$${QT_ARCH}.so \
        lib/libQt6Network_$${QT_ARCH}.so \
        lib/libQt6Sql_$${QT_ARCH}.so \
        plugins/platforms/libplugins_platforms_qtforandroid_$${QT_ARCH}.so \
        plugins/styles/libplugins_styles_qandroidstyle_$${QT_ARCH}.so \
        plugins/sqldrivers/libplugins_sqldrivers_qsqlite_$${QT_ARCH}.so \
        plugins/imageformats/libplugins_imageformats_qjpeg_$${QT_ARCH}.so

    # jar/Qt6Android          QtActivityBase and friends. Left out of the
    #                         first attempt, which is how this list taught
    #                         that it governs Qt's JARS as well as its
    #                         shared objects: the build failed with twelve
    #                         javac errors about missing Qt classes.
    # jar/Qt6AndroidNetwork    QtNetwork's Java side
    # platforms/qtforandroid  the platform plugin; nothing runs without it
    # styles/qandroidstyle    native-looking widgets rather than Fusion
    # sqldrivers/qsqlite      the profile and preset store (sec 10.6)
    # imageformats/qjpeg      inline images (sec 7.1); PNG is inside QtGui
} else {
    SOURCES += \
        src/platform/platform_desktop.cpp \
        src/platform/secret_store_file.cpp \
        src/platform/secret_store_libsecret.cpp \
        src/platform/secret_store_select_desktop.cpp

    CONFIG += link_pkgconfig
    PKGCONFIG += libssh libsixel libcrypto libsecret-1
}
