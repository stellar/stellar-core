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
    ApplicationEditableVersion app{clock, cfg};
    auto& db = app.getDatabase();

    app.start();

    const int64_t assetMultiplier = 10000000;
    const int64_t trustLineLimit = INT64_MAX;
    const int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    auto const minBalance2 = app.getLedgerManager().getMinBalance(2);

    // set up world
    auto root = TestAccount::createRoot(app);
    auto gateway = root.create("gw", minBalance2);
    auto a1 = root.create("A1", minBalance2);
    auto a2 = root.create("A2", minBalance2);

    auto idrCur = makeAsset(gateway, "IDR");

    SECTION("allow trust not required")
    {
        REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, a1),
                          ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
        REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, a1),
                          ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
    }

    SECTION("allow trust without trustline")
    {
        auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
        applySetOptions(app, gateway, gateway.nextSequenceNumber(), nullptr,
                        &setFlags, nullptr, nullptr, nullptr, nullptr);

        SECTION("do not set revocable flag")
        {
            REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, a1),
                              ex_ALLOW_TRUST_NO_TRUST_LINE);
            REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, a1),
                              ex_ALLOW_TRUST_CANT_REVOKE);
        }
        SECTION("set revocable flag")
        {
            auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
            applySetOptions(app, gateway, gateway.nextSequenceNumber(), nullptr,
                            &setFlags, nullptr, nullptr, nullptr, nullptr);

            REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, a1),
                              ex_ALLOW_TRUST_NO_TRUST_LINE);
            REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, a1),
                              ex_ALLOW_TRUST_NO_TRUST_LINE);
        }
    }

    SECTION("allow trust not required with payment")
    {
        a1.changeTrust(idrCur, trustLineLimit);
        gateway.pay(a1, idrCur, trustLineStartingBalance);
        a1.pay(gateway, idrCur, trustLineStartingBalance);
    }

    SECTION("allow trust required")
    {
        auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
        applySetOptions(app, gateway, gateway.nextSequenceNumber(), nullptr,
                        &setFlags, nullptr, nullptr, nullptr, nullptr);

        a1.changeTrust(idrCur, trustLineLimit);
        REQUIRE_THROWS_AS(gateway.pay(a1, idrCur, trustLineStartingBalance),
                          ex_PAYMENT_NOT_AUTHORIZED);

        gateway.allowTrust(idrCur, a1);
        gateway.pay(a1, idrCur, trustLineStartingBalance);

        SECTION("do not set revocable flag")
        {
            REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, a1),
                              ex_ALLOW_TRUST_CANT_REVOKE);
            a1.pay(gateway, idrCur, trustLineStartingBalance);

            REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, a1),
                              ex_ALLOW_TRUST_CANT_REVOKE);
        }
        SECTION("set revocable flag")
        {
            auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
            applySetOptions(app, gateway, gateway.nextSequenceNumber(), nullptr,
                            &setFlags, nullptr, nullptr, nullptr, nullptr);

            gateway.denyTrust(idrCur, a1);
            REQUIRE_THROWS_AS(a1.pay(gateway, idrCur, trustLineStartingBalance),
                              ex_PAYMENT_SRC_NOT_AUTHORIZED);

            gateway.allowTrust(idrCur, a1);
            a1.pay(gateway, idrCur, trustLineStartingBalance);
        }
    }

    SECTION("self allow trust")
    {
        SECTION("protocol version 2")
        {
            app.getLedgerManager().setCurrentLedgerVersion(2);

            SECTION("allow trust not required")
            {
                REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, gateway),
                                  ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, gateway),
                                  ex_ALLOW_TRUST_TRUST_NOT_REQUIRED);
            }

            SECTION("allow trust without explicit trustline")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                applySetOptions(app, gateway, gateway.nextSequenceNumber(),
                                nullptr, &setFlags, nullptr, nullptr, nullptr,
                                nullptr);

                SECTION("do not set revocable flag")
                {
                    gateway.allowTrust(idrCur, gateway);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, gateway),
                                      ex_ALLOW_TRUST_CANT_REVOKE);
                }
                SECTION("set revocable flag")
                {
                    auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                    applySetOptions(app, gateway, gateway.nextSequenceNumber(),
                                    nullptr, &setFlags, nullptr, nullptr,
                                    nullptr, nullptr);

                    gateway.allowTrust(idrCur, gateway);
                    gateway.denyTrust(idrCur, gateway);
                }
            }
        }
        SECTION("protocol version 3")
        {
            app.getLedgerManager().setCurrentLedgerVersion(3);

            SECTION("allow trust not required")
            {
                REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
            }

            SECTION("allow trust without explicit trustline")
            {
                auto setFlags = static_cast<uint32_t>(AUTH_REQUIRED_FLAG);
                applySetOptions(app, gateway, gateway.nextSequenceNumber(),
                                nullptr, &setFlags, nullptr, nullptr, nullptr,
                                nullptr);

                SECTION("do not set revocable flag")
                {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                }
                SECTION("set revocable flag")
                {
                    auto setFlags = static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                    applySetOptions(app, gateway, gateway.nextSequenceNumber(),
                                    nullptr, &setFlags, nullptr, nullptr,
                                    nullptr, nullptr);

                    REQUIRE_THROWS_AS(gateway.allowTrust(idrCur, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idrCur, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                }
            }
        }
    }
}