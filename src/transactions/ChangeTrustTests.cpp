// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "util/TestUtils.h"
#include "lib/json/json.h"
#include "TxTests.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app{clock, cfg};
    Database& db = app.getDatabase();

    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey gateway = getAccount("gw");

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;

    SECTION("basic tests")
    {
        const int64_t minBalance2 = app.getLedgerManager().getMinBalance(2);

        applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
        SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

        Asset idrCur = makeAsset(gateway, "IDR");

        // create a trustline with a limit of 0
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 0,
                         CHANGE_TRUST_INVALID_LIMIT);

        // create a trustline with a limit of 100
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 100);

        // fill it to 90
        applyCreditPaymentTx(app, gateway, root, idrCur, gateway_seq++, 90);

        // can't lower the limit below balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 89,
                         CHANGE_TRUST_INVALID_LIMIT);
        // can't delete if there is a balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 0,
                         CHANGE_TRUST_INVALID_LIMIT);

        // lower the limit at the balance
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 90);

        // clear the balance
        applyCreditPaymentTx(app, root, gateway, idrCur, rootSeq++, 90);
        // delete the trust line
        applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 0);
        REQUIRE(!(TrustFrame::loadTrustLine(root.getPublicKey(), idrCur, db)));
    }
    SECTION("issuer does not exist")
    {
        SECTION("new trust line")
        {
            applyChangeTrust(app, root, gateway, rootSeq, "USD", 100,
                             CHANGE_TRUST_NO_ISSUER);
        }
        SECTION("edit existing")
        {
            const int64_t minBalance2 = app.getLedgerManager().getMinBalance(2);

            applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
            SequenceNumber gateway_seq = getAccountSeqNum(gateway, app) + 1;

            applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 100);
            // Merge gateway back into root (the trustline still exists)
            applyAccountMerge(app, gateway, root, gateway_seq++);

            applyChangeTrust(app, root, gateway, rootSeq++, "IDR", 99,
                             CHANGE_TRUST_NO_ISSUER);
        }
    }
    SECTION("trusting self")
    {
        SECTION("protocol version 2")
        {
            app.getLedgerManager().setCurrentLedgerVersion(2);

            auto const minBalance2 = app.getLedgerManager().getMinBalance(2);

            applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
            auto gateway_seq = getAccountSeqNum(gateway, app) + 1;

            auto idrCur = makeAsset(gateway, "IDR");
            auto loadTrustLine = [&](){ return TrustFrame::loadTrustLine(gateway.getPublicKey(), idrCur, db); };
            auto validateTrustLineIsConst = [&]()
            {
                auto trustLine = loadTrustLine();
                REQUIRE(trustLine);
                REQUIRE(trustLine->getBalance() == INT64_MAX);
            };

            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX - 1 wil lfail
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", INT64_MAX - 1,
                            CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", INT64_MAX);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway, app);
            applyCreditPaymentTx(app, gateway, gateway, idrCur, gateway_seq++, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway, app);
            REQUIRE(gatewayAccountAfter->getBalance() ==
                    (gatewayAccountBefore->getBalance() - app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", 50,
                            CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // delete the trust line will fail
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", 0,
                            CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();
        }
        SECTION("protocol version 3")
        {
            app.getLedgerManager().setCurrentLedgerVersion(3);

            auto const minBalance2 = app.getLedgerManager().getMinBalance(2);

            applyCreateAccountTx(app, root, gateway, rootSeq++, minBalance2);
            auto gateway_seq = getAccountSeqNum(gateway, app) + 1;

            auto idrCur = makeAsset(gateway, "IDR");
            auto loadTrustLine = [&](){ return TrustFrame::loadTrustLine(gateway.getPublicKey(), idrCur, db); };
            auto validateTrustLineIsConst = [&]()
            {
                auto trustLine = loadTrustLine();
                REQUIRE(trustLine);
                REQUIRE(trustLine->getBalance() == INT64_MAX);
            };

            validateTrustLineIsConst();

            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", INT64_MAX - 1,
                            CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", INT64_MAX,
                             CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = loadAccount(gateway, app);
            applyCreditPaymentTx(app, gateway, gateway, idrCur, gateway_seq++, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = loadAccount(gateway, app);
            REQUIRE(gatewayAccountAfter->getBalance() ==
                    (gatewayAccountBefore->getBalance() - app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", 50,
                            CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            // delete the trust line will fail
            applyChangeTrust(app, gateway, gateway, gateway_seq++, "IDR", 0,
                            CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();
        }
    }
}
