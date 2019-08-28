// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("fee bump", "[tx][feebump]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto& lm = app->getLedgerManager();
    auto reserve = lm.getLastReserve();
    auto fee = lm.getLastTxFee();

    auto root = TestAccount::createRoot(*app);

    auto closeLedger = [&](TransactionFrameBasePtr tx) {
        return closeLedgerOn(*app, lm.getLastClosedLedgerNum() + 1, 1, 7, 2014,
                             {tx});
    };

    SECTION("invalid in version 11")
    {
        for_versions({11}, *app, [&]() {
            auto accA = root.create("A", 2 * reserve);
            auto accB = root.create("B", 2 * reserve + 2 * fee);
            auto tx = accA.tx({payment(accA, Asset{}, 1)});
            auto fb = accB.feeBump(tx);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(!fb->checkValid(ltx, 0));
            REQUIRE(fb->getResultCode() == txNOT_SUPPORTED);
        });
    }

    SECTION("success in version 12")
    {
        for_versions({12}, *app, [&]() {
            auto accA = root.create("A", 2 * reserve);
            auto accB = root.create("B", 2 * reserve + 2 * fee);
            auto tx = accA.tx({payment(accA, Asset{}, 1)});
            auto res = closeLedger(accB.feeBump(tx));
            REQUIRE(res.size() == 2);
            REQUIRE(res[0].first.result.result.code() == txFEE_BUMPED);
            REQUIRE(res[1].first.result.result.code() == txSUCCESS);
            REQUIRE(accA.getBalance() == 2 * reserve);
            REQUIRE(accB.getBalance() == 2 * reserve);
        });
    }

    SECTION("inner transaction can have 0 fee")
    {
        for_versions_from(12, *app, [&]() {
            auto accA = root.create("A", 2 * reserve);
            auto accB = root.create("B", 2 * reserve + 2 * fee);
            auto tx = transactionFromOperations(*app, accA.getSecretKey(),
                                                accA.nextSequenceNumber(),
                                                {payment(accA, Asset{}, 1)}, 1);
            auto fb = accB.feeBump(tx);

            LedgerTxn ltx(app->getLedgerTxnRoot());
            REQUIRE(fb->checkValid(ltx, 0));
        });
    }

    SECTION("fee bump counts as an operation for fee calculations")
    {
        for_versions_from(12, *app, [&]() {
            auto accA = root.create("A", 2 * reserve);
            auto accB = root.create("B", 2 * reserve + 101 * fee);

            std::vector<Operation> ops;
            while (ops.size() < 100)
            {
                ops.emplace_back(payment(accA, Asset{}, 1));

                accA.loadSequenceNumber();
                auto tx = accA.tx(ops);
                auto fb1 = feeBumpFromTransaction(*app, accB.getSecretKey(), tx,
                                                  (ops.size() + 1) * fee);
                auto fb2 = feeBumpFromTransaction(*app, accB.getSecretKey(), tx,
                                                  (ops.size() + 1) * fee - 1);

                LedgerTxn ltx(app->getLedgerTxnRoot());
                REQUIRE(fb1->checkValid(ltx, 0));
                REQUIRE(!fb2->checkValid(ltx, 0));
            }
        });
    }
}
