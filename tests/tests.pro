# =============================================================================
# tests/tests.pro -- host unit tests (project.md sec 12.3)
#
# Built in its own directory from its own .pro, for the reason every Qt
# test project needs one: the test binary needs the app's sources but not
# src/main.cpp, or it would have two main() functions.
#
# No device, no server and no display: these run under the offscreen QPA
# plugin, which is what lets CI check them at all. Run via `make test`
# from the repo root.
# =============================================================================

TEMPLATE = app
TARGET = beerssh_test

QT += widgets network sql testlib

CONFIG += link_pkgconfig
PKGCONFIG += libsixel libssh
CONFIG += c++17 console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../src $$PWD $$PWD/../vterm/include
LIBS += $$PWD/../build-vterm/libvterm.a
PRE_TARGETDEPS += $$PWD/../build-vterm/libvterm.a

SOURCES += \
    main.cpp \
    emulator_test.cpp \
    graphics_test.cpp \
    platform_test.cpp \
    profile_store_test.cpp \
    input_router_test.cpp \
    screen_test.cpp \
    terminal_view_test.cpp \
    fake_ssh_session.cpp \
    ../src/input/input_router.cpp \
    ../src/ssh/ssh_session.cpp \
    ../src/ssh/ssh_session_libssh.cpp \
    ../src/term/emulator_vterm.cpp \
    ../src/term/graphics.cpp \
    ../src/term/image_store.cpp \
    ../src/term/scrollback.cpp \
    ../src/term/screen.cpp \
    ../src/platform/platform.cpp \
    ../src/model/profile_store.cpp \
    ../src/ui/host_list.cpp \
    ../src/ui/main_window.cpp \
    ../src/ui/session_dialog.cpp \
    ../src/platform/platform_desktop.cpp \
    ../src/ui/key_row.cpp \
    ../src/ui/terminal_view.cpp

HEADERS += \
    fake_ssh_session.h \
    emulator_test.h \
    graphics_test.h \
    platform_test.h \
    profile_store_test.h \
    input_router_test.h \
    screen_test.h \
    terminal_view_test.h \
    ../src/input/input_router.h \
    ../src/ssh/ssh_session.h \
    ../src/ssh/ssh_session_libssh.h \
    ../src/term/emulator.h \
    ../src/term/graphics_internal.h \
    ../src/term/image_store.h \
    ../src/term/scrollback.h \
    ../src/term/screen.h \
    ../src/platform/platform.h \
    ../src/model/profile.h \
    ../src/model/profile_store.h \
    ../src/ui/host_list.h \
    ../src/ui/main_window.h \
    ../src/ui/session_dialog.h \
    ../src/ui/key_row.h \
    ../src/ui/terminal_view.h
