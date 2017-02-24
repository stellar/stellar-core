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

    auto a1 = root.create("A", minBalance);

    SECTION("merge into self")
    {
        REQUIRE_THROWS_AS(a1.merge(a1), ex_ACCOUNT_MERGE_MALFORMED);
    }

    SECTION("merge into non existent account")
    {
        REQUIRE_THROWS_AS(a1.merge(getAccount("B").getPublicKey()),
                          ex_ACCOUNT_MERGE_NO_ACCOUNT);
    }

    auto b1 = root.create("B", minBalance);
    auto gateway = root.create("gate", minBalance);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    SECTION("Account has static auth flag set")
    {
        uint32 flags = AUTH_IMMUTABLE_FLAG;
        a1.setOptions(nullptr, &flags, nullptr, nullptr, nullptr, nullptr);

        REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_IMMUTABLE_SET);
    }

    SECTION("With sub entries")
    {
        Asset usdCur = makeAsset(gateway, "USD");
        a1.changeTrust(usdCur, trustLineLimit);

        SECTION("account has trust line")
        {
            REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        }
        SECTION("account has offer")
        {
            for (auto v : std::vector<int>{2, 3})
            {
                SECTION("protocol version " + std::to_string(v))
                {
                    app.getLedgerManager().setCurrentLedgerVersion(v);

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
                }
            }
        }

        SECTION("account has data")
        {
            app.getLedgerManager().setCurrentLedgerVersion(4);

            // delete the trust line
            a1.changeTrust(usdCur, 0);

            DataValue value;
            value.resize(20);
            for (int n = 0; n < 20; n++)
            {
                value[n] = (unsigned char)n;
            }

            std::string t1("test");

            applyManageData(app, a1, t1, &value, a1.nextSequenceNumber());
            REQUIRE_THROWS_AS(a1.merge(b1), ex_ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        }
    }

    SECTION("success")
    {
        SECTION("success - basic")
        {
            a1.merge(b1);
            REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                               app.getDatabase()));
        }
        SECTION("success, invalidates dependent tx")
        {
            auto tx1 = createAccountMerge(app.getNetworkID(), a1, b1,
                                          a1.nextSequenceNumber());
            auto tx2 = createPaymentTx(app.getNetworkID(), a1, root,
                                       a1.nextSequenceNumber(), 100);
            TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
                app.getLedgerManager().getLastClosedLedgerHeader().hash);
            txSet->add(tx1);
            txSet->add(tx2);
            txSet->sortForHash();
            REQUIRE(txSet->checkValid(app));
            int64 a1Balance = getAccountBalance(a1, app);
            int64 b1Balance = getAccountBalance(b1, app);
            auto r = closeLedgerOn(app, 2, 1, 1, 2015, txSet);
            checkTx(0, r, txSUCCESS);
            checkTx(1, r, txNO_ACCOUNT);

            REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                               app.getDatabase()));

            int64 expectedB1Balance =
                a1Balance + b1Balance - 2 * app.getLedgerManager().getTxFee();
            REQUIRE(expectedB1Balance == getAccountBalance(b1, app));
        }
    }
}
