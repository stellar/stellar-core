// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("set trustline flags", "[tx][settrustlineflags]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    app->start();

    const int64_t trustLineLimit = INT64_MAX;
    const int64_t trustLineStartingBalance = 20000;

    auto const minBalance4 = app->getLedgerManager().getLastMinBalance(4);

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto gateway = root.create("gw", minBalance4);
    auto a1 = root.create("A1", minBalance4 + 10000);
    auto a2 = root.create("A2", minBalance4);

    auto idr = makeAsset(gateway, "IDR");
    auto native = makeNativeAsset();

    gateway.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

    // gateway is not auth required, so trustline will be authorized
    a1.changeTrust(idr, trustLineLimit);

    SetTrustLineFlagsArguments emptyFlag;

    SECTION("not supported before version 17")
    {
        for_versions_to(16, *app, [&] {
            REQUIRE_THROWS_AS(gateway.setTrustLineFlags(idr, a1, emptyFlag),
                              ex_opNOT_SUPPORTED);
        });
    }

    for_versions_from(17, *app, [&] {
        // this lambda is used to verify offers are not pulled in non-revoke
        // scenarios
        auto market = TestMarket{*app};
        auto setFlagAndCheckOffer =
            [&](Asset const& asset, TestAccount& trustor,
                txtest::SetTrustLineFlagsArguments const& arguments,
                bool addOffer = true) {
                if (addOffer)
                {
                    auto offer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(trustor,
                                               {native, asset, Price{1, 1}, 1});
                    });
                }

                // no offer should be deleted
                market.requireChanges({}, [&] {
                    gateway.setTrustLineFlags(asset, trustor, arguments);
                });
            };

        SECTION("small test")
        {
            gateway.pay(a1, idr, 5);
            a1.pay(gateway, idr, 1);

            auto flags =
                setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) |
                clearTrustLineFlags(AUTHORIZED_FLAG);

            setFlagAndCheckOffer(idr, a1, flags);

            REQUIRE_THROWS_AS(a1.pay(gateway, idr, trustLineStartingBalance),
                              ex_PAYMENT_SRC_NOT_AUTHORIZED);
        }

        SECTION("empty flags")
        {
            // verify that the setTrustLineFlags call is a noop
            auto flag = a1.getTrustlineFlags(idr);
            setFlagAndCheckOffer(idr, a1, emptyFlag);
            REQUIRE(flag == a1.getTrustlineFlags(idr));
        }

        SECTION("clear clawback")
        {
            gateway.setOptions(setFlags(AUTH_CLAWBACK_ENABLED_FLAG));
            a2.changeTrust(idr, trustLineLimit);
            gateway.pay(a2, idr, 100);

            gateway.clawback(a2, idr, 25);

            // clear the clawback flag and then try to clawback
            setFlagAndCheckOffer(
                idr, a2, clearTrustLineFlags(TRUSTLINE_CLAWBACK_ENABLED_FLAG));
            REQUIRE_THROWS_AS(gateway.clawback(a2, idr, 25),
                              ex_CLAWBACK_NOT_CLAWBACK_ENABLED);
        }

        SECTION("upgrade auth when not revocable")
        {
            SECTION("authorized -> authorized to maintain liabilities -> "
                    "authorized - with offers")
            {
                // authorized -> authorized to maintain liabilities
                auto maintainLiabilitiesflags =
                    setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) |
                    clearTrustLineFlags(AUTHORIZED_FLAG);

                setFlagAndCheckOffer(idr, a1, maintainLiabilitiesflags);

                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                // authorized to maintain liabilities -> authorized
                auto authorizedFlags =
                    setTrustLineFlags(AUTHORIZED_FLAG) |
                    clearTrustLineFlags(
                        AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG);
                setFlagAndCheckOffer(idr, a1, authorizedFlags, false);
            }

            SECTION("0 -> authorized")
            {
                gateway.denyTrust(idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                gateway.setTrustLineFlags(idr, a1,
                                          setTrustLineFlags(AUTHORIZED_FLAG));
            }

            SECTION("0 -> authorized to maintain liabilities")
            {
                gateway.denyTrust(idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                gateway.setTrustLineFlags(
                    idr, a1,
                    setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG));
            }
        }

        SECTION("errors")
        {
            SECTION("invalid state")
            {
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));
                a2.changeTrust(idr, trustLineLimit);

                SECTION("set maintain liabilities when authorized")
                {
                    gateway.setTrustLineFlags(
                        idr, a2, setTrustLineFlags(AUTHORIZED_FLAG));
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a2,
                            setTrustLineFlags(
                                AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_INVALID_STATE);
                }
                SECTION("set authorized when maintain liabilities")
                {
                    gateway.setTrustLineFlags(
                        idr, a2,
                        setTrustLineFlags(
                            AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG));
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a2, setTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_INVALID_STATE);
                }
            }

            SECTION("can't revoke")
            {
                // AllowTrustTests.cpp covers most cases.

                SECTION("authorized -> 0")
                {
                    gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1, clearTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_CANT_REVOKE);
                }
                SECTION("authorized to maintain liabilities -> 0")
                {
                    gateway.allowMaintainLiabilities(
                        idr, a1, TrustFlagOp::SET_TRUST_LINE_FLAGS);
                    gateway.setOptions(clearFlags(AUTH_REVOCABLE_FLAG));

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1,
                            clearTrustLineFlags(
                                AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_CANT_REVOKE);
                }
            }

            SECTION("no trust")
            {
                REQUIRE_THROWS_AS(gateway.setTrustLineFlags(idr, a2, emptyFlag),
                                  ex_SET_TRUST_LINE_FLAGS_NO_TRUST_LINE);
            }

            SECTION("malformed")
            {
                // invalid auth flags
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1, setTrustLineFlags(TRUSTLINE_AUTH_FLAGS)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't set clawback
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        setTrustLineFlags(TRUSTLINE_CLAWBACK_ENABLED_FLAG)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't use native asset
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(native, a1, emptyFlag),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // invalid asset
                auto invalidAssets = testutil::getInvalidAssets(gateway);
                for (auto const& asset : invalidAssets)
                {
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(asset, a1, emptyFlag),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                }

                {
                    // set and clear flags can't overlap
                    auto setFlag = setTrustLineFlags(AUTHORIZED_FLAG);
                    auto clearFlag = clearTrustLineFlags(TRUSTLINE_AUTH_FLAGS);
                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(idr, a1, setFlag | clearFlag),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                    REQUIRE_THROWS_AS(
                        gateway.setTrustLineFlags(
                            idr, a1,
                            setFlag | clearTrustLineFlags(AUTHORIZED_FLAG)),
                        ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                }

                // can't clear or set unsupported flags
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        setTrustLineFlags(MASK_TRUSTLINE_FLAGS_V17 + 1)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(
                        idr, a1,
                        clearTrustLineFlags(MASK_TRUSTLINE_FLAGS_V17 + 1)),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // can't operate on self
                REQUIRE_THROWS_AS(
                    gateway.setTrustLineFlags(idr, gateway, emptyFlag),
                    ex_SET_TRUST_LINE_FLAGS_MALFORMED);

                // source account is not issuer
                REQUIRE_THROWS_AS(a1.setTrustLineFlags(idr, gateway, emptyFlag),
                                  ex_SET_TRUST_LINE_FLAGS_MALFORMED);
            }
        }
    });
}
