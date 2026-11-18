# A host reached over fuzznet (src/ssh/fuzznet_session.h, project.md sec 5.6),
# for BeerSSH and its tests alike: the Makefile builds the vendored fuzznet's
# libfuzznetd.a and this tree's client link over it, and passes both in where
# fuzznet builds (Linux, for now). Empty, and none of it is compiled.
!isEmpty(BSSH_FUZZNET_LIB) {
    DEFINES += BSSH_FUZZNET
    SOURCES += $$PWD/src/ssh/fuzznet_session.cpp
    HEADERS += $$PWD/src/ssh/fuzznet_session.h \
               $$PWD/src/session/fuzznet_link.h
    # The link first: it is what reaches into fuzznet's archive.
    LIBS += $$BSSH_FUZZNET_CLIENT_LIB $$BSSH_FUZZNET_LIB
    PRE_TARGETDEPS += $$BSSH_FUZZNET_CLIENT_LIB $$BSSH_FUZZNET_LIB
}
