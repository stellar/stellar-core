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

static EndSponsoringFutureReservesResultCode
getEndSponsoringFutureReservesResultCode(TransactionFrameBasePtr& tx, size_t i)
{
    auto const& opRes = tx->getResult().result.results()[i];
    return opRes.tr().endSponsoringFutureReservesResult().code();
}

TEST_CASE("confirm and clear sponsor", "[tx][sponsorship]")
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
                app->getNetworkID(), root,
                {root.op(endSponsoringFutureReserves())}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!tx->checkValid(ltx, 0, 0, 0));

            REQUIRE(getOperationResultCode(tx, 0) == opNOT_SUPPORTED);
        });
    }

    SECTION("not sponsored")
    {
        for_versions_from(14, *app, [&] {
            auto a1 = root.create("a1", minBalance);
            auto tx = transactionFrameFromOps(
                app->getNetworkID(), root,
                {root.op(endSponsoringFutureReserves())}, {});

            LedgerTxn ltx(app->getLedgerTxnRoot());
            TransactionMeta txm(2);
            REQUIRE(tx->checkValid(ltx, 0, 0, 0));
            REQUIRE(!tx->apply(*app, ltx, txm));

            REQUIRE(tx->getResult().result.code() == txFAILED);
            REQUIRE(getEndSponsoringFutureReservesResultCode(tx, 0) ==
                    END_SPONSORING_FUTURE_RESERVES_NOT_SPONSORED);
        });
    }

    // END_SPONSORING_FUTURE_RESERVES success tested in
    // BeginSponsoringFutureReservesTests
}
