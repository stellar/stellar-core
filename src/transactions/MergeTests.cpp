// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerDelta.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MergeOpFrame.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("merge", "[tx][merge]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    ApplicationEditableVersion app(clock, cfg);
    app.start();

    // set up world
    // set up world
    auto root = TestAccount::createRoot(app);

    const int64_t assetMultiplier = 1000000;

    int64_t trustLineBalance = 100000 * assetMultiplier;
    int64_t trustLineLimit = trustLineBalance * 10;

    int64_t txfee = app.getLedgerManager().getTxFee();

    const int64_t minBalance =
        app.getLedgerManager().getMinBalance(5) + 20 * txfee;

    auto a1 = root.create("A", 2 * minBalance);

    SECTION("merge into self")
    {
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(a1.merge(a1), ex_ACCOUNT_MERGE_MALFORMED);
        });
    }

    SECTION("merge into non existent account")
    {
        for_all_versions(app, [&]{
            REQUIRE_THROWS_AS(a1.merge(getAccount("B").getPublicKey()),
                            ex_ACCOUNT_MERGE_NO_ACCOUNT);
        });
    }

    auto b1 = root.create("B", minBalance);
    auto gateway = root.create("gate", minBalance);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    SECTION("with create")
    {
        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();
        auto createBalance = app.getLedgerManager().getMinBalance(1);
        auto txFrame =
            a1.tx({createMergeOp(&a1.getSecretKey(), b1),
                   createCreateAccountOp(&b1.getSecretKey(), a1.getPublicKey(),
                                         createBalance),
                   createMergeOp(&a1.getSecretKey(), b1)});
        txFrame->addSignature(b1.getSecretKey());

        for_versions_to(5, app, [&]{
            applyCheck(txFrame, delta, app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[2]);

            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1.getBalance() ==
                    2 * a1Balance + b1Balance - createBalance -
                        2 * txFrame->getFee());
        });

        for_versions_from(6, app, [&]{
            applyCheck(txFrame, delta, app);

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[2]);

            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1.getBalance() ==
                    a1Balance + b1Balance - txFrame->getFee());
        });
    }

    SECTION("merge account twice")
    {
        int64 a1Balance = a1.getBalance();
        int64 b1Balance = b1.getBalance();

        auto txFrame =
            a1.tx({createMergeOp(nullptr, b1), createMergeOp(nullptr, b1)});

        for_versions_to(4, app, [&]{
            REQUIRE(applyCheck(txFrame, delta, app));

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            auto a1BalanceAfterFee = a1Balance - txFrame->getFee();
            REQUIRE(result == ACCOUNT_MERGE_SUCCESS);
            REQUIRE(b1Balance + a1BalanceAfterFee + a1BalanceAfterFee ==
                    b1.getBalance());
            REQUIRE(!loadAccount(a1, app, false));
        });

        for_versions(5, 7, app, [&]{
            REQUIRE(!applyCheck(txFrame, delta, app));

            auto result = MergeOpFrame::getInnerCode(
                txFrame->getResult().result.results()[1]);

            REQUIRE(result == ACCOUNT_MERGE_NO_ACCOUNT);
            REQUIRE(b1Balance == b1.getBalance());
            REQUIRE((a1Balance - txFrame->getFee()) ==
                    a1.getBalance());
        });

        for_versions_from(8, app, [&]{
            REQUIRE(!applyCheck(txFrame, delta, app));
            REQUIRE(txFrame->getResult().result.results()[1].code() == opNO_ACCOUNT);
        });
    }

    SECTION("Account has static auth flag set")
    {
        for_all_versions(app, [&]{
            uint32 flags = AUTH_IMMUTABLE_FLAG;
            a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr, nullptr);

            REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_IMMUTABLE_SET);
        });
    }

    SECTION("With sub entries")
    {
        Asset usdCur = makeAsset(gateway, "USD");
        a1.changeTrust(usdCur, trustLineLimit);

        SECTION("account has trust line")
        {
            for_all_versions(app, [&]{
                REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }
        SECTION("account has offer")
        {
            for_all_versions(app, [&]{
                gateway.pay(a1, usdCur, trustLineBalance);
                Asset xlmCur;
                xlmCur.type(AssetType::ASSET_TYPE_NATIVE);

                const Price somePrice(3, 2);
                for (int i = 0; i < 4; i++)
                {
                    a1.manageOffer(0, xlmCur, usdCur, somePrice,
                                    100 * assetMultiplier);
                }
                // empty out balance
                a1.pay(gateway, usdCur, trustLineBalance);
                // delete the trust line
                a1.changeTrust(usdCur, 0);

                REQUIRE_THROWS_AS(a1.merge(b1),
                                    ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }

        SECTION("account has data")
        {
            for_versions_from({2, 4}, app, [&]{
                // delete the trust line
                a1.changeTrust(usdCur, 0);

                DataValue value;
                value.resize(20);
                for (int n = 0; n < 20; n++)
                {
                    value[n] = (unsigned char)n;
                }

                std::string t1("test");

                a1.manageData(t1, &value);
                REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
            });
        }
    }

    SECTION("success")
    {
        SECTION("success - basic")
        {
            for_all_versions(app, [&]{
                a1.merge(b1);
                REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                                app.getDatabase()));
            });
        }
        SECTION("success, invalidates dependent tx")
        {
            for_all_versions(app, [&]{
                auto tx1 = createAccountMerge(app.getNetworkID(), a1, b1,
                                            a1.nextSequenceNumber());
                auto tx2 = createPaymentTx(app.getNetworkID(), a1, root,
                                        a1.nextSequenceNumber(), 100);
                int64 a1Balance = a1.getBalance();
                int64 b1Balance = b1.getBalance();
                auto r = closeLedgerOn(app, 2, 1, 1, 2015, {tx1, tx2});
                checkTx(0, r, txSUCCESS);
                checkTx(1, r, txNO_ACCOUNT);

                REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                                app.getDatabase()));

                int64 expectedB1Balance =
                    a1Balance + b1Balance - 2 * app.getLedgerManager().getTxFee();
                REQUIRE(expectedB1Balance == b1.getBalance());
            });
        }
    }
}
