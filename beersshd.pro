# =============================================================================
# beersshd.pro -- qmake project for beersshd, the SFTP-only server
#
# A separate program from BeerSSH, in the same tree (project.md sec 19):
# it shares the libssh build and the gates, and links none of the
# terminal. On the desktop it is a command line, which is what lets the
# server be tested here before it reaches a phone.
#
# Built through the top-level Makefile (`make beersshd`), not by hand.
# =============================================================================

TEMPLATE = app
TARGET = beersshd

isEmpty(BSSH_VERSION): BSSH_VERSION = 0.1.0
DEFINES += BSSH_VERSION_STRING=\\\"$$BSSH_VERSION\\\"

CONFIG += c++17
CONFIG -= app_bundle

QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os

INCLUDEPATH += $$PWD/src

SOURCES += \
    src/sshd/root_path.cpp \
    src/sshd/sftp_handler.cpp \
    src/sshd/sshd.cpp

HEADERS += \
    src/sshd/root_path.h \
    src/sshd/sftp_handler.h \
    src/sshd/sshd.h

# The one platform conditional. The desktop program is a command line
# over Qt Core; the Android one is a window, since a launcher has no
# arguments to give it. The window is not desktop-only code wearing an
# Android hat: it builds in the desktop test suite over a platform the
# tests control (test/sshd_window_test.cpp).
android {
    QT = core gui widgets network

    SOURCES += \
        src/sshd/main_android.cpp \
        src/sshd/sshd_platform.cpp \
        src/sshd/sshd_platform_android.cpp \
        src/sshd/sshd_window.cpp
    HEADERS += \
        src/sshd/sshd_platform.h \
        src/sshd/sshd_window.h

    # The same cross-compiled libssh BeerSSH links, built with its server
    # half by tool/build-deps-android.sh.
    BSSH_ANDROID_DEPS = $$PWD/deps/android/$$QT_ARCH
    INCLUDEPATH += $$BSSH_ANDROID_DEPS/include
    LIBS += -L$$BSSH_ANDROID_DEPS/lib -lssh -lssl -lcrypto
    !exists($$BSSH_ANDROID_DEPS/lib/libssh.a) {
        error("Android dependencies are missing. Run: tool/build-deps-android.sh $$QT_ARCH")
    }

    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android-sshd

    isEmpty(BSSH_VERSION_CODE): BSSH_VERSION_CODE = 1
    ANDROID_VERSION_CODE = $$BSSH_VERSION_CODE
    ANDROID_VERSION_NAME = $$BSSH_VERSION

    # Android 11, not BeerSSH's 8: "All files access" is an Android 11
    # permission, and below it shared storage is a different permission
    # model this app would need a second path for.
    ANDROID_MIN_SDK_VERSION = 30
    ANDROID_TARGET_SDK_VERSION = 36

    # Exactly these, for beerssh.pro's reason: left to scan, androiddeployqt
    # bundles Qt's virtual keyboard and the QML runtime under it.
    ANDROID_DEPLOYMENT_DEPENDENCIES = \
        jar/Qt6Android.jar \
        jar/Qt6AndroidNetwork.jar \
        lib/libQt6Core_$${QT_ARCH}.so \
        lib/libQt6Gui_$${QT_ARCH}.so \
        lib/libQt6Widgets_$${QT_ARCH}.so \
        lib/libQt6Network_$${QT_ARCH}.so \
        plugins/platforms/libplugins_platforms_qtforandroid_$${QT_ARCH}.so \
        plugins/styles/libplugins_styles_qandroidstyle_$${QT_ARCH}.so
} else {
    QT = core
    CONFIG += console

    SOURCES += src/sshd/main.cpp

    CONFIG += link_pkgconfig
    # libcrypto for the password's PBKDF2 (project.md sec 19); Android links
    # it with libssh above.
    PKGCONFIG += libssh libcrypto
}
