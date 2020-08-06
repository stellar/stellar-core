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
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static OperationResultCode
getOperationResultCode(TransactionFrameBasePtr& tx, size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.code();
}

static BeginSponsoringFutureReservesResultCode
getSponsorFutureReservesResultCode(TransactionFrameBasePtr& tx, size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.tr().beginSponsoringFutureReservesResult().code();
}

TEST_CASE("sponsor future reserves", "[tx][sponsorship]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    auto root = TestAccount::createRoot(*app);
    int64_t minBalance = app->getLedgerManager().getLastMinBalance(0);

    SECTION("not supported")
    {
        for_versions({13}, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root, {root.op(sponsorFutureReserves(a1))},
                {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
            ltx.commit();

            REQUIRE(getOperationResultCode(tx, 0) == opNOT_SUPPORTED);
        });
    }

    SECTION("malformed")
    {
        for_versions_from(14, *app, [&] {
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(sponsorFutureReserves(root))}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));
            ltx.commit();

            REQUIRE(getSponsorFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_MALFORMED);
        });
    }

    SECTION("already sponsored")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root,
                                        {root.op(sponsorFutureReserves(a1)),
                                         root.op(sponsorFutureReserves(a1))},
                                        {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_ALREADY_SPONSORED);
        });
    }

    SECTION("bad sponsorship")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root, {root.op(sponsorFutureReserves(a1))},
                {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
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
            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root,
                                        {root.op(sponsorFutureReserves(a1)),
                                         a1.op(sponsorFutureReserves(a2)),
                                         a2.op(confirmAndClearSponsor()),
                                         a1.op(confirmAndClearSponsor())},
                                        {a1, a2});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        });
    }

    SECTION("sponsored account is sponsoring")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto a2 = root.create("a2", minBalance);
            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root,
                                        {a1.op(sponsorFutureReserves(a2)),
                                         root.op(sponsorFutureReserves(a1)),
                                         a2.op(confirmAndClearSponsor()),
                                         a1.op(confirmAndClearSponsor())},
                                        {a1, a2});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 0) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_SUCCESS);
            REQUIRE(getSponsorFutureReservesResultCode(tx, 1) ==
                    BEGIN_SPONSORING_FUTURE_RESERVES_RECURSIVE);
        });
    }

    SECTION("success")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx =
                transactionFrameFromOps(app->getNetworkID(), root,
                                        {root.op(sponsorFutureReserves(a1)),
                                         a1.op(confirmAndClearSponsor())},
                                        {a1});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(tx->apply(*app, ltx, txm));
            ltx.commit();

            REQUIRE(tx->getResultCode() == txSUCCESS);
        });
    }
}
