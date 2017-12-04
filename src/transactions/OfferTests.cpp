// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "lib/util/uint128_t.h"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/OfferExchange.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/format.h"
#include "util/make_unique.h"

using namespace stellar;
using namespace stellar::txtest;

// Offer that takes multiple other offers and remains
// Offer selling XLM
// Offer buying XLM
// Offer with transfer rate
// Offer for more than you have
// Offer for something you can't hold
// Offer with line full (both accounts)

TEST_CASE("create offer", "[tx][offers]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);

    int64_t trustLineBalance = 100000;
    int64_t trustLineLimit = trustLineBalance * 10;

    int64_t txfee = app->getLedgerManager().getTxFee();

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app->getLedgerManager().getMinBalance(2) + 20 * txfee;

    // sets up issuer account
    auto issuer = root.create("issuer", minBalance2 * 10);
    auto xlm = makeNativeAsset();
    auto idr = issuer.asset("IDR");
    auto usd = issuer.asset("USD");

    const Price oneone(1, 1);

    SECTION("passive offer")
    {
        auto a1 = root.create("A", minBalance2 * 2);
        auto b1 = root.create("B", minBalance2 * 2);

        a1.changeTrust(idr, trustLineLimit);
        a1.changeTrust(usd, trustLineLimit);
        b1.changeTrust(idr, trustLineLimit);
        b1.changeTrust(usd, trustLineLimit);

        issuer.pay(a1, idr, trustLineBalance);
        issuer.pay(b1, usd, trustLineBalance);

        auto market = TestMarket{*app};
        auto firstOffer = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(a1, {idr, usd, oneone, 100});
        });
        auto secondOffer = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(b1,
                                   {usd, idr, oneone, 100, OfferType::PASSIVE});
        });

        SECTION("create a passive offer with a better price")
        {
            for_all_versions(*app, [&] {
                // firstOffer is taken, new offer was not created
                market.requireChangesWithOffer(
                    {{firstOffer.key, OfferState::DELETED}}, [&] {
                        return market.addOffer(
                            b1,
                            {usd, idr, Price{99, 100}, 100, OfferType::PASSIVE},
                            OfferState::DELETED);
                    });
            });
        }
        SECTION("modify existing passive offer with higher price")
        {
            for_all_versions(*app, [&] {
                market.requireChangesWithOffer({}, [&] {
                    return market.updateOffer(
                        b1, secondOffer.key.offerID,
                        {usd, idr, Price{100, 99}, 100, OfferType::PASSIVE});
                });
            });
        }

        SECTION("modify existing passive offer with lower price")
        {
            for_all_versions(*app, [&] {
                // firstOffer is taken with updated offer
                market.requireChangesWithOffer(
                    {{firstOffer.key, OfferState::DELETED}}, [&] {
                        return market.updateOffer(
                            b1, secondOffer.key.offerID,
                            {usd, idr, Price{99, 100}, 100, OfferType::PASSIVE},
                            OfferState::DELETED);
                    });
            });
        }
    }

    SECTION("create offer errors")
    {
        auto market = TestMarket{*app};

        SECTION("create offer without account")
        {
            auto a1 = TestAccount{*app, getAccount("a1"), 1};
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_txNO_ACCOUNT);
            });
        }

        SECTION("create offer without trustline for selling")
        {
            auto a1 = root.create("A", minBalance2);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_SELL_NO_TRUST);
            });
        }

        SECTION("create offer without issuer for selling")
        {
            auto a1 = root.create("A", minBalance2);
            auto fakeIssuer = getAccount("fakeIssuer");
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(market.requireChangesWithOffer(
                                      {},
                                      [&] {
                                          return market.addOffer(
                                              a1, {makeAsset(fakeIssuer, "IDR"),
                                                   usd, oneone, 100});
                                      }),
                                  ex_MANAGE_OFFER_SELL_NO_ISSUER);
            });
        }

        SECTION("create offer without having any amount of asset")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_UNDERFUNDED);
            });
        }

        SECTION("create offer without trustline for buying")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            issuer.pay(a1, idr, trustLineLimit);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_BUY_NO_TRUST);
            });
        }

        SECTION("create offer without issuer for buying")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            issuer.pay(a1, idr, trustLineLimit);
            auto fakeIssuer = getAccount("fakeIssuer");
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(market.requireChangesWithOffer(
                                      {},
                                      [&] {
                                          return market.addOffer(
                                              a1, {idr,
                                                   makeAsset(fakeIssuer, "USD"),
                                                   oneone, 100});
                                      }),
                                  ex_MANAGE_OFFER_BUY_NO_ISSUER);
            });
        }

        SECTION("create offer without XLM to make for reserve")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            a1.changeTrust(usd, trustLineLimit);
            issuer.pay(a1, idr, trustLineLimit);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_LOW_RESERVE);
            });
        }

        SECTION("create offer with trustline filled up")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            a1.changeTrust(usd, trustLineLimit);
            issuer.pay(a1, idr, trustLineLimit);
            issuer.pay(a1, usd, trustLineLimit);
            root.pay(a1, minBalance2);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_LINE_FULL);
            });
        }

        SECTION("create offer with trustline filled up to INT64_MAX")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            a1.changeTrust(usd, INT64_MAX);
            issuer.pay(a1, idr, trustLineLimit);
            issuer.pay(a1, usd, INT64_MAX);
            root.pay(a1, minBalance2);
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 100});
                        }),
                    ex_MANAGE_OFFER_LINE_FULL);
            });
        }

        SECTION("create offer with amount 0")
        {
            auto a1 = root.create("A", minBalance2);
            a1.changeTrust(idr, trustLineLimit);
            a1.changeTrust(usd, trustLineLimit);
            issuer.pay(a1, idr, trustLineLimit);
            issuer.pay(a1, usd, trustLineLimit);
            root.pay(a1, minBalance2);
            for_versions_to(2, *app, [&] {
                market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(a1, {idr, usd, oneone, 0},
                                           OfferState::DELETED);
                });
            });
            for_versions_from(3, *app, [&] {
                REQUIRE_THROWS_AS(
                    market.requireChangesWithOffer(
                        {},
                        [&] {
                            return market.addOffer(a1, {idr, usd, oneone, 0});
                        }),
                    ex_MANAGE_OFFER_NOT_FOUND);
            });
        }

        SECTION("create offer with invalid prices")
        {
            auto invalidPrices = std::vector<Price>{
                Price{-1, -1}, Price{-1, 1}, Price{0, -1}, Price{-1, 0},
                Price{0, 0},   Price{0, 1},  Price{1, -1}, Price{1, 0}};
            for_all_versions(*app, [&] {
                auto a = root.create("A", minBalance2 * 2);
                a.changeTrust(idr, trustLineLimit);
                for (auto const& p : invalidPrices)
                {
                    REQUIRE_THROWS_AS(
                        market.requireChangesWithOffer(
                            {},
                            [&] {
                                return market.addOffer(a, {xlm, idr, p, 150});
                            }),
                        ex_MANAGE_OFFER_MALFORMED);
                }
            });
        }
    }

    SECTION("update offer")
    {
        auto const minBalanceA = app->getLedgerManager().getMinBalance(3);
        auto a1 = root.create("A", minBalanceA + 10000);
        a1.changeTrust(usd, trustLineLimit);
        a1.changeTrust(idr, trustLineLimit);
        issuer.pay(a1, idr, trustLineBalance);

        auto market = TestMarket{*app};
        auto offer = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(a1, {idr, usd, oneone, 100});
        });
        auto cancelCheck = [&]() {
            market.requireChangesWithOffer({}, [&] {
                return market.updateOffer(a1, offer.key.offerID,
                                          {idr, usd, oneone, 0},
                                          OfferState::DELETED);
            });
        };

        SECTION("cancel offer")
        {
            for_all_versions(*app, [&] { cancelCheck(); });
        }

        SECTION("cancel offer with empty selling trust line")
        {
            for_all_versions(*app, [&] {
                a1.pay(issuer, idr, trustLineBalance);
                cancelCheck();
            });
        }

        SECTION("cancel offer with deleted selling trust line")
        {
            for_all_versions(*app, [&] {
                a1.pay(issuer, idr, trustLineBalance);
                a1.changeTrust(idr, 0);
                cancelCheck();
            });
        }

        SECTION("cancel offer with full buying trust line")
        {
            for_all_versions(*app, [&] {
                issuer.pay(a1, usd, trustLineLimit);
                cancelCheck();
            });
        }

        SECTION("cancel offer with deleted buying trust line")
        {
            for_all_versions(*app, [&] {
                a1.changeTrust(usd, 0);
                cancelCheck();
            });
        }

        SECTION("update price")
        {
            for_all_versions(*app, [&] {
                market.requireChangesWithOffer({}, [&] {
                    return market.updateOffer(a1, offer.key.offerID,
                                              {idr, usd, Price{1, 2}, 100});
                });
            });
        }
        SECTION("update amount")
        {
            for_all_versions(*app, [&] {
                market.requireChangesWithOffer({}, [&] {
                    return market.updateOffer(a1, offer.key.offerID,
                                              {idr, usd, oneone, 10});
                });
            });
        }
        SECTION("update selling/buying assets")
        {
            for_all_versions(*app, [&] {
                // needs usd
                issuer.pay(a1, usd, trustLineBalance);
                market.requireChangesWithOffer({}, [&] {
                    return market.updateOffer(a1, offer.key.offerID,
                                              {usd, idr, oneone, 10});
                });
            });
        }

        SECTION("update non existent offer")
        {
            auto bogusOfferID = offer.key.offerID + 1;
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(market.requireChangesWithOffer(
                                      {},
                                      [&] {
                                          return market.updateOffer(
                                              a1, bogusOfferID,
                                              {idr, usd, oneone, 100});
                                      }),
                                  ex_MANAGE_OFFER_NOT_FOUND);
            });
        }

        SECTION("delete non existent offer")
        {
            auto bogusOfferID = offer.key.offerID + 1;
            for_all_versions(*app, [&] {
                REQUIRE_THROWS_AS(market.requireChangesWithOffer(
                                      {},
                                      [&] {
                                          return market.updateOffer(
                                              a1, bogusOfferID,
                                              {idr, usd, oneone, 0});
                                      }),
                                  ex_MANAGE_OFFER_NOT_FOUND);
            });
        }
    }

    SECTION("create offer")
    {
        auto const nbOffers = 22;
        auto const minBalanceA =
            app->getLedgerManager().getMinBalance(3 + nbOffers);
        auto const minBalance3 = app->getLedgerManager().getMinBalance(3);
        auto a1 = root.create("A", minBalanceA + 10000);
        a1.changeTrust(usd, trustLineLimit);
        a1.changeTrust(idr, trustLineLimit);
        issuer.pay(a1, idr, trustLineBalance);

        auto market = TestMarket{*app};

        SECTION("idr -> xlm")
        {
            for_all_versions(*app, [&] {
                market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(a1, {xlm, idr, Price{3, 2}, 100});
                });
            });
        }

        SECTION("xlm -> idr")
        {
            SECTION("create")
            {
                for_all_versions(*app, [&] {
                    market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(a1,
                                               {idr, xlm, Price{3, 2}, 100});
                    });
                });
            }
            SECTION("crossing + create")
            {
                int64 a1IDrs = trustLineBalance;

                // a1 is selling a1IDrs idr
                auto a1Offer = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(a1, {idr, xlm, Price{1, 1}, a1IDrs});
                });

                auto checkCrossed = [&](TestAccount& b1, int64 actualPayment,
                                        int64 offerAmount) {
                    auto b1Before = b1.getBalance();
                    auto a1Before = a1.getBalance();
                    auto b1Offer =
                        market.addOffer(b1, {xlm, idr, oneone, offerAmount},
                                        OfferState::DELETED);
                    market.requireBalances(
                        {{a1,
                          {{xlm, a1Before + actualPayment},
                           {idr, trustLineBalance - actualPayment}}},
                         {b1,
                          {{xlm, b1Before - txfee - actualPayment},
                           {idr, actualPayment}}}});
                };
                SECTION("small offer amount - cross only")
                {
                    auto base0 = app->getLedgerManager().getMinBalance(1);

                    auto offerAmount = 1000;

                    int64 bStartingBalance;
                    bStartingBalance = base0;
                    // changetrust + manageoffer
                    bStartingBalance += txfee * 2;
                    bStartingBalance += offerAmount;

                    auto b1 = root.create("B", bStartingBalance);
                    b1.changeTrust(idr, 1000000000000000000ll);

                    for_versions_to(8, *app, [&]() {
                        checkCrossed(b1, offerAmount, offerAmount);
                    });

                    for_versions_from(9, *app, [&]() {
                        // would need at least base1 to do anything
                        REQUIRE_THROWS_AS(
                            market.requireChangesWithOffer(
                                {},
                                [&] {
                                    return market.addOffer(
                                        b1, {xlm, idr, oneone, offerAmount});
                                }),
                            ex_MANAGE_OFFER_LOW_RESERVE);
                    });
                }
                SECTION("large amount (oversell) - cross & create")
                {
                    auto const base2 = app->getLedgerManager().getMinBalance(2);

                    const int64 delta = 100;
                    const int64 payment = 1000;
                    auto offerAmount = a1IDrs + payment;

                    int64 bStartingBalance;
                    // we would have 2 subentries after creating an offer
                    bStartingBalance = base2;
                    // changetrust + manageoffer
                    bStartingBalance += txfee * 2;
                    bStartingBalance += a1IDrs - delta;

                    auto b1 = root.create("B", bStartingBalance);
                    b1.changeTrust(idr, 1000000000000000000ll);

                    for_versions_to(8, *app, [&]() {
                        // in v1..8
                        // oversell scenario is:
                        // endBalance = start - 2*txfee - actualPayment
                        // with actualPayment == a1IDrs (so that it needs to
                        // create an offer)
                        // endBalance < base2  ==> can't create offer

                        REQUIRE_THROWS_AS(
                            market.requireChangesWithOffer(
                                {},
                                [&] {
                                    return market.addOffer(
                                        b1, {xlm, idr, oneone, offerAmount});
                                }),
                            ex_MANAGE_OFFER_LOW_RESERVE);
                    });

                    for_versions_from(9, *app, [&]() {
                        // in v9, we sell as much as possible above base1
                        // endBalance = base1
                        // actualPayment = start - 2*txfee - endBalance
                        //    = base2 + 2*txfee + a1IDrs - delta - 2*txfee -
                        //                                        base2
                        //    = a1IDrs - delta
                        auto actualPayment = a1IDrs - delta;
                        checkCrossed(b1, actualPayment, offerAmount);
                    });
                }
            }
        }
        SECTION("multiple offers")
        {
            auto b1 = root.create("B", minBalance3 + 10000);
            b1.changeTrust(idr, trustLineLimit);
            b1.changeTrust(usd, trustLineLimit);

            auto const price = Price{3, 2};
            auto exactCrossPrice = Price{2, 3};
            auto offerState = OfferState{idr, usd, price, 100};
            auto offers = std::vector<TestMarketOffer>{};
            for (auto i = 0; i < nbOffers; i++)
            {
                offers.push_back(market.requireChangesWithOffer(
                    {}, [&] { return market.addOffer(a1, offerState); }));
            }

            SECTION("offer does not cross")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(b1, usd, 20000);
                    // offer is sell 40 USD for 80 IDR ; sell USD @ 2
                    market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(b1, {usd, idr, Price{2, 1}, 40});
                    });
                });
            }

            SECTION("offer crosses own")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(a1, usd, 20000);

                    // ensure we could receive proceeds from the offer
                    a1.pay(issuer, idr, 100000);

                    // offer is sell 150 USD for 100 IDR; sell USD @ 1.5 /
                    // buy IRD @ 0.66
                    REQUIRE_THROWS_AS(
                        market.requireChangesWithOffer(
                            {},
                            [&] {
                                return market.addOffer(
                                    a1, {usd, idr, exactCrossPrice, 150});
                            }),
                        ex_MANAGE_OFFER_CROSS_SELF);
                });
            }

            SECTION("offer crosses and removes first")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(b1, usd, 20000);

                    // offer is sell 150 USD for 100 USD; sell USD @ 1.5 /
                    // buy IRD @ 0.66
                    market.requireChangesWithOffer(
                        {{offers[0].key, OfferState::DELETED}}, [&] {
                            return market.addOffer(
                                b1, {usd, idr, exactCrossPrice, 150},
                                OfferState::DELETED);
                        });
                });
            }

            SECTION("offer crosses, removes first six and changes seventh")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(b1, usd, 20000);

                    market.requireBalances({{a1, {{usd, 0}, {idr, 100000}}},
                                            {b1, {{usd, 20000}, {idr, 0}}}});

                    // Offers are: sell 100 IDR for 150 USD; sell IRD @ 0.66
                    // -> buy USD @ 1.5
                    // first 6 offers get taken for 6*150=900 USD, gets 600
                    // IDR in return
                    // offer #7 : has 110 USD available
                    //    -> can claim partial offer 100*110/150 = 73.333 ;
                    //    -> 26.66666 left
                    // 8 .. untouched
                    // the USDs were sold at the (better) rate found in the
                    // original offers

                    // offer is sell 1010 USD for 505 IDR; sell USD @ 0.5
                    market.requireChangesWithOffer(
                        {{offers[0].key, OfferState::DELETED},
                         {offers[1].key, OfferState::DELETED},
                         {offers[2].key, OfferState::DELETED},
                         {offers[3].key, OfferState::DELETED},
                         {offers[4].key, OfferState::DELETED},
                         {offers[5].key, OfferState::DELETED},
                         {offers[6].key, {idr, usd, price, 27}}},
                        [&] {
                            return market.addOffer(
                                b1, {usd, idr, Price{1, 2}, 1010},
                                OfferState::DELETED);
                        });

                    market.requireBalances({{a1, {{usd, 1010}, {idr, 99327}}},
                                            {b1, {{usd, 18990}, {idr, 673}}}});
                });
            }

            SECTION("offer crosses, removes first six and changes seventh and "
                    "then remains")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(b1, usd, 20000);

                    market.requireBalances({{a1, {{usd, 0}, {idr, 100000}}},
                                            {b1, {{usd, 20000}, {idr, 0}}}});

                    auto c1 = root.create("C", minBalance3 + 10000);

                    // inject also an offer that should get cleaned up
                    c1.changeTrust(idr, trustLineLimit);
                    c1.changeTrust(usd, trustLineLimit);
                    issuer.pay(c1, idr, 20000);

                    // matches the offer from A
                    auto cOffer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(c1, {idr, usd, price, 100});
                    });
                    // drain account
                    c1.pay(issuer, idr, 20000);
                    // offer should still be there
                    market.checkCurrentOffers();

                    // offer is sell 10000 USD for 5000 IDR; sell USD @ 0.5

                    auto usdBalanceForSale = 10000;
                    auto usdBalanceRemaining = 6700;
                    auto offerPosted =
                        OfferState{usd, idr, Price{1, 2}, usdBalanceForSale};
                    auto offerRemaining =
                        OfferState{usd, idr, Price{1, 2}, usdBalanceRemaining};
                    auto removed = std::vector<TestMarketOffer>{};
                    for (auto o : offers)
                    {
                        removed.push_back({o.key, OfferState::DELETED});
                    }
                    // c1 has no idr to support that offer
                    removed.push_back({cOffer.key, OfferState::DELETED});
                    auto offer = market.requireChangesWithOffer(removed, [&] {
                        return market.addOffer(b1, offerPosted, offerRemaining);
                    });

                    market.requireBalances({{a1, {{usd, 3300}, {idr, 97800}}},
                                            {b1, {{usd, 16700}, {idr, 2200}}}});
                });
            }

            SECTION("multiple offers with small amount crosses")
            {
                for_all_versions(*app, [&] {
                    issuer.pay(b1, usd, 20000);

                    market.requireBalances({{a1, {{usd, 0}, {idr, 100000}}},
                                            {b1, {{usd, 20000}, {idr, 0}}}});

                    auto offerPosted = OfferState{usd, idr, Price{1, 2}, 10};
                    auto offerChanged = OfferState{idr, usd, price, 100};
                    for (auto i = 0; i < 10; i++)
                    {
                        offerChanged.amount -= 6;
                        market.requireChangesWithOffer(
                            {{offers[0].key, offerChanged}}, [&] {
                                return market.addOffer(b1, offerPosted,
                                                       OfferState::DELETED);
                            });
                    }

                    market.requireBalances({{a1, {{usd, 100}, {idr, 99940}}},
                                            {b1, {{usd, 19900}, {idr, 60}}}});
                });
            }
        }

        SECTION("offers with limits")
        {
            auto const price = Price{3, 2};
            auto b1 = root.create("B", minBalance3 + 10000);
            b1.changeTrust(idr, trustLineLimit);
            b1.changeTrust(usd, trustLineLimit);

            // offer is sell 100 IDR for 150 USD; buy USD @ 1.5 = sell
            // IRD @ 0.66
            auto offerA1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(a1, {idr, usd, Price{3, 2}, 100});
            });

            // b1 sells the same thing
            issuer.pay(b1, idr, trustLineBalance);
            auto offerB1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(b1, {idr, usd, Price{3, 2}, 100});
            });

            auto c1 = root.create("C", minBalanceA + 10000);
            c1.changeTrust(usd, trustLineLimit);
            c1.changeTrust(idr, trustLineLimit);
            issuer.pay(c1, usd, trustLineBalance);

            SECTION("creates an offer but reaches limit while selling")
            {
                for_all_versions(*app, [&] {
                    // fund C such that it's 150 IDR below its limit
                    issuer.pay(c1, idr, trustLineLimit - 150);

                    // try to create an offer:
                    // it will cross with the offers from A and B but
                    // will stop when C1's limit is reached.
                    // it should still be able to buy 150 IDR / sell 225
                    // USD

                    // offer is buy 200 IDR for 300 USD; buy IDR @ 0.66
                    // USD
                    // -> sell USD @ 1.5 IDR
                    auto offerChanged = OfferState{idr, usd, price, 50};
                    market.requireChangesWithOffer(
                        {{offerA1.key, OfferState::DELETED},
                         {offerB1.key, offerChanged}},
                        [&] {
                            return market.addOffer(c1,
                                                   {usd, idr, Price{2, 3}, 300},
                                                   OfferState::DELETED);
                        });

                    // A1's offer was taken entirely
                    // B1's offer was partially taken
                    // buyer may have paid a bit more to cross offers
                    market.requireBalances(
                        {{a1, {{usd, 150}, {idr, 99900}}},
                         {b1, {{usd, 75}, {idr, 99950}}},
                         {c1, {{usd, 99775}, {idr, 1000000}}}});
                });
            }

            SECTION("creates an offer but top seller is not authorized")
            {
                for_all_versions(*app, [&] {
                    // sets up the secure issuer account for USD
                    auto issuerAuth = root.create("issuerAuth", minBalance2);

                    auto usdAuth = issuerAuth.asset("USD");
                    auto idrAuth = issuerAuth.asset("IDR");

                    uint32_t setFlags =
                        AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;
                    issuerAuth.setOptions(nullptr, &setFlags, nullptr, nullptr,
                                          nullptr, nullptr);

                    // setup d1
                    auto d1 = root.create("D", minBalance3 + 10000);

                    d1.changeTrust(idrAuth, trustLineLimit);
                    d1.changeTrust(usdAuth, trustLineLimit);

                    issuerAuth.allowTrust(usdAuth, d1);
                    issuerAuth.allowTrust(idrAuth, d1);

                    issuerAuth.pay(d1, idrAuth, trustLineBalance);

                    // offer is sell 100 IDR for 150 USD; buy USD @
                    // 1.5 = sell IRD @ 0.66
                    auto offerD1 = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(
                            d1, {idrAuth, usdAuth, Price{3, 2}, 100});
                    });

                    SECTION("D not authorized to hold USD")
                    {
                        issuerAuth.denyTrust(usdAuth, d1);
                    }
                    SECTION("D not authorized to send IDR")
                    {
                        issuerAuth.denyTrust(idrAuth, d1);
                    }

                    // setup e1
                    auto e1 = root.create("E", minBalance3 + 10000);

                    e1.changeTrust(idrAuth, trustLineLimit);
                    e1.changeTrust(usdAuth, trustLineLimit);

                    issuerAuth.allowTrust(usdAuth, e1);
                    issuerAuth.allowTrust(idrAuth, e1);

                    issuerAuth.pay(e1, idrAuth, trustLineBalance);

                    auto offerE1 = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(
                            e1, {idrAuth, usdAuth, Price{3, 2}, 100});
                    });

                    // setup f1
                    auto f1 = root.create("F", minBalance3 + 10000);

                    f1.changeTrust(idrAuth, trustLineLimit);
                    f1.changeTrust(usdAuth, trustLineLimit);

                    issuerAuth.allowTrust(usdAuth, f1);
                    issuerAuth.allowTrust(idrAuth, f1);

                    issuerAuth.pay(f1, usdAuth, trustLineBalance);

                    // try to create an offer:
                    // it will cross with the offer from E and skip
                    // the / offer from D it should still be able to buy 100
                    // IDR
                    // / sell
                    // 150 USD

                    // offer is buy 200 IDR for 300 USD; buy IDR @
                    // 0.66 USD -> sell USD @ 1.5 IDR
                    auto offerPosted =
                        OfferState{usdAuth, idrAuth, Price{2, 3}, 300};
                    auto offerRemaining =
                        OfferState{usdAuth, idrAuth, Price{2, 3}, 150};
                    auto offerF1 = market.requireChangesWithOffer(
                        {{offerD1.key, OfferState::DELETED},
                         {offerE1.key, OfferState::DELETED}},
                        [&] {
                            return market.addOffer(f1, offerPosted,
                                                   offerRemaining);
                        });
                    // offer created would be buy 100 IDR for 150
                    // USD ; 0.66

                    // D1's offer was deleted
                    // E1's offer was taken
                    market.requireBalances(
                        {{d1, {{usdAuth, 0}, {idrAuth, 100000}}},
                         {e1, {{usdAuth, 150}, {idrAuth, 99900}}},
                         {f1, {{usdAuth, 99850}, {idrAuth, 100}}}});
                });
            }

            SECTION("creates an offer but top seller reaches limit")
            {
                for_all_versions(*app, [&] {
                    // makes "A" only capable of holding 75 "USD"
                    issuer.pay(a1, usd, trustLineLimit - 75);

                    // try to create an offer:
                    // it will cross with the offer from B fully
                    // but partially cross the offer from A
                    // it should still be able to buy 150 IDR / sell
                    // 225 USD

                    // offer is buy 200 IDR for 300 USD; buy IDR @
                    // 0.66 USD
                    // -> sell USD @ 1.5 IDR
                    auto offerPosted = OfferState{usd, idr, Price{2, 3}, 300};
                    auto offerRemaining = OfferState{usd, idr, Price{2, 3}, 75};
                    market.requireChangesWithOffer(
                        {{offerA1.key, OfferState::DELETED},
                         {offerB1.key, OfferState::DELETED}},
                        [&] {
                            return market.addOffer(c1, offerPosted,
                                                   offerRemaining);
                        });
                    // offer created would be buy 50 IDR for 75 USD
                    // ; 0.66

                    // check balances

                    // A1's offer was deleted
                    // B1's offer was taken
                    market.requireBalances(
                        {{a1,
                          {{usd, trustLineLimit},
                           {idr, trustLineBalance - 50}}},
                         {b1, {{usd, 150}, {idr, trustLineBalance - 100}}},
                         {c1, {{usd, trustLineBalance - 225}, {idr, 150}}}});
                });
            }
        }

        SECTION("issuer offers")
        {
            auto offerA1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(a1, {idr, usd, Price{3, 2}, 100});
            });

            SECTION("issuer creates an offer, claimed by somebody else")
            {
                for_all_versions(*app, [&] {
                    // sell 100 IDR for 90 USD
                    auto gwOffer = market.requireChangesWithOffer({}, [&] {
                        return market.addOffer(issuer,
                                               {idr, usd, Price{9, 10}, 100});
                    });

                    // fund a1 with some USD
                    issuer.pay(a1, usd, 1000);

                    // sell USD for IDR
                    market.requireChangesWithOffer(
                        {{gwOffer.key, OfferState::DELETED}}, [&] {
                            return market.addOffer(a1,
                                                   {usd, idr, Price{1, 1}, 90},
                                                   OfferState::DELETED);
                        });

                    market.requireBalances({
                        {a1, {{usd, 910}, {idr, trustLineBalance + 100}}},
                    });
                });
            }

            SECTION("issuer claims an offer from somebody else")
            {
                for_all_versions(*app, [&] {
                    market.requireChangesWithOffer(
                        {{offerA1.key, OfferState::DELETED}}, [&] {
                            return market.addOffer(issuer,
                                                   {usd, idr, Price(2, 3), 150},
                                                   OfferState::DELETED);
                        });

                    market.requireBalances({
                        {a1, {{usd, 150}, {idr, trustLineBalance - 100}}},
                    });
                });
            }
        }
    }

    SECTION("crossing offers with rounding")
    {
        auto market = TestMarket{*app};
        auto bidAmount = 8224563625;
        auto bidPrice = Price{500, 2061}; // bid for 4.1220000
        auto askAmount = 2000000000;
        auto askPrice = Price{2551, 625}; // ask for 4.0816000

        auto askingAccount = root.create("asking offer account", 10000000000);
        auto biddingAccount = root.create("bidding offer account", 10000000000);
        askingAccount.changeTrust(idr, 1000000000000);
        biddingAccount.changeTrust(idr, 1000000000000);
        issuer.pay(askingAccount, idr, 100000000000);

        auto bidding = OfferState{xlm, idr, bidPrice, bidAmount};
        auto asking = OfferState{idr, xlm, askPrice, askAmount};

        SECTION("bid before ask uses bid price")
        {
            auto biddingKey =
                market
                    .requireChangesWithOffer({},
                                             [&] {
                                                 return market.addOffer(
                                                     biddingAccount, bidding);
                                             })
                    .key;

            for_versions_to(2, *app, [&] {
                // 8224563625 / 4.1220000 = 1995284722 = 2000000000 -
                // 4715278
                // (rounding down)
                auto updatedAsking = OfferState{idr, xlm, askPrice, 4715278};
                // rounding error, should be 0
                auto updatedBidding = OfferState{xlm, idr, bidPrice, 1};

                market.requireChangesWithOffer(
                    {{biddingKey, updatedBidding}}, [&] {
                        return market.addOffer(askingAccount, asking,
                                               updatedAsking);
                    });
            });

            for_versions_from(3, *app, [&] {
                // 8224563625 / 4.1220000 = 1995284722 = 2000000000 -
                // 4715278
                // (rounding up)
                auto updatedAsking = OfferState{idr, xlm, askPrice, 4715277};
                market.requireChangesWithOffer(
                    {{biddingKey, OfferState::DELETED}}, [&] {
                        return market.addOffer(askingAccount, asking,
                                               updatedAsking);
                    });
            });
        }

        SECTION("ask before bid uses ask price")
        {
            for_all_versions(*app, [&] {
                // 2000000000 * 4.0816000 = 8163200000 = 8224563625 -
                // 61363625
                auto updatedBidding = OfferState{xlm, idr, bidPrice, 61363625};

                auto askingKey =
                    market
                        .requireChangesWithOffer({},
                                                 [&] {
                                                     return market.addOffer(
                                                         askingAccount, asking);
                                                 })
                        .key;
                market.requireChangesWithOffer(
                    {{askingKey, OfferState::DELETED}}, [&] {
                        return market.addOffer(biddingAccount, bidding,
                                               updatedBidding);
                    });
            });
        }
    }
}
