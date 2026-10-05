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
#include "util/ProtocolVersion.h"
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
            // Re-close at the same close time, adding an ms remainder once
            // ms close times are active: minSeqAge/minSeqLedgerGap read
            // whole seconds and must behave identically against a
            // sub-second LCL
            closeLedgerOn(
                *app, app->getLedgerManager().getLastClosedLedgerNum() + 1,
                withMsCloseTime(*app, app->getLedgerManager()
                                          .getLastClosedLedgerHeader()
                                          .header.scpValue.closeTime));

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

#ifdef MS_CLOSE_TIME
TEST_CASE("minSeqAge under sub-second ledgers", "[tx][bumpsequence]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& lm = app->getLedgerManager();
    auto root = app->getRoot();

    // These test networks run the ms protocol from genesis
    REQUIRE(protocolVersionStartsFrom(
        lm.getLastClosedLedgerHeader().header.ledgerVersion,
        MS_CLOSE_TIME_PROTOCOL_VERSION));

    // Establish a known whole-second close time and a funded account
    TimePoint const T =
        lm.getLastClosedLedgerHeader().header.scpValue.closeTime + 2;
    closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, T);
    auto a1 = root->create("a1", lm.getLastMinBalance(3) + 100000);
    auto nextSeq = [&]() { return lm.getLastClosedLedgerNum() + 1; };

    // a1's sequence number moves in a sub-second ledger; seqTime records the
    // whole second only
    auto r0 = closeLedgerOn(*app, nextSeq(), makeConsensusTime(T, 100),
                            {a1.tx({payment(*root, 1)})});
    checkTx(0, r0, txSUCCESS);
    {
        LedgerTxn ltx(app->getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, a1.getPublicKey());
        REQUIRE(
            getAccountEntryExtensionV3(acc.current().data.account()).seqTime ==
            T);
    }

    PreconditionsV2 cond;
    cond.minSeqAge = 1;
    auto tx2 = transactionWithV2Precondition(*app, a1, 1, 100, cond);

    SECTION("sub-second ledger in the same whole second: age still 0")
    {
        closeLedgerOn(*app, nextSeq(), makeConsensusTime(T, 800));

        // We always round down to whole seconds, so a subsecond ledger should
        // not advance minSeqAge.
        LedgerTxn ltx(app->getLedgerTxnRoot());
        REQUIRE(
            !tx2->checkValidForTesting(app->getAppConnector(), ltx, 0, 0, 0));
        REQUIRE(tx2->getResultCode() == txBAD_MIN_SEQ_AGE_OR_GAP);
    }
    SECTION("whole-second ledger one second later: age requirement met")
    {
        closeLedgerOn(*app, nextSeq(), T + 1);
        auto r = closeLedger(*app, {tx2});
        checkTx(0, r, txSUCCESS);
    }
}
#endif // MS_CLOSE_TIME
