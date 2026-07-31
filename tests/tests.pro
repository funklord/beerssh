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

QT += widgets testlib
CONFIG += c++17 console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../src $$PWD

SOURCES += \
    terminal_view_test.cpp \
    fake_ssh_session.cpp \
    ../src/ssh/ssh_session.cpp \
    ../src/ui/terminal_view.cpp

HEADERS += \
    fake_ssh_session.h \
    ../src/ssh/ssh_session.h \
    ../src/ui/terminal_view.h
