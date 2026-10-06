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

QT = core
CONFIG += c++17 console
CONFIG -= app_bundle

QMAKE_CXXFLAGS_RELEASE -= -O2
QMAKE_CXXFLAGS_RELEASE += -Os

INCLUDEPATH += $$PWD/src

CONFIG += link_pkgconfig
PKGCONFIG += libssh

SOURCES += \
    src/sshd/main.cpp \
    src/sshd/root_path.cpp \
    src/sshd/sftp_handler.cpp \
    src/sshd/sshd.cpp

HEADERS += \
    src/sshd/root_path.h \
    src/sshd/sftp_handler.h \
    src/sshd/sshd.h
