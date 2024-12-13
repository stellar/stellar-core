// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/test/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/XDROperators.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("bump sequence", "[tx][bumpsequence]")
{
    Config const& cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto& lm = app->getLedgerManager();

    auto a = root.create("A", lm.getLastMinBalance(0) + 1000);
    auto b = root.create("B", lm.getLastMinBalance(0) + 1000);

    SECTION("test success")
    {
        for_versions_from(10, *app, [&]() {
            SECTION("small bump")
            {
                auto newSeq = a.loadSequenceNumber() + 2;
                a.bumpSequence(newSeq);
                REQUIRE(a.loadSequenceNumber() == newSeq);
            }
            SECTION("large bump")
            {
                auto newSeq = INT64_MAX;
                a.bumpSequence(newSeq);
                REQUIRE(a.loadSequenceNumber() == newSeq);
                SECTION("no more tx when INT64_MAX is reached")
                {
                    REQUIRE_THROWS_AS(
                        applyTx(
                            {a.tx({payment(root, 1)},
                                  std::numeric_limits<SequenceNumber>::min())},
                            *app),
                        ex_txBAD_SEQ);
                }
            }
            SECTION("backward jump (no-op)")
            {
                auto oldSeq = a.loadSequenceNumber();
                a.bumpSequence(1);
                // tx consumes sequence, bumpSequence doesn't do anything
                REQUIRE(a.loadSequenceNumber() == oldSeq + 1);
            }
            SECTION("bad seq")
            {
                REQUIRE_THROWS_AS(a.bumpSequence(-1), ex_BUMP_SEQUENCE_BAD_SEQ);
                REQUIRE_THROWS_AS(a.bumpSequence(INT64_MIN),
                                  ex_BUMP_SEQUENCE_BAD_SEQ);
            }
        });
    }
    SECTION("not supported")
    {
        for_versions_to(9, *app, [&]() {
            REQUIRE_THROWS_AS(a.bumpSequence(1), ex_opNOT_SUPPORTED);
        });
    }

    SECTION("seqnum equals starting sequence")
    {
        for_versions_from(10, *app, [&]() {
            int64_t newSeq = 0;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto ledgerSeq = ltx.loadHeader().current().ledgerSeq + 2;
                newSeq = getStartingSequenceNumber(ledgerSeq) - 1;
            }

            a.bumpSequence(newSeq);
            REQUIRE(a.loadSequenceNumber() == newSeq);
            REQUIRE_THROWS_AS(applyTx({a.tx({payment(root, 1)})}, *app),
                              ex_txBAD_SEQ);
        });
    }

    SECTION("minSeq conditions fail due to bump sequence")
    {
        for_versions_from(19, *app, [&]() {
            closeLedger(*app);
            closeLedger(*app);

            auto tx1 = transactionFrameFromOps(app->getNetworkID(), root,
                                               {a.op(bumpSequence(0))}, {a});

            auto runTest = [&](PreconditionsV2 const& cond) {
                auto tx2 = transactionWithV2Precondition(*app, a, 1, 100, cond);

                auto preTxSeqNum = a.loadSequenceNumber();
                auto r = closeLedger(*app, {tx1, tx2}, true);

                checkTx(0, r, txSUCCESS);
                checkTx(1, r, txBAD_MIN_SEQ_AGE_OR_GAP);

                // seq was consumed even though tx2 return
                // txBAD_MIN_SEQ_AGE_OR_GAP
                REQUIRE(a.loadSequenceNumber() - 1 == preTxSeqNum);
            };

            SECTION("minSeqLedgerGap")
            {
                PreconditionsV2 cond;
                cond.minSeqLedgerGap = 1;
                runTest(cond);
            }

            SECTION("minSeqAge")
            {
                PreconditionsV2 cond;
                cond.minSeqAge = 1;
                runTest(cond);
            }
        });
    }
}
