// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionMetaFrame.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static OperationResultCode
getOperationResultCode(TransactionFrameBasePtr& tx, size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.code();
}

static BeginSponsoringFutureReservesResultCode
getBeginSponsoringFutureReservesResultCode(TransactionFrameBasePtr& tx,
                                           size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.tr().beginSponsoringFutureReservesResult().code();
}

TEST_CASE_VERSIONS("sponsor future reserves", "[tx][sponsorship]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto root = TestAccount::createRoot(*app);
    int64_t minBalance = app->getLedgerManager().getLastMinBalance(0);

    SECTION("not supported")
    {
        for_versions({13}, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root, {root.op(beginSponsoringFutureReserves(a1))}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!tx->checkValid(*app, ltx, 0, 0, 0));
            ltx.commit();

            REQUIRE(getOperationResultCode(tx, 0) == opNOT_SUPPORTED);
        });
    }

    SECTION("malformed")
    {
        for_versions_from(14, *app, [&] {
            auto tx = transactionFrameFromOps(
                *app, root, {root.op(beginSponsoringFutureReserves(root))}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!tx->checkValid(*app, ltx, 0, 0, 0));
            ltx.commit();

            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED);
        });
    }

    SECTION("already sponsored")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root,
                {root.op(beginSponsoringFutureReserves(a1)),
                 root.op(beginSponsoringFutureReserves(a1))},
                {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED);
        });
    }

    SECTION("bad sponsorship")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root, {root.op(beginSponsoringFutureReserves(a1))}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResultCode() == txBAD_SPONSORSHIP);
        });
    }

    SECTION("sponsoring account is sponsored")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto a2 = root.create("a2", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root,
                {root.op(beginSponsoringFutureReserves(a1)),
                 a1.op(beginSponsoringFutureReserves(a2)),
                 a2.op(endSponsoringFutureReserves()),
                 a1.op(endSponsoringFutureReserves())},
                {a1, a2});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        });
    }

    SECTION("sponsored account is sponsoring")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto a2 = root.create("a2", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root,
                {a1.op(beginSponsoringFutureReserves(a2)),
                 root.op(beginSponsoringFutureReserves(a1)),
                 a2.op(endSponsoringFutureReserves()),
                 a1.op(endSponsoringFutureReserves())},
                {a1, a2});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getBeginSponsoringFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        });
    }

    SECTION("success")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                *app, root,
                {root.op(beginSponsoringFutureReserves(a1)),
                 a1.op(endSponsoringFutureReserves())},
                {a1});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResultCode() == txSUCCESS);
        });
    }

    SECTION("add sponsored entry before adding first sponsored signer")
    {
        for_versions_from(14, *app, [&] {
            auto const minBalance0 =
                app->getLedgerManager().getLastMinBalance(0);
            auto a1 = root.create("a1", minBalance0);
            auto cur1 = makeAsset(root, "CUR1");
            auto signer = makeSigner(getAccount("S1"), 1);

            // creating the sponsored trustline first will make sure the account
            // is using a V2 extension before the first signer is added
            auto tx1 = transactionFrameFromOps(
                *app, root,
                {root.op(beginSponsoringFutureReserves(a1)),
                 a1.op(changeTrust(cur1, 1000)),
                 a1.op(endSponsoringFutureReserves())},
                {a1});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMetaFrame txm1(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx1->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx1->apply(*app, ltx, txm1));

            checkSponsorship(ltx, trustlineKey(a1, cur1), 1,
                             &root.getPublicKey());

            auto tx2 = transactionFrameFromOps(
                *app, root,
                {root.op(beginSponsoringFutureReserves(a1)),
                 a1.op(setOptions(setSigner(signer))),
                 a1.op(endSponsoringFutureReserves())},
                {a1});

            TransactionMetaFrame txm2(ltx.loadHeader().current().ledgerVersion);
            REQUIRE(tx2->checkValid(*app, ltx, 0, 0, 0));
            REQUIRE(tx2->apply(*app, ltx, txm2));

            checkSponsorship(ltx, a1.getPublicKey(), signer.key, 2,
                             &root.getPublicKey());
        });
    }

    SECTION("sponsorships with precondition that uses v3 extension")
    {
        auto a1 = root.create("a1", minBalance + 301);
        for_versions_from(19, *app, [&] {
            {
                auto cur1 = makeAsset(root, "CUR1");

                // creating the sponsored trustline first will make sure the
                // account is using a V2 extension
                auto tx = transactionFrameFromOps(
                    *app, root,
                    {root.op(beginSponsoringFutureReserves(a1)),
                     a1.op(changeTrust(cur1, 1000)),
                     a1.op(endSponsoringFutureReserves())},
                    {a1});

                closeLedger(*app, {tx});

                LedgerTxn ltx(app->getLedgerTxnRoot());
                checkSponsorship(ltx, trustlineKey(a1, cur1), 1,
                                 &root.getPublicKey());
            }

            // set seqLedger
            a1.bumpSequence(0);

            {
                PreconditionsV2 cond;
                cond.minSeqLedgerGap = 1;

                auto tx = transactionWithV2Precondition(*app, a1, 1, 100, cond);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(!tx->checkValid(*app, ltx, 0, 0, 0));
                    REQUIRE(tx->getResultCode() == txBAD_MIN_SEQ_AGE_OR_GAP);
                }

                {
                    // this increments ledgerSeq
                    closeLedger(*app);
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(tx->checkValid(*app, ltx, 0, 0, 0));
                    REQUIRE(tx->getResultCode() == txSUCCESS);
                }
            }
        });
    }
}
