# Read back the dependency files the compiler wrote into a qmake build directory.
#
# qmake works out header dependencies when it runs, and it runs when the .pro
# changes, so a new #include in an existing file was missing from them for good.
# A changed header then left that object on the old layout: on 2026-11-07
# session_host_test.o, a day older than session.h, segfaulted at a different
# address in a different test each run, in code the change never touched.
# So every .pro compiles with -MMD -MP and every sub-make takes this as well:
#
#     $(MAKE) -C dir -f Makefile -f $(CURDIR)/tool/qmake-deps.mk
#
# Second on that line, so that qmake's first target stays the default goal.
# And below it, where a build keeps objects beside their sources' paths
# (the media build's object_parallel_to_source): unread, those trees
# would never rebuild for a changed header.
-include $(wildcard *.d */*.d */*/*.d */*/*/*.d)

# And what the build was configured with, where the caller keeps a record
# of it: a switch handed to qmake (a DEFINES, a library) changes how every
# object compiles, but qmake's objects depend on their sources and headers,
# not on the Makefile it rewrote -- so a build directory made before the
# switch moved kept objects compiled without it. The record is rewritten
# only when the configuration moves, so this costs nothing otherwise.
ifneq ($(wildcard config.source),)
$(OBJECTS): config.source
endif
