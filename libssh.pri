# The vendored libssh for a desktop build (project.md sec 8.4): the static
# archive the top-level Makefile builds from libssh/ with libssh-patch/
# applied. Included by beerssh.pro, beersshd.pro and test/tests.pro in
# place of PKGCONFIG libssh, so every program in the tree links the same
# patched copy -- a session exported by one build has to import into
# another.
#
# Built by `make`, not by qmake: a bare qmake build fails at link time
# naming the archive, which is the cue to run make.

BSSH_LIBSSH_PREFIX = $$PWD/build-libssh/prefix

INCLUDEPATH += $$BSSH_LIBSSH_PREFIX/include
DEFINES += LIBSSH_STATIC
LIBS += $$BSSH_LIBSSH_PREFIX/lib/libssh.a
PRE_TARGETDEPS += $$BSSH_LIBSSH_PREFIX/lib/libssh.a

# What a static libssh brings with it: its crypto backend and zlib, which
# the shared library would have pulled in by itself.
CONFIG += link_pkgconfig
PKGCONFIG += libcrypto zlib
LIBS += -lpthread
