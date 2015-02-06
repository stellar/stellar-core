## Process this file with automake to produce Makefile.in

SUBDIRS =
noinst_LTLIBRARIES = libsoci.la
bin_PROGRAMS = bin/stellard

FMT_SRCS = src/clf/BucketList.cpp src/clf/CLFMaster.cpp src/clf/CLFTests.cpp    \
    src/crypto/Base58.cpp src/crypto/Hex.cpp src/crypto/SHA.cpp                 \
    src/crypto/SecretKey.cpp src/crypto/CryptoTests.cpp src/crypto/Random.cpp   \
    src/database/Database.cpp src/database/DatabaseTests.cpp src/fba/FBA.cpp    \
    src/fba/Slot.cpp src/fba/Node.cpp src/fba/LocalNode.cpp                     \
    src/fba/FBATests.cpp src/history/HistoryArchive.cpp                         \
    src/history/HistoryMaster.cpp src/history/HistoryTests.cpp                  \
    src/ledger/AccountFrame.cpp src/ledger/EntryFrame.cpp                       \
    src/ledger/LedgerDelta.cpp src/ledger/LedgerMaster.cpp                      \
    src/ledger/OfferFrame.cpp src/ledger/TrustFrame.cpp                         \
    src/ledger/LedgerHeaderFrame.cpp src/ledger/LedgerHeaderTests.cpp           \
    src/main/Application.cpp src/main/CommandHandler.cpp src/main/Config.cpp    \
    src/main/main.cpp src/main/test.cpp src/overlay/Floodgate.cpp               \
    src/overlay/ItemFetcher.cpp src/overlay/LoopbackPeer.cpp                    \
    src/overlay/OverlayTests.cpp src/overlay/Peer.cpp src/overlay/PeerDoor.cpp  \
    src/overlay/PeerMaster.cpp src/overlay/TCPPeer.cpp                          \
    src/process/ProcessMaster.cpp src/process/ProcessTests.cpp                  \
    src/simulation/CycleTests.cpp src/simulation/CoreTests.cpp                  \
    src/simulation/Simulation.cpp  src/herder/Herder.cpp                        \
    src/herder/TxSetFrame.cpp src/herder/HerderTests.cpp src/util/Logging.cpp   \
	src/util/Timer.cpp src/util/types.cpp                                       \
    src/util/TmpDir.cpp src/util/Uint128Tests.cpp src/util/TimerTests.cpp       \
    src/transactions/AllowTrustTxFrame.cpp                                      \
    src/transactions/CancelOfferFrame.cpp                                       \
    src/transactions/ChangeTrustTxFrame.cpp                                     \
    src/transactions/CreateOfferFrame.cpp src/transactions/InflationFrame.cpp   \
    src/transactions/InflationTests.cpp src/transactions/MergeFrame.cpp         \
    src/transactions/MergeTests.cpp src/transactions/OfferTests.cpp             \
    src/transactions/OfferExchange.cpp                                          \
    src/transactions/PaymentFrame.cpp src/transactions/PaymentTests.cpp         \
    src/transactions/SetOptionsFrame.cpp src/transactions/SetOptionsTests.cpp   \
    src/transactions/TransactionFrame.cpp src/transactions/TxEnvelopeTests.cpp  \
    src/transactions/TxTests.cpp


FMT_HDRS = src/clf/BucketList.h src/clf/LedgerCmp.h src/clf/CLFMaster.h         \
    src/crypto/Base58.h src/crypto/Hex.h src/crypto/SHA.h                       \
    src/crypto/SecretKey.h src/crypto/Random.h src/database/Database.h          \
    src/fba/FBA.h src/fba/Slot.h src/fba/Node.h src/fba/LocalNode.h             \
    src/history/HistoryArchive.h src/history/HistoryGateway.h                   \
    src/history/HistoryMaster.h src/ledger/AccountFrame.h                       \
    src/ledger/EntryFrame.h src/ledger/LedgerDelta.h src/ledger/LedgerGateway.h \
    src/ledger/LedgerMaster.h src/ledger/OfferFrame.h src/ledger/TrustFrame.h   \
    src/ledger/LedgerHeaderFrame.h src/main/Application.h                       \
    src/main/CommandHandler.h src/main/Config.h src/main/test.h                 \
    src/overlay/FetchableItem.h src/overlay/Floodgate.h                         \
    src/overlay/ItemFetcher.h src/overlay/LoopbackPeer.h                        \
    src/overlay/OverlayGateway.h src/overlay/Peer.h src/overlay/PeerDoor.h      \
    src/overlay/PeerMaster.h src/overlay/TCPPeer.h src/process/ProcessGateway.h \
    src/process/ProcessMaster.h src/simulation/Simulation.h src/herder/Herder.h \
    src/herder/HerderGateway.h src/herder/TxSetFrame.h src/util/Logging.h       \
    src/util/Timer.h src/util/make_unique.h src/util/types.h src/util/TmpDir.h  \
    src/util/XDRStream.h src/transactions/AllowTrustTxFrame.h                   \
    src/transactions/CancelOfferFrame.h src/transactions/ChangeTrustTxFrame.h   \
    src/transactions/CreateOfferFrame.h src/transactions/InflationFrame.h       \
    src/transactions/OfferExchange.h                                            \
    src/transactions/MergeFrame.h src/transactions/PaymentFrame.h               \
    src/transactions/SetOptionsFrame.h src/transactions/TransactionFrame.h      \
    src/transactions/TxTests.h


BUILT_SOURCES = src/generated/StellardVersion.h                                 \
                src/generated/StellarXDR.h src/generated/FBAXDR.h

bin_stellard_SOURCES = $(FMT_SRCS) \
                       $(BUILT_SOURCES) \
                       src/lib/util/getopt_long.c  \
                       src/lib/util/format.cc      \
                       src/lib/util/uint128_t.cpp  \
                       src/lib/json/jsoncpp.cpp    \
                       src/lib/http/HttpClient.cpp \
                       src/lib/http/connection.cpp \
                       src/lib/http/connection_manager.cpp \
                       src/lib/http/reply.cpp \
                       src/lib/http/request_parser.cpp \
                       src/lib/http/server.cpp \
                       src/lib/asio/src/asio.cpp

bin_stellard_CPPFLAGS = -I$(top_srcdir)/src \
                        -I$(builddir)/src \
                        -I$(srcdir)/src/lib/autocheck/include \
                        -I$(srcdir)/src/lib/cereal/include \
                        -I$(srcdir)/src/lib/util \
                        -I$(srcdir)/src/lib/asio/include -DASIO_SEPARATE_COMPILATION \
                        -I$(srcdir)/src/lib/soci/src/core \
                        -I$(srcdir)/src/lib/soci/src/backends/sqlite3 \
                        $(libsodium_CFLAGS) \
                        $(xdrpp_CFLAGS) \
                        $(sqlite_CFLAGS) \
                        $(libpq_CFLAGS) \
                        $(libmedida_CFLAGS)

bin_stellard_LDADD = libsoci.la \
                     $(libsodium_LIBS) \
                     $(xdrpp_LIBS) \
                     $(sqlite_LIBS) \
                     $(libpq_LIBS) \
                     $(libmedida_LIBS)
bin_stellard_DEPENDENCIES =

if LIBSODIUM_INTERNAL
LIBSODIUM_DIR=src/lib/libsodium
LIBSODIUM=$(top_builddir)/$(LIBSODIUM_DIR)/src/libsodium/libsodium.la
bin_stellard_LDADD += $(LIBSODIUM)
SUBDIRS += $(LIBSODIUM_DIR)
$(LIBSODIUM):
	$(MAKE) -C $(top_builddir)/$(LIBSODIUM_DIR)
endif

if SQLITE_INTERNAL
noinst_LTLIBRARIES += libsqlite.la
libsqlite_la_SOURCES = src/lib/sqlite/sqlite3.c src/lib/sqlite/sqlite3.h
libsqlite_la_CPPFLAGS = -I$(srcdir)/src/lib/sqlite -DSQLITE_OMIT_LOAD_EXTENSION
bin_stellard_DEPENDENCIES += libsqlite.la
bin_stellard_LDADD += libsqlite.la
endif

libsoci_la_CPPFLAGS = -I$(srcdir)/src/lib/soci/src/core \
                      -I$(srcdir)/src/lib/soci/src/backends/sqlite3 \
                      $(sqlite_CFLAGS)

libsoci_la_SOURCES = src/lib/soci/src/backends/sqlite3/standard-use-type.cpp    \
    src/lib/soci/src/backends/sqlite3/session.cpp                               \
    src/lib/soci/src/backends/sqlite3/common.cpp                                \
    src/lib/soci/src/backends/sqlite3/vector-use-type.cpp                       \
    src/lib/soci/src/backends/sqlite3/row-id.cpp                                \
    src/lib/soci/src/backends/sqlite3/factory.cpp                               \
    src/lib/soci/src/backends/sqlite3/vector-into-type.cpp                      \
    src/lib/soci/src/backends/sqlite3/statement.cpp                             \
    src/lib/soci/src/backends/sqlite3/blob.cpp                                  \
    src/lib/soci/src/backends/sqlite3/standard-into-type.cpp                    \
    src/lib/soci/src/core/ref-counted-prepare-info.cpp                          \
    src/lib/soci/src/core/transaction.cpp src/lib/soci/src/core/session.cpp     \
    src/lib/soci/src/core/rowid.cpp src/lib/soci/src/core/prepare-temp-type.cpp \
    src/lib/soci/src/core/into-type.cpp src/lib/soci/src/core/soci-simple.cpp   \
    src/lib/soci/src/core/backend-loader.cpp                                    \
    src/lib/soci/src/core/connection-pool.cpp                                   \
    src/lib/soci/src/core/ref-counted-statement.cpp                             \
    src/lib/soci/src/core/error.cpp src/lib/soci/src/core/values.cpp            \
    src/lib/soci/src/core/once-temp-type.cpp                                    \
    src/lib/soci/src/core/statement.cpp src/lib/soci/src/core/blob.cpp          \
    src/lib/soci/src/core/row.cpp                                               \
    src/lib/soci/src/core/connection-parameters.cpp                             \
    src/lib/soci/src/core/procedure.cpp src/lib/soci/src/core/use-type.cpp

if USE_POSTGRES
bin_stellard_CPPFLAGS += -DUSE_POSTGRES
libsoci_la_CPPFLAGS += -I$(srcdir)/src/lib/soci/src/backends/postgresql \
                         $(libpq_CFLAGS)

libsoci_la_SOURCES +=                                           \
    src/lib/soci/src/backends/postgresql/standard-use-type.cpp  \
    src/lib/soci/src/backends/postgresql/session.cpp            \
    src/lib/soci/src/backends/postgresql/common.cpp             \
    src/lib/soci/src/backends/postgresql/vector-use-type.cpp    \
    src/lib/soci/src/backends/postgresql/error.cpp              \
    src/lib/soci/src/backends/postgresql/row-id.cpp             \
    src/lib/soci/src/backends/postgresql/factory.cpp            \
    src/lib/soci/src/backends/postgresql/vector-into-type.cpp   \
    src/lib/soci/src/backends/postgresql/statement.cpp          \
    src/lib/soci/src/backends/postgresql/blob.cpp               \
    src/lib/soci/src/backends/postgresql/standard-into-type.cpp
endif

if XDRPP_INTERNAL
XDRPP_DIR=src/lib/xdrpp
XDRPP=$(top_builddir)/$(XDRPP_DIR)/xdrpp/libxdrpp.a
bin_stellard_LDADD += $(XDRPP)
SUBDIRS += $(XDRPP_DIR)
$(XDRC) $(XDRPP):
	$(MAKE) -C $(top_builddir)/$(XDRPP_DIR)
endif

if LIBMEDIDA_INTERNAL
noinst_LTLIBRARIES += libmedida.la
libmedida_la_SOURCES = src/lib/libmedida/src/medida/metered_interface.h         \
    src/lib/libmedida/src/medida/types.h                                        \
    src/lib/libmedida/src/medida/histogram.cc                                   \
    src/lib/libmedida/src/medida/reporting/collectd_reporter.h                  \
    src/lib/libmedida/src/medida/reporting/collectd_reporter.cc                 \
    src/lib/libmedida/src/medida/reporting/util.cc                              \
    src/lib/libmedida/src/medida/reporting/console_reporter.h                   \
    src/lib/libmedida/src/medida/reporting/abstract_polling_reporter.cc         \
    src/lib/libmedida/src/medida/reporting/abstract_polling_reporter.h          \
    src/lib/libmedida/src/medida/reporting/json_reporter.cc                     \
    src/lib/libmedida/src/medida/reporting/json_reporter.h                      \
    src/lib/libmedida/src/medida/reporting/util.h                               \
    src/lib/libmedida/src/medida/reporting/console_reporter.cc                  \
    src/lib/libmedida/src/medida/medida.h                                       \
    src/lib/libmedida/src/medida/metrics_registry.cc                            \
    src/lib/libmedida/src/medida/meter.cc src/lib/libmedida/src/medida/timer.h  \
    src/lib/libmedida/src/medida/stats/ewma.cc                                  \
    src/lib/libmedida/src/medida/stats/uniform_sample.cc                        \
    src/lib/libmedida/src/medida/stats/snapshot.cc                              \
    src/lib/libmedida/src/medida/stats/uniform_sample.h                         \
    src/lib/libmedida/src/medida/stats/snapshot.h                               \
    src/lib/libmedida/src/medida/stats/sample.h                                 \
    src/lib/libmedida/src/medida/stats/exp_decay_sample.h                       \
    src/lib/libmedida/src/medida/stats/ewma.h                                   \
    src/lib/libmedida/src/medida/stats/exp_decay_sample.cc                      \
    src/lib/libmedida/src/medida/timer_context.h                                \
    src/lib/libmedida/src/medida/metric_interface.h                             \
    src/lib/libmedida/src/medida/histogram.h                                    \
    src/lib/libmedida/src/medida/counter.h                                      \
    src/lib/libmedida/src/medida/metric_processor.cc                            \
    src/lib/libmedida/src/medida/metrics_registry.h                             \
    src/lib/libmedida/src/medida/metric_name.cc                                 \
    src/lib/libmedida/src/medida/meter.h                                        \
    src/lib/libmedida/src/medida/metric_name.h                                  \
    src/lib/libmedida/src/medida/counter.cc                                     \
    src/lib/libmedida/src/medida/metric_processor.h                             \
    src/lib/libmedida/src/medida/timer_context.cc                               \
    src/lib/libmedida/src/medida/summarizable_interface.h                       \
    src/lib/libmedida/src/medida/sampling_interface.h                           \
    src/lib/libmedida/src/medida/timer.cc
libmedida_la_CPPFLAGS = -I$(srcdir)/src/lib/libmedida/src
bin_stellard_DEPENDENCIES += libmedida.la
bin_stellard_LDADD += libmedida.la
endif

CLEANFILES = $(BUILT_SOURCES)
GITDIR = $(top_srcdir)/.git
src/generated/StellardVersion.h: Makefile
	$(AM_V_GEN)mkdir -p src/generated; \
    if test -d "$(GITDIR)"; then                                            \
      (VERS="$$(GIT_DIR=$(GITDIR) git describe --always --dirty --tags)" && \
       echo "#define STELLARD_VERSION \"$$VERS\"") >$@;                     \
    else                                                                    \
       echo "#define STELLARD_VERSION \"unknown\"" >$@;                     \
    fi

src/generated/Stellar-types.h: src/xdr/Stellar-types.x src/generated/FBAXDR.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/Stellar-overlay.h: src/xdr/Stellar-overlay.x src/generated/Stellar-ledger.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/Stellar-ledger-entries.h: src/xdr/Stellar-ledger-entries.x src/generated/Stellar-types.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/Stellar-transaction.h: src/xdr/Stellar-transaction.x src/generated/Stellar-ledger-entries.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/Stellar-ledger.h: src/xdr/Stellar-ledger.x src/generated/Stellar-transaction.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/StellarXDR.h: src/overlay/StellarXDR.x src/generated/Stellar-overlay.h Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated; \
	$(XDRC) -hh -o $@ $<

src/generated/FBAXDR.h: src/fba/FBAXDR.x Makefile $(XDRC)
	$(AM_V_GEN)mkdir -p src/generated
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


libtool: $(LIBTOOL_DEPS)
	$(SHELL) ./config.status libtool

ACLOCAL_AMFLAGS = -I m4

README: README.md
	$(AM_V_GEN)cp $< $@
