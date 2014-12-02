## Process this file with automake to produce Makefile.in

bin_PROGRAMS = bin/stellard

bin_stellard_SOURCES = src/overlay/Floodgate.cpp src/overlay/PeerDoor.cpp	\
	src/overlay/Peer.cpp src/overlay/ItemFetcher.cpp						\
	src/overlay/PeerMaster.cpp src/overlay/PreferredPeers.cpp				\
	src/lib/util/types.cpp src/fba/FBAMaster.cpp							\
	src/fba/FutureStatement.cpp src/fba/OurNode.cpp src/fba/QuorumSet.cpp	\
	src/fba/NodePool.cpp src/fba/Statement.cpp src/fba/Node.cpp				\
	src/fba/History.cpp src/fba/PreparedStatement.cpp						\
	src/fba/CommitStatement.cpp src/fba/CommittedStatement.cpp				\
	src/fba/PrepareStatement.cpp src/fba/Ballot.cpp							\
	src/ledger/LedgerDatabase.cpp src/ledger/Ledger.cpp						\
	src/ledger/LedgerEntry.cpp src/ledger/OfferEntry.cpp					\
	src/ledger/LedgerMaster.cpp src/ledger/AccountEntry.cpp					\
	src/ledger/TrustLine.cpp src/txherder/TxHerder.cpp						\
	src/txherder/TransactionSet.cpp src/clf/BucketList.cpp					\
	src/main/main.cpp src/main/Application.cpp src/main/Config.cpp			\
	src/transactions/TrustSetTx.cpp src/transactions/Transaction.cpp		\
	src/util/Logging.cpp

bin_stellard_LDADD = -lxdrpp
bin_stellard_CPPFLAGS = -I$(srcdir)/src/lib/asio/include -DASIO_STANDALONE

AM_CXXFLAGS = -stdlib=libc++

# Less elegant but more portable than %.hh: %.x
SUFFIXES = .x .hh
.x.hh:
	$(XDRC) -hh -o $@ $<

maintainer-clean-local:
	+@echo rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
	rm -rf `sed -ne 's!^/!!p' .gitignore` Makefile.in
