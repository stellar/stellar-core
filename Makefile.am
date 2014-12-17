## Process this file with automake to produce Makefile.in


bin_PROGRAMS = bin/stellard

FMT_SRCS = src/overlay/Floodgate.cpp src/overlay/PeerDoor.cpp                   \
	src/overlay/TCPPeer.cpp src/overlay/LoopbackPeer.cpp src/overlay/Peer.cpp   \
	src/overlay/ItemFetcher.cpp src/overlay/PeerMaster.cpp                      \
	src/overlay/PreferredPeers.cpp src/util/types.cpp src/fba/FBAMaster.cpp \
	src/fba/FutureStatement.cpp src/fba/OurNode.cpp src/fba/QuorumSet.cpp       \
	src/fba/Statement.cpp src/fba/Node.cpp src/fba/Ballot.cpp                   \
	src/ledger/LedgerDatabase.cpp src/ledger/Ledger.cpp                         \
	src/ledger/LedgerEntry.cpp src/ledger/OfferEntry.cpp                        \
	src/ledger/LedgerMaster.cpp src/ledger/AccountEntry.cpp                     \
	src/ledger/TrustLine.cpp src/txherder/TxHerder.cpp                          \
	src/txherder/TransactionSet.cpp src/clf/BucketList.cpp src/main/main.cpp    \
	src/main/test.cpp src/main/Application.cpp src/main/Config.cpp              \
	src/transactions/TrustSetTx.cpp src/transactions/Transaction.cpp            \
	src/util/Logging.cpp src/fba/FBATests.cpp src/clf/CLFTests.cpp              \
	src/lib/http/connection.cpp src/lib/http/connection_manager.cpp             \
	src/lib/http/HttpClient.cpp src/lib/http/reply.cpp                          \
	src/lib/http/request_parser.cpp src/lib/http/server.cpp                     \
	src/main/CommandHandler.cpp src/process/ProcessMaster.cpp                   \
	src/process/ProcessTests.cpp src/history/HistoryMaster.cpp                  \
	src/history/HistoryTests.cpp src/overlay/OverlayTests.cpp

FMT_HDRS = src/clf/BucketList.h src/clf/CanonicalLedgerForm.h src/clf/CLF.h     \
	src/clf/CLFDelta.h src/clf/CLFGateway.h src/crypto/EdKeyPair.h              \
	src/crypto/StellarPrivateKey.h src/crypto/StellarPublicKey.h                \
	src/fba/Ballot.h src/fba/FBA.h src/fba/FBAGateway.h src/fba/FBAMaster.h     \
	src/fba/FBAWorldInterface.h src/fba/FutureStatement.h src/fba/Node.h        \
	src/fba/OurNode.h src/fba/QuorumSet.h src/fba/Statement.h                   \
	src/history/HistoryGateway.h src/history/HistoryMaster.h                    \
	src/ledger/AccountEntry.h src/ledger/Ledger.h src/ledger/LedgerDatabase.h   \
	src/ledger/LedgerEntry.h src/ledger/LedgerGateway.h                         \
	src/ledger/LedgerMaster.h src/ledger/OfferEntry.h src/ledger/TrustLine.h    \
	src/main/Application.h src/main/CommandHandler.h src/main/Config.h          \
	src/main/test.h src/overlay/FetchableItem.h src/overlay/Floodgate.h         \
	src/overlay/ItemFetcher.h src/overlay/LoopbackPeer.h                        \
	src/overlay/OverlayGateway.h src/overlay/Peer.h src/overlay/PeerDoor.h      \
	src/overlay/PeerMaster.h src/overlay/PreferredPeers.h src/overlay/TCPPeer.h \
	src/process/ProcessGateway.h src/process/ProcessMaster.h                    \
	src/transactions/CancelOfferTx.h src/transactions/CreateOfferTx.h           \
	src/transactions/InflationTx.h src/transactions/MergeTx.h                   \
	src/transactions/PaymentTx.h src/transactions/SetAccountTx.h                \
	src/transactions/SetRegularKeyTx.h src/transactions/Transaction.h           \
	src/transactions/TransactionResultCodes.h src/transactions/TrustSetTx.h     \
	src/txherder/TransactionSet.h src/txherder/TxHerder.h                       \
	src/txherder/TxHerderGateway.h src/util/Logging.h src/util/make_unique.h    \
	src/util/Timer.h src/util/types.h

BUILT_SOURCES = src/generated/StellardVersion.h src/generated/StellarXDR.h

bin_stellard_SOURCES = $(FMT_SRCS) \
                       $(BUILT_SOURCES) \
                       src/lib/util/getopt_long.c

bin_stellard_CPPFLAGS = -I$(top_srcdir)/src \
                        -I$(builddir)/src \
                        -I$(srcdir)/src/lib/autocheck/include \
                        -I$(srcdir)/src/lib/asio/include -DASIO_STANDALONE

CLEANFILES = $(BUILT_SOURCES)
GITDIR = $(top_srcdir)/.git
src/generated/StellardVersion.h: Makefile
	if test -d "$(GITDIR)"; then                                            \
      (VERS="$$(GIT_DIR=$(GITDIR) git describe --always --dirty --tags)" && \
       echo "#define STELLARD_VERSION \"$$VERS\"") >$@;                     \
    else                                                                    \
       echo "#define STELLARD_VERSION \"unknown\"" >$@;                     \
    fi

AM_CXXFLAGS = -stdlib=libc++

src/generated/StellarXDR.h: src/overlay/StellarXDR.x Makefile
	mkdir -p src/generated
	$(XDRC) -hh -o $@ $<

if USE_CLANG_FORMAT
format:
	cd $(srcdir) && $(CLANG_FORMAT) -i $(FMT_SRCS) $(FMT_HDRS)
endif

TESTS=test/selftest-stellard

@VALGRIND_CHECK_RULES@
VALGRIND_FLAGS = --num-callers=30 --trace-children=yes \
          --trace-children-skip=hostname,aws,gcutil,gcloud,azure,node,python
VALGRIND_SUPPRESSIONS_FILES = stellard.supp
EXTRA_DIST = stellard.supp
MOSTLYCLEANFILES =

maintainer-clean-local:
	+@echo rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
	rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
