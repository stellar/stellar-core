// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SignerKey.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/test/LoopbackPeer.h"
#include "test/Catch2.h"
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
    auto root = app->getRoot();
    auto& lm = app->getLedgerManager();
    // Establish non-zero ledger close time for the time-based tests.
    closeLedgerOn(*app, 1, 1, 2020);

    auto a = root->create("A", lm.getLastMinBalance(0) + 1000);
    auto b = root->create("B", lm.getLastMinBalance(0) + 1000);

    SECTION("test success")
    {
        for_versions_from(10, *app, [&]() {
            SECTION("small bump")
            {
                auto newSeq = a.getLastSequenceNumber() + 2;
                a.bumpSequence(newSeq);
                REQUIRE(a.getLastSequenceNumber() == newSeq);
            }
            SECTION("large bump")
            {
                auto newSeq = INT64_MAX;
                a.bumpSequence(newSeq);
                REQUIRE(a.getLastSequenceNumber() == newSeq);
                SECTION("no more tx when INT64_MAX is reached")
                {
                    REQUIRE_THROWS_AS(
                        applyTx(
                            {a.tx({payment(*root, 1)},
                                  std::numeric_limits<SequenceNumber>::min())},
                            *app),
                        ex_txBAD_SEQ);
                }
            }
            SECTION("backward jump (no-op)")
            {
                auto oldSeq = a.getLastSequenceNumber();
                a.bumpSequence(1);
                // tx consumes sequence, bumpSequence doesn't do anything
                REQUIRE(a.getLastSequenceNumber() == oldSeq + 1);
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
            REQUIRE(a.getLastSequenceNumber() == newSeq);

            // Right now the transaction validation is broken for this edge case
            // because it checks `isBadSeq` against the LCL ledger sequence,
            // instead of LCL+1 used during transaction application. Thus the
            // transaction can be included into ledger and fail with txBAD_SEQ
            // during application. This change also has been introduced
            // accidentally without a protocol gate, so we have a blanket test
            // for this behavior for now.
            // We should eventually fix this with a protocol guard; at that
            // point this check should be conditioned on protocols before the
            // fix.
            auto r = closeLedger(*app, {a.tx({payment(*root, 1)})});
            checkTx(0, r, txBAD_SEQ);
            REQUIRE(a.getLastSequenceNumber() == newSeq);
        });
    }

    SECTION("minSeq conditions fail due to bump sequence")
    {
        for_versions_from(19, *app, [&]() {
            // Consume the account's sequence number to stamp its `seqTime`
            // and `seqLedger`.
            a.pay(*root, 1);

            // Close two ledgers (sequence number is advanced twice), and set
            // the close time to 1 day from the initial close time.
            closeLedgerOn(*app, 2, 1, 2020);
            closeLedgerOn(*app, 2, 1, 2020);

            auto tx1 = transactionFrameFromOps(app->getNetworkID(), *root,
                                               {a.op(bumpSequence(0))}, {a});

            auto runTest = [&](PreconditionsV2 const& cond) {
                auto tx2 = transactionWithV2Precondition(*app, a, 1, 100, cond);
                // The precondition is satisfied against the last closed
                // ledger, i.e. the transaction is valid on its own.
                REQUIRE(tx2->checkValid(app->getAppConnector(),
                                        CheckValidLedgerViewWrapper(*app), 0, 0,
                                        0)
                            ->isSuccess());

                auto preTxSeqNum = a.getLastSequenceNumber();
                auto r = closeLedger(*app, {tx1, tx2}, true);

                // tx1 bumps the account's sequence number within the same
                // ledger, which resets the sequence and time stamps of the
                // account, and thus invalidates the sequence/time gap bounds.
                checkTx(0, r, txSUCCESS);
                checkTx(1, r, txBAD_MIN_SEQ_AGE_OR_GAP);

                // seq was consumed even though tx2 return
                // txBAD_MIN_SEQ_AGE_OR_GAP
                REQUIRE(a.getLastSequenceNumber() - 1 == preTxSeqNum);
            };

            SECTION("min minSeqLedgerGap")
            {
                PreconditionsV2 cond;
                cond.minSeqLedgerGap = 1;
                runTest(cond);
            }
            SECTION("max minSeqLedgerGap")
            {
                PreconditionsV2 cond;
                // Maximum valid gap that would pass validation (2 ledgers).
                cond.minSeqLedgerGap = 2;
                runTest(cond);
            }
            SECTION("min minSeqAge")
            {
                PreconditionsV2 cond;
                cond.minSeqAge = 1;
                runTest(cond);
            }
            SECTION("max minSeqAge")
            {
                PreconditionsV2 cond;
                // Maximum valid ledger age that would pass the validation (1
                // day from the last close).
                cond.minSeqAge = 24 * 3600;
                runTest(cond);
            }
        });
    }
}
