// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("allow trust", "[tx][allowtrust]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    const int64_t trustLineLimit = INT64_MAX;
    const int64_t trustLineStartingBalance = 20000;

    auto const minBalance2 = app->getLedgerManager().getMinBalance(2);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto gateway = root.create("gw", minBalance2);
    auto a1 = root.create("A1", minBalance2);
    auto a2 = root.create("A2", minBalance2);

    auto idr = makeAsset(gateway, "IDR");

    SECTION("allow trust not required")
    {
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(gateway.allowTrust(idr, a1),
                              ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
            REQUIRE_THROWS_AS(gateway.denyTrust(idr, a1),
                              ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
        });
    }

    SECTION("allow trust without trustline")
    {
        for_all_versions(*app, [&] {
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);
            }
            SECTION("do not set revocable flag")
            {
                REQUIRE_THROWS_AS(gateway.allowTrust(idr, a1),
                                  ex_ALLOW_TRUST_NO_TRUST_LINE);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, a1),
                                  ex_ALLOW_TRUST_CANT_REVOKE);
            }
            SECTION("set revocable flag")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);

                REQUIRE_THROWS_AS(gateway.allowTrust(idr, a1),
                                  ex_ALLOW_TRUST_NO_TRUST_LINE);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, a1),
                                  ex_ALLOW_TRUST_NO_TRUST_LINE);
            }
        });
    }

    SECTION("allow trust not required with payment")
    {
        for_all_versions(*app, [&] {
            a1.changeTrust(idr, trustLineLimit);
            gateway.pay(a1, idr, trustLineStartingBalance);
            a1.pay(gateway, idr, trustLineStartingBalance);
        });
    }

    SECTION("allow trust required")
    {
        for_all_versions(*app, [&] {
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);

                a1.changeTrust(idr, trustLineLimit);
                REQUIRE_THROWS_AS(
                    gateway.pay(a1, idr, trustLineStartingBalance),
                    ex_PAYMENT_NOT_AUTHORIZED);

                gateway.allowTrust(idr, a1);
                gateway.pay(a1, idr, trustLineStartingBalance);
            }
            SECTION("do not set revocable flag")
            {
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, a1),
                                  ex_ALLOW_TRUST_CANT_REVOKE);
                a1.pay(gateway, idr, trustLineStartingBalance);

                REQUIRE_THROWS_AS(gateway.denyTrust(idr, a1),
                                  ex_ALLOW_TRUST_CANT_REVOKE);
            }
            SECTION("set revocable flag")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);

                gateway.denyTrust(idr, a1);
                REQUIRE_THROWS_AS(
                    a1.pay(gateway, idr, trustLineStartingBalance),
                    ex_PAYMENT_SRC_NOT_AUTHORIZED);

                gateway.allowTrust(idr, a1);
                a1.pay(gateway, idr, trustLineStartingBalance);
            }
        });
    }

    SECTION("self allow trust")
    {
        SECTION("allow trust with trustline")
        {
            for_versions_to(2, *app, [&] {
                REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                  ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                  ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
            });

            for_versions_from(3, *app, [&] {
                REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
            });
        }

        SECTION("allow trust without explicit trustline")
        {
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);
            }
            SECTION("do not set revocable flag")
            {
                for_versions_to(2, *app, [&] {
                    gateway.allowTrust(idr, gateway);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_CANT_REVOKE);
                });

                for_versions_from(3, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                });
            }
            SECTION("set revocable flag")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                gateway.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                   nullptr, nullptr);

                for_versions_to(2, *app, [&] {
                    gateway.allowTrust(idr, gateway);
                    gateway.denyTrust(idr, gateway);
                });

                for_versions_from(3, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                });
            }
        }
    }
}
