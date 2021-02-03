// Copyright 2021 Stellar Development Foundation and contributors. Licensed
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
#include "transactions/TransactionUtils.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("clawback", "[tx][clawback]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    auto root = TestAccount::createRoot(*app);

    auto const minBalance3 = app->getLedgerManager().getLastMinBalance(3);
    auto a1 = root.create("A1", minBalance3);
    auto gateway = root.create("gw", minBalance3);
    auto idr = makeAsset(gateway, "IDR");
    auto native = makeNativeAsset();

    SECTION("all version errors")
    {
        for_all_versions(*app, [&]() {
            REQUIRE_THROWS_AS(
                gateway.allowTrust(idr, a1, TRUSTLINE_CLAWBACK_ENABLED_FLAG),
                ex_ALLOW_TRUST_MALFORMED);
        });
    }

    SECTION("pre V16 errors")
    {
        for_versions_to(15, *app, [&] {
            REQUIRE_THROWS_AS(
                gateway.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG)),
                ex_SET_OPTIONS_UNKNOWN_FLAG);
            REQUIRE_THROWS_AS(
                gateway.setOptions(clearFlags(AUTH_CLAWBACK_ENABLED_FLAG)),
                ex_SET_OPTIONS_UNKNOWN_FLAG);

            REQUIRE_THROWS_AS(gateway.clawback(a1, idr, 75),
                              ex_opNOT_SUPPORTED);
        });
    }

    SECTION("from V16")
    {
        for_versions_from(16, *app, [&] {
            auto toSet = static_cast<uint32_t>(AUTH_CLAWBACK_ENABLED_FLAG |
                                               AUTH_REVOCABLE_FLAG);
            gateway.setOptions(setFlags(toSet));

            a1.changeTrust(idr, 1000);
            gateway.pay(a1, idr, 100);

            SECTION("basic test")
            {
                gateway.clawback(a1, idr, 75);
                REQUIRE(a1.getTrustlineBalance(idr) == 25);
            }
            SECTION("clawback after removing liabilites")
            {
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));

                // clawback more than balance - liabilities (100-26)
                a1.manageOffer(0, idr, native, Price{1, 1}, 26);
                REQUIRE_THROWS_AS(gateway.clawback(a1, idr, 75),
                                  ex_CLAWBACK_UNDERFUNDED);

                REQUIRE(a1.getTrustlineBalance(idr) == 100);

                gateway.allowTrust(idr, a1, 0);

                gateway.clawback(a1, idr, 75);
                REQUIRE(a1.getTrustlineBalance(idr) == 25);
            }
            SECTION("allow trust")
            {
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));

                SECTION("allow trust can't clear clawback")
                {
                    // this shouldn't clear clawback
                    gateway.allowTrust(idr, a1, 0);

                    // we can clawback from a trustline that isn't authorized
                    gateway.clawback(a1, idr, 75);
                    REQUIRE(a1.getTrustlineBalance(idr) == 25);
                }
                SECTION("allow trust can't set clawback")
                {
                    REQUIRE_THROWS_AS(
                        gateway.allowTrust(idr, a1,
                                           TRUSTLINE_CLAWBACK_ENABLED_FLAG),
                        ex_ALLOW_TRUST_MALFORMED);

                    // show that TRUSTLINE_CLAWBACK_ENABLED_FLAG was the reason
                    // the above op failed
                    gateway.allowTrust(idr, a1, 0);
                }
            }

            SECTION("errors")
            {
                SECTION("set options")
                {
                    // can't clear revocable if AUTH_CLAWBACK_ENABLED_FLAG is
                    // set
                    REQUIRE_THROWS_AS(
                        gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG)),
                        ex_SET_OPTIONS_AUTH_REVOCABLE_REQUIRED);

                    // a1 doesn't have AUTH_REVOCABLE_FLAG set, so can't add
                    // AUTH_CLAWBACK_ENABLED_FLAG
                    REQUIRE_THROWS_AS(
                        a1.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG)),
                        ex_SET_OPTIONS_AUTH_REVOCABLE_REQUIRED);

                    // both can be removed together
                    gateway.setOptions(clearFlags(AUTH_CLAWBACK_ENABLED_FLAG |
                                                  AUTH_REVOCABLE_FLAG));
                }
                SECTION("set options clawback immutable")
                {
                    gateway.setOptions(setFlags(AUTH_IMMUTABLE_FLAG));

                    REQUIRE_THROWS_AS(gateway.setOptions(
                                          setFlags(AUTH_CLAWBACK_ENABLED_FLAG)),
                                      ex_SET_OPTIONS_CANT_CHANGE);

                    REQUIRE_THROWS_AS(
                        gateway.setOptions(clearFlags(
                            AUTH_CLAWBACK_ENABLED_FLAG | AUTH_REVOCABLE_FLAG)),
                        ex_SET_OPTIONS_CANT_CHANGE);
                }
                SECTION("check validity")
                {
                    // clawback from self
                    REQUIRE_THROWS_AS(gateway.clawback(gateway, idr, 75),
                                      ex_CLAWBACK_MALFORMED);

                    // invalid amount
                    REQUIRE_THROWS_AS(gateway.clawback(a1, idr, 0),
                                      ex_CLAWBACK_MALFORMED);
                    REQUIRE_THROWS_AS(gateway.clawback(a1, idr, -1),
                                      ex_CLAWBACK_MALFORMED);

                    // invalid asset
                    auto invalidAssets = testutil::getInvalidAssets(gateway);
                    for (auto const& asset : invalidAssets)
                    {
                        REQUIRE_THROWS_AS(gateway.clawback(a1, asset, 75),
                                          ex_CLAWBACK_MALFORMED);
                    }

                    // native asset
                    REQUIRE_THROWS_AS(gateway.clawback(a1, native, 75),
                                      ex_CLAWBACK_MALFORMED);

                    // source account is not the issuer
                    REQUIRE_THROWS_AS(a1.clawback(gateway, idr, 75),
                                      ex_CLAWBACK_MALFORMED);
                }
                SECTION("apply")
                {
                    SECTION("no trust")
                    {
                        REQUIRE_THROWS_AS(gateway.clawback(root, idr, 75),
                                          ex_CLAWBACK_NO_TRUST);
                    }
                    SECTION("not clawback enabled")
                    {
                        gateway.setOptions(
                            clearFlags(AUTH_CLAWBACK_ENABLED_FLAG));
                        root.changeTrust(idr, 100);
                        gateway.pay(root, idr, 100);
                        REQUIRE_THROWS_AS(gateway.clawback(root, idr, 75),
                                          ex_CLAWBACK_NOT_CLAWBACK_ENABLED);

                        REQUIRE(root.getTrustlineBalance(idr) == 100);
                    }
                    SECTION("underfunded")
                    {
                        // clawback more than trustline balance
                        REQUIRE_THROWS_AS(gateway.clawback(a1, idr, 101),
                                          ex_CLAWBACK_UNDERFUNDED);

                        // clawback more than balance - liabilities (100-26)
                        a1.manageOffer(0, idr, native, Price{1, 1}, 26);
                        REQUIRE_THROWS_AS(gateway.clawback(a1, idr, 75),
                                          ex_CLAWBACK_UNDERFUNDED);

                        REQUIRE(a1.getTrustlineBalance(idr) == 100);

                        // claim as much as possible
                        gateway.clawback(a1, idr, 74);
                        REQUIRE(a1.getTrustlineBalance(idr) == 26);
                    }
                }
            }
        });
    }
}