// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "ledger/LedgerDelta.h"

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
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    // set up world
    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");
    SecretKey gateway = getAccount("gate");

    const int64_t assetMultiplier = 1000000;

    int64_t trustLineBalance = 100000 * assetMultiplier;
    int64_t trustLineLimit = trustLineBalance * 10;

    int64_t txfee = app.getLedgerManager().getTxFee();

    const int64_t minBalance =
        app.getLedgerManager().getMinBalance(5) + 20 * txfee;

    SequenceNumber root_seq = getAccountSeqNum(root, app) + 1;

    applyCreateAccountTx(app, root, a1, root_seq++, minBalance);

    SequenceNumber a1_seq = getAccountSeqNum(a1, app) + 1;

    SECTION("merge into self")
    {
        applyAccountMerge(app, a1, a1, a1_seq++, ACCOUNT_MERGE_MALFORMED);
    }

    SECTION("merge into non existent account")
    {
        applyAccountMerge(app, a1, b1, a1_seq++, ACCOUNT_MERGE_NO_ACCOUNT);
    }

    applyCreateAccountTx(app, root, b1, root_seq++, minBalance);
    applyCreateAccountTx(app, root, gateway, root_seq++, minBalance);

    SequenceNumber gw_seq = getAccountSeqNum(gateway, app) + 1;

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    SECTION("Account has static auth flag set")
    {
        uint32 flags = AUTH_IMMUTABLE_FLAG;
        applySetOptions(app, a1, a1_seq++, nullptr, &flags, nullptr, nullptr,
                        nullptr, nullptr);

        applyAccountMerge(app, a1, b1, a1_seq++, ACCOUNT_MERGE_IMMUTABLE_SET);
    }

    SECTION("With sub entries")
    {
        Asset usdCur = makeAsset(gateway, "USD");
        applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);

        SECTION("account has trust line")
        {
            applyAccountMerge(app, a1, b1, a1_seq++,
                              ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        }
        SECTION("account has offer")
        {
            applyCreditPaymentTx(app, gateway, a1, usdCur, gw_seq++,
                                 trustLineBalance);
            Asset xlmCur;
            xlmCur.type(AssetType::ASSET_TYPE_NATIVE);

            const Price somePrice(3, 2);
            for (int i = 0; i < 4; i++)
            {
                applyCreateOffer(app, delta, 0, a1, xlmCur, usdCur, somePrice,
                                 100 * assetMultiplier, a1_seq++);
            }
            // empty out balance
            applyCreditPaymentTx(app, a1, gateway, usdCur, a1_seq++,
                                 trustLineBalance);
            // delete the trust line
            applyChangeTrust(app, a1, gateway, a1_seq++, "USD", 0);

            applyAccountMerge(app, a1, b1, a1_seq++,
                              ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        }

        SECTION("account has data")
        {
            // delete the trust line
            applyChangeTrust(app, a1, gateway, a1_seq++, "USD", 0);

            DataValue value;
            value.resize(20);
            for(int n = 0; n < 20; n++)
            {
                value[n] = (unsigned char)n;
            }

            std::string t1("test");

            applyManageData(app, a1, t1, &value, a1_seq++);
            applyAccountMerge(app, a1, b1, a1_seq++,
                ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        }
    }

    SECTION("success")
    {
        SECTION("success - basic")
        {
            applyAccountMerge(app, a1, b1, a1_seq++);
            REQUIRE(!AccountFrame::loadAccount(a1.getPublicKey(),
                                               app.getDatabase()));
        }
        SECTION("success, invalidates dependent tx")
        {
            auto tx1 = createAccountMerge(app.getNetworkID(), a1, b1, a1_seq++);
            auto tx2 =
                createPaymentTx(app.getNetworkID(), a1, root, a1_seq++, 100);
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
