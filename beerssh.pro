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

# Passed in by the top-level Makefile so the version lives in one place.
isEmpty(BSSH_VERSION): BSSH_VERSION = 0.1.0
DEFINES += BSSH_VERSION_STRING=\\\"$$BSSH_VERSION\\\"

QT += widgets network

# The font set's pins, which the phone checks a download against (sec 9.4).
RESOURCES += resources.qrc

# The zip reader under font downloads (term/zip_index.cpp) inflates with zlib.
LIBS += -lz

# QtDBus is desktop-only: the XDG portal is where Linux keeps the
# light/dark preference, and Qt does not always read it for us (sec 6.2).
!android: QT += dbus
CONFIG += c++17
CONFIG -= app_bundle

# Size over speed. qmake's release default is -O2 and it is applied by the
# generated Makefile, so saying it here is the only place it takes effect.
QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os
QMAKE_CFLAGS_RELEASE   -= -O2
QMAKE_CFLAGS_RELEASE   += -Os

INCLUDEPATH += $$PWD/src $$PWD

SOURCES += \
    src/main.cpp \
    src/hang_watchdog.cpp \
    src/model/app_settings.cpp \
    src/model/conn_manager.cpp \
    src/model/command_line.cpp \
    src/model/document_path.cpp \
    src/model/preset.cpp \
    src/model/multiplexer.cpp \
    src/ui/multiplexer_menu.cpp \
    src/model/ssh_config.cpp \
    src/model/profile_store.cpp \
    src/model/settings_resolver.cpp \
    src/model/window_title.cpp \
    src/model/session.cpp \
    src/model/handoff_wire.cpp \
    src/model/handoff_point.cpp \
    src/model/session_registry.cpp \
    src/model/kept_scrollback.cpp \
    src/model/font_sources.cpp \
    src/model/font_download.cpp \
    src/term/zip_index.cpp \
    src/term/picture_spill.cpp \
    src/model/custom_font.cpp \
    src/input/input_router.cpp \
    src/platform/platform.cpp \
    src/platform/secret_store.cpp \
    src/ssh/pty_session.cpp \
    src/ssh/ssh_session.cpp \
    src/ssh/jump_tunnel.cpp \
    src/ssh/ssh_session_libssh.cpp \
    src/term/emulator_vterm.cpp \
    src/term/colour_scheme.cpp \
    src/term/font_catalog.cpp \
    src/term/graphics.cpp \
    src/term/image_store.cpp \
    src/term/scrollback.cpp \
    src/term/screen.cpp \
    src/ui/font_picker.cpp \
    src/ui/form_wrap.cpp \
    src/ui/password_prompt.cpp \
    src/ui/preset_editor.cpp \
    src/ui/host_editor.cpp \
    src/ui/override_badge.cpp \
    src/ui/host_list.cpp \
    src/ui/layout_manager.cpp \
    src/ui/pane_bar.cpp \
    src/ui/pane_container.cpp \
    src/ui/find_bar.cpp \
    src/ui/input_selector.cpp \
    src/ui/interface_scale.cpp \
    src/ui/app_settings_view.cpp \
    src/ui/store_view.cpp \
    src/ui/colour_editor.cpp \
    src/ui/term_features_editor.cpp \
    src/ui/key_row_editor.cpp \
    src/ui/licences_view.cpp \
    src/ui/long_press.cpp \
    src/term/term_features.cpp \
    src/ui/scroll_guard.cpp \
    src/ui/wrapped_note.cpp \
    src/ui/main_window.cpp \
    src/ui/session_dialog.cpp \
    src/ui/ssh_config_import.cpp \
    src/ui/tab_status.cpp \
    src/ui/touch_metrics.cpp \
    src/ui/terminal_accessible.cpp \
    src/ui/theme.cpp \
    src/ui/terminal_view.cpp

HEADERS += \
    src/input/input_router.h \
    src/hang_watchdog.h \
    src/model/profile.h \
    src/model/app_settings.h \
    src/model/conn_manager.h \
    src/model/command_line.h \
    src/model/document_path.h \
    src/model/preset.h \
    src/model/multiplexer.h \
    src/ui/multiplexer_menu.h \
    src/model/ssh_config.h \
    src/model/profile_store.h \
    src/model/settings_resolver.h \
    src/model/window_title.h \
    src/model/session.h \
    src/model/handoff_wire.h \
    src/model/handoff_point.h \
    src/model/session_registry.h \
    src/model/kept_scrollback.h \
    src/model/font_sources.h \
    src/model/font_download.h \
    src/term/zip_index.h \
    src/term/picture_spill.h \
    src/model/custom_font.h \
    src/platform/platform.h \
    src/platform/secret_store.h \
    src/ssh/pty_session.h \
    src/ssh/ssh_session.h \
    src/ssh/jump_tunnel.h \
    src/ssh/ssh_session_libssh.h \
    src/term/emulator.h \
    src/term/colour_scheme.h \
    src/term/font_catalog.h \
    src/term/graphics_internal.h \
    src/term/image_store.h \
    src/term/scrollback.h \
    src/term/screen.h \
    src/ui/font_picker.h \
    src/ui/form_wrap.h \
    src/ui/password_prompt.h \
    src/ui/preset_editor.h \
    src/ui/host_editor.h \
    src/ui/override_badge.h \
    src/ui/host_list.h \
    src/ui/layout_manager.h \
    src/ui/pane_bar.h \
    src/ui/pane_container.h \
    src/ui/find_bar.h \
    src/ui/input_selector.h \
    src/ui/input_mode.h \
    src/ui/interface_scale.h \
    src/platform/headless.h \
    src/ui/app_settings_view.h \
    src/ui/store_view.h \
    src/ui/colour_editor.h \
    src/ui/term_features_editor.h \
    src/ui/key_row_editor.h \
    src/ui/licences_view.h \
    src/ui/long_press.h \
    src/term/term_features.h \
    src/ui/scroll_guard.h \
    src/ui/wrapped_note.h \
    src/ui/main_window.h \
    src/ui/session_dialog.h \
    src/ui/ssh_config_import.h \
    src/ui/tab_status.h \
    src/ui/touch_metrics.h \
    src/ui/terminal_accessible.h \
    src/ui/theme.h \
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

# The on-screen keyboard is softkeys, vendored as a submodule (project.md
# sec 6.2): its .pri adds its sources, its headers for moc, and its
# include/ so "softkeys/keyboard.h" resolves. Not listed here a second
# time, so a file softkeys adds needs no edit in this tree.
include($$PWD/softkeys/softkeys.pri)

# One archive per target: the host build is x86_64 and cannot go into an
# arm64 library, which the first Android link demonstrated at length.
android {
    BSSH_VTERM_LIB = $$PWD/build-vterm-android/$$QT_ARCH/libvterm.a
} else {
    BSSH_VTERM_LIB = $$PWD/build-vterm/libvterm.a
}
# forkpty(3) for the local shell transport (sec 2, sec 3). glibc puts it
# in libutil; bionic has it in libc, so Android needs nothing and linking
# -lutil there fails.
!android: LIBS += -lutil

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
        src/platform/range_source_android.cpp \
        src/platform/secret_store_file.cpp \
        src/platform/secret_store_keystore.cpp \
        src/platform/secret_store_select_android.cpp
    HEADERS += src/platform/range_source_android.h

    # Cross-compiled, not system: an NDK sysroot has none of these.
    # tool/build-deps-android.sh builds them into deps/android/<abi>.
    BSSH_ANDROID_DEPS = $$PWD/deps/android/$$QT_ARCH
    INCLUDEPATH += $$BSSH_ANDROID_DEPS/include
    LIBS += -L$$BSSH_ANDROID_DEPS/lib -lssh -lssl -lcrypto
    # A rebuilt libssh relinks the app: libssh carries this tree's patches
    # (sec 8.4), and an app linked against the archive before a patch
    # landed would run without it and say nothing.
    PRE_TARGETDEPS += $$BSSH_ANDROID_DEPS/lib/libssh.a

    # Fails HERE, naming the fix, rather than as a pile of missing symbols
    # at link time -- which is what the first Android build would otherwise
    # produce and spend an afternoon on.
    !exists($$BSSH_ANDROID_DEPS/lib/libssh.a) {
        error("Android dependencies are missing. Run: tool/build-deps-android.sh $$QT_ARCH")
    }

    # Sixel is NOT built for Android yet: libsixel needs its own cross
    # build, and inline images are the least of what a first device build
    # has to prove. bssh_parse_sixel declines when it is absent, which is
    # the same honest path it took before libsixel existed at all.
    DEFINES += BSSH_NO_SIXEL

    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
    # Play rejects any upload whose versionCode is not strictly greater
    # than the last one it accepted, so a hardcoded number is fine for the
    # first upload and blocks every update after it -- which is most of
    # the point of being on a store.
    #
    # The number is computed by the Makefile from VERSION and passed in
    # (major*10000 + minor*100 + patch, so 0.1.0 is 100 and 1.2.3 is
    # 10203). It is done there rather than here because qmake has no
    # arithmetic: num_add exists and multiply does not, and building the
    # digits as a padded string yields "00100", which is not an integer
    # any manifest should carry.
    #
    # The fallback matters. A qmake run without the Makefile -- opening
    # this in Qt Creator, say -- must still produce something installable,
    # and 1 is the value that was hardcoded here before.
    isEmpty(BSSH_VERSION_CODE): BSSH_VERSION_CODE = 1
    ANDROID_VERSION_CODE = $$BSSH_VERSION_CODE
    ANDROID_VERSION_NAME = $$BSSH_VERSION

    # Set here rather than as <uses-sdk> in the manifest: the Gradle
    # plugin takes these from the build file and warns about (or
    # overrides) a manifest that also declares them. project.md sec 2:
    # minSdk 26, targetSdk 36 -- Play's floor for new apps and updates
    # since 2026-08-31, and android-play-check refuses a bundle below it.
    ANDROID_MIN_SDK_VERSION = 26
    ANDROID_TARGET_SDK_VERSION = 36

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
        jar/Qt6AndroidNetworkInformationBackend.jar \
        lib/libQt6Core_$${QT_ARCH}.so \
        lib/libQt6Gui_$${QT_ARCH}.so \
        lib/libQt6Widgets_$${QT_ARCH}.so \
        lib/libQt6Network_$${QT_ARCH}.so \
        plugins/platforms/libplugins_platforms_qtforandroid_$${QT_ARCH}.so \
        plugins/styles/libplugins_styles_qandroidstyle_$${QT_ARCH}.so \
        plugins/imageformats/libplugins_imageformats_qjpeg_$${QT_ARCH}.so \
        plugins/networkinformation/libplugins_networkinformation_qandroidnetworkinformation_$${QT_ARCH}.so

    # jar/Qt6Android          QtActivityBase and friends. Left out of the
    #                         first attempt, which is how this list taught
    #                         that it governs Qt's JARS as well as its
    #                         shared objects: the build failed with twelve
    #                         javac errors about missing Qt classes.
    # jar/Qt6AndroidNetwork    QtNetwork's Java side
    # jar/Qt6AndroidNetwork-  the networkinformation plugin's Java side, and
    #   InformationBackend    the plugin is useless without it: it loads,
    #                         fails to find its class, and Qt falls back to
    #                         a "dummy" backend that reports nothing. That
    #                         is silent -- no error, no missing feature,
    #                         just a signal that never fires -- and only a
    #                         device showed it.
    # platforms/qtforandroid  the platform plugin; nothing runs without it
    # styles/qandroidstyle    native-looking widgets rather than Fusion
    # imageformats/qjpeg      inline images (sec 7.1); PNG is inside QtGui
    # networkinformation/     sec 5.3's network-change reconnect. This is
    #   qandroidnetwork...    ConnectivityManager underneath, which is what
    #                         sec 5.3 asks for by name, so no JNI and no
    #                         second Java file are needed. It is exactly the
    #                         cost this list warns about: QNetworkInformation
    #                         compiles and links without it and simply finds
    #                         no backend at runtime, so a Wi-Fi-to-mobile
    #                         handover waits out the backoff in silence.
} else {
    SOURCES += \
        src/platform/platform_desktop.cpp \
        src/platform/secret_store_file.cpp \
        src/platform/secret_store_libsecret.cpp \
        src/platform/secret_store_select_desktop.cpp

    CONFIG += link_pkgconfig
    PKGCONFIG += libsixel libcrypto libsecret-1
    include($$PWD/libssh.pri)

    # The X server, asked which window is under the pointer during the
    # app's own session drag (project.md sec 8.4): the one way to tell a
    # drop on another BeerSSH from a drop on the desktop.
    PKGCONFIG += xcb
}
