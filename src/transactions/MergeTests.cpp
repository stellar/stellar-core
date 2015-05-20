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
#include "crypto/Base58.h"
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
TEST_CASE("merge", "[tx][merge]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    // set up world
    // set up world
    SecretKey root = getRoot();
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");
    SecretKey gateway = getAccount("gate");

    const int64_t currencyMultiplier = 1000000;

    int64_t trustLineBalance = 100000 * currencyMultiplier;
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

    Currency usdCur = makeCurrency(gateway, "USD");
    applyChangeTrust(app, a1, gateway, a1_seq++, "USD", trustLineLimit);
    applyCreditPaymentTx(app, gateway, a1, usdCur, gw_seq++, trustLineBalance);

    SECTION("account issued credits")
    {
        applyAccountMerge(app, gateway, a1, gw_seq++,
                          ACCOUNT_MERGE_CREDIT_HELD);
    }

    SECTION("account has balance")
    {
        applyAccountMerge(app, a1, b1, a1_seq++, ACCOUNT_MERGE_HAS_CREDIT);
    }

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());

    SECTION("success")
    {
        SECTION("with offers")
        {
            Currency xlmCur;
            xlmCur.type(CurrencyType::CURRENCY_TYPE_NATIVE);

            const Price somePrice(3, 2);
            for (int i = 0; i < 4; i++)
            {
                applyCreateOffer(app, delta, 0, a1, xlmCur, usdCur, somePrice,
                                 100 * currencyMultiplier, a1_seq++);
            }
        }

        // empty out balance
        applyCreditPaymentTx(app, a1, gateway, usdCur, a1_seq++,
                             trustLineBalance);

        applyAccountMerge(app, a1, b1, a1_seq++);
        REQUIRE(
            !AccountFrame::loadAccount(a1.getPublicKey(), app.getDatabase()));
    }
}
