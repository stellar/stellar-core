## Process this file with automake to produce Makefile.in


bin_PROGRAMS = bin/stellard

FMT_SRCS = src/overlay/Floodgate.cpp src/overlay/PeerDoor.cpp                   \
	src/overlay/TCPPeer.cpp src/overlay/LoopbackPeer.cpp src/overlay/Peer.cpp   \
	src/overlay/ItemFetcher.cpp src/overlay/PeerMaster.cpp                      \
	src/overlay/PreferredPeers.cpp src/lib/util/types.cpp src/fba/FBAMaster.cpp \
	src/fba/FutureStatement.cpp src/fba/OurNode.cpp src/fba/QuorumSet.cpp       \
	src/fba/Statement.cpp src/fba/Node.cpp src/fba/Ballot.cpp                   \
	src/ledger/LedgerDatabase.cpp src/ledger/Ledger.cpp                         \
	src/ledger/LedgerEntry.cpp src/ledger/OfferEntry.cpp                        \
	src/ledger/LedgerMaster.cpp src/ledger/AccountEntry.cpp                     \
	src/ledger/TrustLine.cpp src/txherder/TxHerder.cpp                          \
	src/txherder/TransactionSet.cpp src/clf/BucketList.cpp src/main/main.cpp    \
	src/main/test.cpp src/main/Application.cpp src/main/Config.cpp              \
	src/transactions/TrustSetTx.cpp src/transactions/Transaction.cpp            \
	src/util/Logging.cpp


bin_stellard_SOURCES = $(FMT_SRCS) src/lib/util/getopt_long.c src/util/StellardVersion.h
bin_stellard_LDADD = -lxdrpp
bin_stellard_CPPFLAGS = -I$(srcdir)/src/lib/autocheck/include \
                        -I$(srcdir)/src/lib/asio/include -DASIO_STANDALONE

MAINTAINERCLEANFILES = src/util/StellardVersion.h
BUILT_SOURCES = src/util/StellardVersion.h
GITDIR = $(top_srcdir)/.git
src/util/StellardVersion.h: Makefile
	if test -d "$(GITDIR)"; then                                            \
	  (VERS="$$(GIT_DIR=$(GITDIR) git describe --always --dirty --tags)" && \
       echo "#define STELLARD_VERSION \"$$VERS\"") >$(srcdir)/$@;           \
    fi

.PHONY: $(srcdir)/src/util/StellardVersion.h

AM_CXXFLAGS = -stdlib=libc++

# Less elegant but more portable than %.hh: %.x
SUFFIXES = .x .hh
.x.hh:
	$(XDRC) -hh -o $@ $<

if USE_CLANG_FORMAT
format:
	cd $(srcdir) && $(CLANG_FORMAT) -i $(FMT_SRCS)
endif

maintainer-clean-local:
	+@echo rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
	rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
