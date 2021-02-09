// Copyright 2016 Stellar Development Foundation and contributors. Licensed
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
#include "util/Timer.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("authorized to maintain liabilities", "[tx][allowtrust]")
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

    auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                 static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);

    gateway.setOptions(setFlags(toSet));

    auto native = makeNativeAsset();

    auto usd = makeAsset(gateway, "USD");

    a1.changeTrust(usd, trustLineLimit);
    gateway.allowTrust(usd, a1);

    auto idr = makeAsset(gateway, "IDR");

    a1.changeTrust(idr, trustLineLimit);
    gateway.allowTrust(idr, a1);

    gateway.pay(a1, usd, trustLineStartingBalance);
    gateway.pay(a1, idr, trustLineStartingBalance);

    auto market = TestMarket{*app};
    auto offer = market.requireChangesWithOffer({}, [&] {
        return market.addOffer(a1, {usd, idr, Price{1, 1}, 1000});
    });

    auto offerTest = [&](bool buyIsOnlyAllowedToMaintainLiabilities) {
        auto& maintainLiabilitiesAsset =
            buyIsOnlyAllowedToMaintainLiabilities ? idr : usd;

        market.requireChanges({}, [&] {
            gateway.allowMaintainLiabilities(maintainLiabilitiesAsset, a1);
        });

        SECTION("don't pull orders until denyTrust")
        {
            SECTION("denyTrust on buying asset")
            {
                market.requireChanges({{offer.key, OfferState::DELETED}},
                                      [&] { gateway.denyTrust(idr, a1); });
            }

            SECTION("denyTrust on selling asset")
            {
                market.requireChanges({{offer.key, OfferState::DELETED}},
                                      [&] { gateway.denyTrust(usd, a1); });
            }
        }

        SECTION("can't update offer")
        {
            SECTION("try updating amount")
            {
                OfferState updatedOffer = offer.state;
                SECTION("increase amount")
                {
                    ++updatedOffer.amount;
                }
                SECTION("decrease amount")
                {
                    --updatedOffer.amount;
                }

                if (buyIsOnlyAllowedToMaintainLiabilities)
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
                }
                else
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
                }
            }
            SECTION("try updating price")
            {
                OfferState updatedOffer = offer.state;
                SECTION("increase price")
                {
                    ++updatedOffer.price.n;
                }
                SECTION("decrease price")
                {
                    ++updatedOffer.price.d;
                }

                if (buyIsOnlyAllowedToMaintainLiabilities)
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
                }
                else
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
                }
            }
            SECTION("swap assets")
            {
                OfferState updatedOffer = offer.state;
                std::swap(updatedOffer.selling, updatedOffer.buying);

                if (buyIsOnlyAllowedToMaintainLiabilities)
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
                }
                else
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
                }
            }

            SECTION("change selling asset")
            {
                OfferState updatedOffer = offer.state;
                updatedOffer.selling = native;
                if (buyIsOnlyAllowedToMaintainLiabilities)
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
                }
                else
                {
                    market.updateOffer(a1, offer.key.offerID, updatedOffer);
                }
            }

            SECTION("change buying asset")
            {
                OfferState updatedOffer = offer.state;
                updatedOffer.buying = native;
                if (buyIsOnlyAllowedToMaintainLiabilities)
                {
                    market.updateOffer(a1, offer.key.offerID, updatedOffer);
                }
                else
                {
                    REQUIRE_THROWS_AS(
                        market.updateOffer(a1, offer.key.offerID, updatedOffer),
                        ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
                }
            }
        }

        SECTION("can't add offer")
        {
            OfferState offerState(usd, idr, Price{1, 1}, 1000);
            if (buyIsOnlyAllowedToMaintainLiabilities)
            {
                REQUIRE_THROWS_AS(market.addOffer(a1, offerState),
                                  ex_MANAGE_SELL_OFFER_BUY_NOT_AUTHORIZED);
            }
            else
            {
                REQUIRE_THROWS_AS(market.addOffer(a1, offerState),
                                  ex_MANAGE_SELL_OFFER_SELL_NOT_AUTHORIZED);
            }
        }

        SECTION("delete offer")
        {
            market.requireChanges({{offer.key, OfferState::DELETED}}, [&] {
                market.updateOffer(a1, offer.key.offerID,
                                   {usd, idr, Price{1, 1}, 0},
                                   OfferState::DELETED);
            });
        }
    };

    SECTION("allowMaintainLiabilities only works from version 12")
    {
        for_versions_to(12, *app, [&] {
            REQUIRE_THROWS_AS(gateway.allowMaintainLiabilities(idr, a1),
                              ex_ALLOW_TRUST_MALFORMED);
        });
    }

    SECTION("AUTHORIZED_FLAG and AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG can't "
            "be used together")
    {
        for_versions_from(13, *app, [&] {
            REQUIRE_THROWS_AS(
                gateway.allowTrust(idr, a1,
                                   AUTHORIZED_FLAG |
                                       AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG),
                ex_ALLOW_TRUST_MALFORMED);
        });
    }

    for_versions_from(13, *app, [&] {
        SECTION("offer tests")
        {
            SECTION("buying asset is only allowed to maintain liabilities")
            {
                offerTest(true);
            }
            SECTION("selling asset is only allowed to maintain liabilities")
            {
                offerTest(false);
            }
        }

        SECTION("payment tests")
        {
            market.requireChanges(
                {}, [&] { gateway.allowMaintainLiabilities(idr, a1); });

            SECTION("can't send payment")
            {
                REQUIRE_THROWS_AS(
                    a1.pay(gateway, idr, trustLineStartingBalance),
                    ex_PAYMENT_SRC_NOT_AUTHORIZED);
            }

            SECTION("can't receive payment")
            {
                a2.changeTrust(idr, trustLineLimit);
                gateway.allowTrust(idr, a2);
                gateway.pay(a2, idr, trustLineStartingBalance);

                REQUIRE_THROWS_AS(a2.pay(a1, idr, 1),
                                  ex_PAYMENT_NOT_AUTHORIZED);
            }
        }

        SECTION("auth transition tests")
        {
            auto issuer = root.create("issuer", minBalance4);
            issuer.setOptions(
                setFlags(static_cast<uint32_t>(AUTH_REQUIRED_FLAG)));

            auto iss = makeAsset(issuer, "iss");

            auto a3 = root.create("A3", minBalance4);
            a3.changeTrust(iss, trustLineLimit);

            SECTION("authorized -> authorized to maintain liabilities")
            {
                issuer.allowTrust(iss, a3);
                REQUIRE_THROWS_AS(issuer.allowMaintainLiabilities(iss, a3),
                                  ex_ALLOW_TRUST_CANT_REVOKE);
            }

            SECTION("authorized to maintain liabilities -> not authorized")
            {
                issuer.allowMaintainLiabilities(iss, a3);
                REQUIRE_THROWS_AS(issuer.denyTrust(iss, a3),
                                  ex_ALLOW_TRUST_CANT_REVOKE);
            }
        }
    });
}

TEST_CASE("allow trust", "[tx][allowtrust]")
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
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));
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
                gateway.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

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
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));

                a1.changeTrust(idr, trustLineLimit);
                REQUIRE_THROWS_AS(
                    gateway.pay(a1, idr, trustLineStartingBalance),
                    ex_PAYMENT_NOT_AUTHORIZED);

                gateway.allowTrust(idr, a1);
                gateway.pay(a1, idr, trustLineStartingBalance);
            }
            SECTION("invalid authorization flag")
            {
                REQUIRE_THROWS_AS(
                    gateway.allowTrust(
                        idr, a1, AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG + 1),
                    ex_ALLOW_TRUST_MALFORMED);
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
                gateway.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

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

            for_versions(3, 15, *app, [&] {
                REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                  ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
            });

            for_versions_from(16, *app, [&] {
                REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                  ex_ALLOW_TRUST_MALFORMED);
                REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                  ex_ALLOW_TRUST_MALFORMED);
            });
        }

        SECTION("allow trust without explicit trustline")
        {
            {
                gateway.setOptions(setFlags(AUTH_REQUIRED_FLAG));
            }
            SECTION("do not set revocable flag")
            {
                for_versions_to(2, *app, [&] {
                    gateway.allowTrust(idr, gateway);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_CANT_REVOKE);
                });

                for_versions(3, 15, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                });

                for_versions_from(16, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_MALFORMED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_MALFORMED);
                });
            }
            SECTION("set revocable flag")
            {
                gateway.setOptions(setFlags(AUTH_REVOCABLE_FLAG));

                for_versions_to(2, *app, [&] {
                    gateway.allowTrust(idr, gateway);
                    gateway.denyTrust(idr, gateway);
                });

                for_versions(3, 15, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_SELF_NOT_ALLOWED);
                });

                for_versions_from(16, *app, [&] {
                    REQUIRE_THROWS_AS(gateway.allowTrust(idr, gateway),
                                      ex_ALLOW_TRUST_MALFORMED);
                    REQUIRE_THROWS_AS(gateway.denyTrust(idr, gateway),
                                      ex_ALLOW_TRUST_MALFORMED);
                });
            }
        }
    }

    SECTION("allow trust with offers")
    {
        SECTION("an asset matches")
        {
            for_versions_from(10, *app, [&] {
                auto native = makeNativeAsset();

                auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                             static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                gateway.setOptions(setFlags(toSet));

                a1.changeTrust(idr, trustLineLimit);
                gateway.allowTrust(idr, a1);

                auto market = TestMarket{*app};
                SECTION("buying asset matches")
                {
                    auto offer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(
                            a1, {native, idr, Price{1, 1}, 1000});
                    });
                    market.requireChanges({{offer.key, OfferState::DELETED}},
                                          [&] { gateway.denyTrust(idr, a1); });
                }
                SECTION("selling asset matches")
                {
                    gateway.pay(a1, idr, trustLineStartingBalance);

                    auto offer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(
                            a1, {idr, native, Price{1, 1}, 1000});
                    });
                    market.requireChanges({{offer.key, OfferState::DELETED}},
                                          [&] { gateway.denyTrust(idr, a1); });
                }
            });
        }

        SECTION("neither asset matches")
        {
            for_versions_from(10, *app, [&] {
                auto toSet = static_cast<uint32_t>(AUTH_REQUIRED_FLAG) |
                             static_cast<uint32_t>(AUTH_REVOCABLE_FLAG);
                gateway.setOptions(setFlags(toSet));

                auto cur1 = makeAsset(gateway, "CUR1");
                auto cur2 = makeAsset(gateway, "CUR2");

                a1.changeTrust(idr, trustLineLimit);
                gateway.allowTrust(idr, a1);

                a1.changeTrust(cur1, trustLineLimit);
                gateway.allowTrust(cur1, a1);

                a1.changeTrust(cur2, trustLineLimit);
                gateway.allowTrust(cur2, a1);

                gateway.pay(a1, cur1, trustLineStartingBalance);

                auto market = TestMarket{*app};
                auto offer = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(a1, {cur1, cur2, Price{1, 1}, 1000});
                });
                market.requireChanges(
                    {{offer.key, {cur1, cur2, Price{1, 1}, 1000}}},
                    [&] { gateway.denyTrust(idr, a1); });
            });
        }
    }
}
