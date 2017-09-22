// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/TrustLineQueries.h"
#include "ledgerdelta/LedgerDelta.h"
#include "ledger/LedgerEntries.h"
#include "ledger/TrustFrame.h"
#include "lib/catch.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app{clock, cfg};
    auto& entries = app.getLedgerEntries();

    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);
    auto const minBalance2 = app.getLedgerManager().getMinBalance(2);
    auto gateway = root.create("gw", minBalance2);
    Asset idr = makeAsset(gateway, "IDR");

    SECTION("basic tests")
    {
        for_all_versions(app, [&]{
            // create a trustline with a limit of 0
            REQUIRE_THROWS_AS(root.changeTrust(idr, 0),
                            ex_CHANGE_TRUST_INVALID_LIMIT);

            // create a trustline with a limit of 100
            root.changeTrust(idr, 100);

            // fill it to 90
            gateway.pay(root, idr, 90);

            // can't lower the limit below balance
            REQUIRE_THROWS_AS(root.changeTrust(idr, 89),
                            ex_CHANGE_TRUST_INVALID_LIMIT);

            // can't delete if there is a balance
            REQUIRE_THROWS_AS(root.changeTrust(idr, 0),
                            ex_CHANGE_TRUST_INVALID_LIMIT);

            // lower the limit at the balance
            root.changeTrust(idr, 90);

            // clear the balance
            root.pay(gateway, idr, 90);

            // delete the trust line
            root.changeTrust(idr, 0);
            REQUIRE(!(selectTrustLine(root.getPublicKey(), idr, entries.getDatabase())));
        });
    }
    SECTION("issuer does not exist")
    {
        SECTION("new trust line")
        {
            for_all_versions(app, [&]{
                Asset usd = makeAsset(getAccount("non-existing"), "IDR");
                REQUIRE_THROWS_AS(root.changeTrust(usd, 100),
                                ex_CHANGE_TRUST_NO_ISSUER);
            });
        }
        SECTION("edit existing")
        {
            for_all_versions(app, [&]{
                root.changeTrust(idr, 100);
                // Merge gateway back into root (the trustline still exists)
                gateway.merge(root);

                REQUIRE_THROWS_AS(root.changeTrust(idr, 99),
                                ex_CHANGE_TRUST_NO_ISSUER);
                REQUIRE(!loadAccount(gateway, app, false));
            });
        }
    }
    SECTION("trusting self")
    {
        auto idr = makeAsset(gateway, "IDR");
        auto loadTrustLine = [&]() {
            LedgerDelta ledgerDelta(app.getLedgerManager().getCurrentLedgerHeader(),
                                        app.getLedgerEntries());
            return TrustFrame{*ledgerDelta.loadTrustLine(gateway.getPublicKey(), idr)};
        };
        auto validateTrustLineIsConst = [&]() {
            auto trustLine = loadTrustLine();
            REQUIRE(trustLine.getBalance() == INT64_MAX);
        };

        validateTrustLineIsConst();

        for_versions_to(2, app, [&]{
            // create a trustline with a limit of INT64_MAX - 1 wil lfail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // create a trustline with a limit of INT64_MAX
            gateway.changeTrust(idr, INT64_MAX);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = AccountFrame{*loadAccount(gateway, app)};
            gateway.pay(gateway, idr, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = AccountFrame{*loadAccount(gateway, app)};
            REQUIRE(gatewayAccountAfter.getBalance() ==
                    (gatewayAccountBefore.getBalance() -
                     app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_INVALID_LIMIT);
            validateTrustLineIsConst();
        });

        for_versions_from(3, app, [&]{
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX - 1),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            REQUIRE_THROWS_AS(gateway.changeTrust(idr, INT64_MAX),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            auto gatewayAccountBefore = AccountFrame{*loadAccount(gateway, app)};
            gateway.pay(gateway, idr, 50);
            validateTrustLineIsConst();
            auto gatewayAccountAfter = AccountFrame{*loadAccount(gateway, app)};
            REQUIRE(gatewayAccountAfter.getBalance() ==
                    (gatewayAccountBefore.getBalance() -
                     app.getLedgerManager().getTxFee()));

            // lower the limit will fail, because it is still INT64_MAX
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 50),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();

            // delete the trust line will fail
            REQUIRE_THROWS_AS(gateway.changeTrust(idr, 0),
                              ex_CHANGE_TRUST_SELF_NOT_ALLOWED);
            validateTrustLineIsConst();
        });
    }
}