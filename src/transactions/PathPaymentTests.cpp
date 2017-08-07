// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestExceptions.h"
#include "test/TestMarket.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Timer.h"

#include <deque>

using namespace stellar;
using namespace stellar::txtest;

namespace
{

long operator*(long x, const Price& y)
{
    return x * y.n / y.d;
}

Price operator*(const Price& x, const Price& y)
{
    return Price{x.n * y.n, x.d * y.d};
}

template <typename T>
void
rotateRight(std::deque<T>& d)
{
    auto e = d.back();
    d.pop_back();
    d.push_front(e);
}

std::string
assetToString(const Asset& asset)
{
    auto r = std::string{};
    switch (asset.type())
    {
    case stellar::ASSET_TYPE_NATIVE:
        r = std::string{"XLM"};
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM4:
        assetCodeToStr(asset.alphaNum4().assetCode, r);
        break;
    case stellar::ASSET_TYPE_CREDIT_ALPHANUM12:
        assetCodeToStr(asset.alphaNum12().assetCode, r);
        break;
    }
    return r;
};

std::string
assetPathToString(const std::deque<Asset>& assets)
{
    auto r = assetToString(assets[0]);
    for (auto i = assets.rbegin(); i != assets.rend(); i++)
    {
        r += " -> " + assetToString(*i);
    }
    return r;
};
}

// *XLM Payment
// *Credit Payment
// XLM -> Credit Payment
// Credit -> XLM Payment
// Credit -> XLM -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("pathpayment", "[tx][pathpayment]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    ApplicationEditableVersion app(clock, cfg);
    app.start();

    // set up world
    auto root = TestAccount::createRoot(app);

    Asset xlm;

    int64_t txfee = app.getLedgerManager().getTxFee();

    // minimum balance necessary to hold 2 trust lines
    const int64_t minBalance2 =
        app.getLedgerManager().getMinBalance(2) + 10 * txfee;

    // minimum balance necessary to hold 2 trust lines and an offer
    const int64_t minBalance3 =
        app.getLedgerManager().getMinBalance(3) + 10 * txfee;

    const int64_t paymentAmount = minBalance3;

    // create an account
    auto a1 = root.create("A", paymentAmount);

    const int64_t morePayment = paymentAmount / 2;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000;

    // sets up gateway account
    const int64_t gatewayPayment = minBalance2 + morePayment;
    auto gateway = root.create("gate", gatewayPayment);

    // sets up gateway2 account
    auto gateway2 = root.create("gate2", gatewayPayment);

    Asset idr = makeAsset(gateway, "IDR");
    Asset usd = makeAsset(gateway2, "USD");

    AccountFrame::pointer a1Account, rootAccount;
    rootAccount = loadAccount(root, app);
    a1Account = loadAccount(a1, app);
    REQUIRE(rootAccount->getMasterWeight() == 1);
    REQUIRE(rootAccount->getHighThreshold() == 0);
    REQUIRE(rootAccount->getLowThreshold() == 0);
    REQUIRE(rootAccount->getMediumThreshold() == 0);
    REQUIRE(a1Account->getBalance() == paymentAmount);
    REQUIRE(a1Account->getMasterWeight() == 1);
    REQUIRE(a1Account->getHighThreshold() == 0);
    REQUIRE(a1Account->getLowThreshold() == 0);
    REQUIRE(a1Account->getMediumThreshold() == 0);
    // root did 2 transactions at this point
    REQUIRE(rootAccount->getBalance() == (1000000000000000000 - paymentAmount -
                                          gatewayPayment * 2 - txfee * 3));

    SECTION("path payment to self XLM")
    {
        auto a1Balance = a1.getBalance();
        auto amount = a1Balance / 10;

        for_versions_to(7, app, [&] {
            a1.pay(a1, xlm, amount, xlm, amount, {});
            REQUIRE(a1.getBalance() == a1Balance - amount - txfee);
        });

        for_versions_from(8, app, [&] {
            a1.pay(a1, xlm, amount, xlm, amount, {});
            REQUIRE(a1.getBalance() == a1Balance - txfee);
        });
    }

    SECTION("path payment to self asset")
    {
        auto curBalance = 10000;
        auto amount = curBalance / 10;

        a1.changeTrust(idr, 2 * curBalance);
        gateway.pay(a1, idr, curBalance);

        for_all_versions(app, [&] {
            a1.pay(a1, idr, amount, idr, amount, {});
            auto trustLine = loadTrustLine(a1, idr, app);
            REQUIRE(trustLine->getBalance() == curBalance);
        });
    }

    for_all_versions(app, [&] {
        SECTION("payment through path")
        {
            auto market = TestMarket{app};

            SECTION("send XLM with path (not enough offers)")
            {
                REQUIRE_THROWS_AS(market.requireChanges(
                                      {},
                                      [&] {
                                          gateway.pay(a1, idr, morePayment * 10,
                                                      xlm, morePayment, {});
                                      }),
                                  ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            }

            // setup a1
            a1.changeTrust(usd, trustLineLimit);
            a1.changeTrust(idr, trustLineLimit);

            gateway2.pay(a1, usd, trustLineStartingBalance);

            // add a couple offers in the order book

            OfferFrame::pointer offer;

            const Price usdPriceOffer(2, 1);

            // offer is sell 100 IDR for 200 USD ; buy USD @ 2.0 = sell IRD
            // @ 0.5

            auto b1 = root.create("B", minBalance3 + 10000);
            b1.changeTrust(usd, trustLineLimit);
            b1.changeTrust(idr, trustLineLimit);
            gateway.pay(b1, idr, trustLineStartingBalance);

            auto offerB1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(b1, {idr, usd, usdPriceOffer, 100});
            });

            // setup "c1"
            auto c1 = root.create("C", minBalance3 + 10000);

            c1.changeTrust(usd, trustLineLimit);
            c1.changeTrust(idr, trustLineLimit);

            gateway.pay(c1, idr, trustLineStartingBalance);

            // offer is sell 100 IDR for 150 USD ; buy USD @ 1.5 = sell IRD
            // @ 0.66
            auto offerC1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(c1, {idr, usd, Price(3, 2), 100});
            });

            // at this point:
            // a1 holds (0, IDR) (trustLineStartingBalance, USD)
            // b1 holds (trustLineStartingBalance, IDR) (0, USD)
            // c1 holds (trustLineStartingBalance, IDR) (0, USD)
            SECTION("send with path (over sendmax)")
            {
                // A1: try to send 100 IDR to B1
                // using 149 USD

                REQUIRE_THROWS_AS(
                    market.requireChanges(
                        {}, [&] { a1.pay(b1, usd, 149, idr, 100, {}); }),
                    ex_PATH_PAYMENT_OVER_SENDMAX);
            }

            SECTION("send with path (success)")
            {
                // A1: try to send 125 IDR to B1 using USD
                // should cost 150 (C's offer taken entirely) +
                //  50 (1/4 of B's offer)=200 USD

                auto offerB1Updated = OfferState{idr, usd, usdPriceOffer, 75};
                auto res = PathPaymentResult{};
                market.requireChanges(
                    {{offerC1.key, OfferState::DELETED},
                     {offerB1.key, offerB1Updated}},
                    [&] { res = a1.pay(b1, usd, 250, idr, 125, {}); });

                auto exchanged = std::vector<ClaimOfferAtom>{
                    offerC1.exchanged(100, 150), offerB1.exchanged(25, 50)};
                REQUIRE(res.success().offers == exchanged);

                market.requireBalances(
                    {{a1, {{usd, trustLineStartingBalance - 200}, {idr, 0}}},
                     {b1,
                      {{usd, 50},
                       {idr, trustLineStartingBalance + (125 - 25)}}},
                     {c1,
                      {{usd, 150}, {idr, trustLineStartingBalance - 100}}}});
            }

            SECTION("missing issuer")
            {
                SECTION("dest is standard account")
                {
                    SECTION("last")
                    {
                        // gateway issued idr
                        gateway.merge(root);

                        REQUIRE_THROWS_AS(
                            market.requireChanges({},
                                                  [&] {
                                                      a1.pay(b1, usd, 250, idr,
                                                             125, {}, &idr);
                                                  }),
                            ex_PATH_PAYMENT_NO_ISSUER);
                    }
                    SECTION("first")
                    {
                        // gateway2 issued usd
                        gateway2.merge(root);

                        REQUIRE_THROWS_AS(
                            market.requireChanges({},
                                                  [&] {
                                                      a1.pay(b1, usd, 250, idr,
                                                             125, {}, &usd);
                                                  }),
                            ex_PATH_PAYMENT_NO_ISSUER);
                    }
                    SECTION("mid")
                    {
                        SecretKey missing = getAccount("missing");
                        Asset btcCur = makeAsset(missing, "BTC");
                        REQUIRE_THROWS_AS(market.requireChanges(
                                              {},
                                              [&] {
                                                  a1.pay(b1, usd, 250, idr, 125,
                                                         {btcCur}, &btcCur);
                                              }),
                                          ex_PATH_PAYMENT_NO_ISSUER);
                    }
                }
                SECTION("dest is issuer")
                {
                    // single currency payment already covered elsewhere
                    // only one negative test:
                    SECTION("cannot take offers on the way")
                    {
                        // gateway issued idr
                        gateway.merge(root);

                        REQUIRE_THROWS_AS(
                            market.requireChanges({},
                                                  [&] {
                                                      a1.pay(gateway, usd, 250,
                                                             idr, 125, {});
                                                  }),
                            ex_PATH_PAYMENT_NO_DESTINATION);
                    }
                }
            }

            SECTION("dest amount too big for XLM")
            {
                REQUIRE_THROWS_AS(
                    root.pay(a1, xlm, 20, xlm,
                             std::numeric_limits<int64_t>::max(), {}),
                    ex_PATH_PAYMENT_MALFORMED);
            }

            SECTION("dest amount too big for asset")
            {
                gateway.pay(a1, idr, std::numeric_limits<int64_t>::max());
                REQUIRE_THROWS_AS(
                    gateway.pay(a1, idr, 20, idr,
                                std::numeric_limits<int64_t>::max(), {}),
                    ex_PATH_PAYMENT_LINE_FULL);
            }

            SECTION("send with path (takes own offer)")
            {
                // raise A1's balance by what we're trying to send
                root.pay(a1, 100);

                // offer is sell 100 USD for 100 XLM
                market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(a1, {usd, xlm, Price(1, 1), 100});
                });

                // A1: try to send 100 USD to B1 using XLM

                REQUIRE_THROWS_AS(
                    market.requireChanges(
                        {}, [&] { a1.pay(b1, xlm, 100, usd, 100, {}); }),
                    ex_PATH_PAYMENT_OFFER_CROSS_SELF);
            }

            SECTION("send with path (offer participant reaching limit)")
            {
                // make it such that C can only receive 120 USD (4/5th of
                // offerC)
                c1.changeTrust(usd, 120);

                // A1: try to send 105 IDR to B1 using USD
                // cost 120 (C's offer maxed out at 4/5th of published
                // amount)
                //  50 (1/4 of B's offer)=170 USD

                auto offerB1Updated = OfferState{idr, usd, usdPriceOffer, 75};
                auto res = PathPaymentResult{};
                market.requireChanges(
                    {{offerC1.key, OfferState::DELETED},
                     {offerB1.key, offerB1Updated}},
                    [&] { res = a1.pay(b1, usd, 400, idr, 105, {}); });

                auto exchanged = std::vector<ClaimOfferAtom>{
                    offerC1.exchanged(80, 120), offerB1.exchanged(25, 50),
                };
                REQUIRE(res.success().offers == exchanged);

                TrustFrame::pointer line;

                market.requireBalances(
                    {{a1, {{usd, trustLineStartingBalance - 170}, {idr, 0}}},
                     {b1,
                      {{usd, 50},
                       {idr, trustLineStartingBalance + (105 - 25)}}},
                     {c1, {{usd, 120}, {idr, trustLineStartingBalance - 80}}}});
            }
            SECTION("missing trust line")
            {
                // modify C's trustlines to invalidate C's offer
                // * C's offer should be deleted
                // sell 100 IDR for 200 USD
                // * B's offer 25 IDR by 50 USD

                auto checkBalances = [&]() {
                    auto offerB1Updated =
                        OfferState{idr, usd, usdPriceOffer, 75};
                    auto res = PathPaymentResult{};
                    market.requireChanges(
                        {{offerC1.key, OfferState::DELETED},
                         {offerB1.key, offerB1Updated}},
                        [&] { res = a1.pay(b1, usd, 200, idr, 25, {}); });

                    // C1 offer was deleted
                    auto exchanged = std::vector<ClaimOfferAtom>{
                        offerC1.exchanged(0, 0), offerB1.exchanged(25, 50),
                    };
                    REQUIRE(res.success().offers == exchanged);

                    market.requireBalances({
                        {a1, {{usd, trustLineStartingBalance - 50}, {idr, 0}}},
                        {b1, {{usd, 50}, {idr, trustLineStartingBalance}}},
                    });
                };

                SECTION("deleted selling line")
                {
                    c1.pay(gateway, idr, trustLineStartingBalance);
                    c1.changeTrust(idr, 0);

                    checkBalances();
                }

                SECTION("deleted buying line")
                {
                    c1.changeTrust(usd, 0);
                    checkBalances();
                }
            }
        }

        SECTION("path payment with rounding errors")
        {
            auto market = TestMarket{app};
            auto issuer = root.create("issuer", 5999999400);
            auto source = root.create("source", 1989999000);
            auto destination = root.create("destination", 499999700);
            auto seller = root.create("seller", 20999999300);

            auto cny = issuer.asset("CNY");
            destination.changeTrust(cny, INT64_MAX);
            seller.changeTrust(cny, 1000000000000);

            issuer.pay(seller, cny, 1700000000);
            auto price = Price{2000, 29};
            auto sellerOffer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(seller, {cny, xlm, price, 145000000});
            });

            auto path = std::vector<Asset>{};
            if (app.getLedgerManager().getCurrentLedgerVersion() <= 2)
            {
                // bug, it should succeed
                REQUIRE_THROWS_AS(
                    market.requireChanges({},
                                          [&] {
                                              source.pay(destination, xlm,
                                                         1382068965, cny,
                                                         20000000, path);
                                          }),
                    ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            }
            else
            {
                auto sellerOfferRemaining =
                    OfferState{cny, xlm, price, 145000000 - 20000000};
                market.requireChanges(
                    {{sellerOffer.key, sellerOfferRemaining}}, [&] {
                        source.pay(destination, xlm, 1382068965, cny, 20000000,
                                   path, nullptr);
                    });

                // 1379310345 = round up(20000000 * price)
                market.requireBalances(
                    {{source, {{xlm, 1989999000 - 100 - 1379310345}}},
                     {seller, {{cny, 1680000000}}},
                     {destination, {{cny, 20000000}}}});
            }
        }

        SECTION("path with bogus offer, bogus offer shows on offers trail")
        {
            auto market = TestMarket{app};

            auto paymentToReceive = 240000000;
            auto offerSize = paymentToReceive / 2;
            auto initialBalance = app.getLedgerManager().getMinBalance(10) +
                                  txfee * 10 + 1000000000;
            auto mm = root.create("mm", initialBalance);
            auto source = root.create("source", initialBalance);
            auto destination = root.create("destination", initialBalance);
            mm.changeTrust(idr, trustLineLimit);
            mm.changeTrust(usd, trustLineLimit);
            destination.changeTrust(idr, trustLineLimit);
            gateway.pay(mm, idr, 1000000000);
            gateway2.pay(mm, usd, 1000000000);

            auto idrCurCheapOffer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm, {idr, usd, Price{3, 12}, offerSize});
            });
            auto idrCurMidBogusOffer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm, {idr, usd, Price{4, 12}, 1});
            });
            auto idrCurExpensiveOffer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm, {idr, usd, Price{6, 12}, offerSize});
            });
            auto usdCurOffer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm,
                                       {usd, xlm, Price{1, 2}, 2 * offerSize});
            });

            auto path = std::vector<Asset>{xlm, usd};
            auto res = source.pay(destination, xlm, 8 * paymentToReceive, idr,
                                  paymentToReceive, path);

            auto exchanged =
                app.getLedgerManager().getCurrentLedgerVersion() < 3
                    ? std::vector<ClaimOfferAtom>{usdCurOffer.exchanged(
                                                      90000000, 45000000),
                                                  idrCurCheapOffer.exchanged(
                                                      120000000, 30000000),
                                                  idrCurMidBogusOffer.exchanged(
                                                      0, 0),
                                                  idrCurExpensiveOffer
                                                      .exchanged(120000000,
                                                                 60000000)}
                    : std::vector<ClaimOfferAtom>{
                          usdCurOffer.exchanged(90000001, 45000001),
                          idrCurCheapOffer.exchanged(120000000, 30000000),
                          idrCurMidBogusOffer.exchanged(1, 1),
                          idrCurExpensiveOffer.exchanged(119999999, 60000000)};
            REQUIRE(res.success().offers == exchanged);
        }

        SECTION("path payment with cycle")
        {
            // Create 3 different cycles.
            // First cycle involves 3 transaction in which buying price is
            // always half - so sender buys 8 times as much XLM as he/she
            // sells (arbitrage).
            // Second cycle involves 3 transaction in which buying price is
            // always two - so sender buys 8 times as much XLM as he/she
            // sells (anti-arbitrage).
            // Thanks to send max option this transaction is rejected.
            // Third cycle is similar to second, but send max is set to a
            // high value, so transaction proceeds even if it makes sender
            // lose a lot of XLM.

            // Each cycle is created in 3 variants (to check if behavior
            // does not depend of nativeness of asset):
            // * XLM -> USD -> IDR -> XLM
            // * USD -> IDR -> XLM -> USD
            // * IDR -> XLM -> USD -> IDR
            // To create variants, rotateRight() function is used on
            // accounts, offers and assets -
            // it greatly simplified index calculation in the code.

            auto market = TestMarket{app};
            auto paymentAmount =
                int64_t{100000000}; // amount of money that 'destination'
                                    // account will receive
            auto offerAmount = 8 * paymentAmount; // amount of money in
                                                  // offer required to pass
                                                  // - needs 8x of payment
                                                  // for anti-arbitrage case
            auto initialBalance =
                2 * offerAmount; // we need twice as much money as in the
                                 // offer because of Price{2, 1} that is
                                 // used in one case
            auto txFee = app.getLedgerManager().getTxFee();

            auto assets = std::deque<Asset>{xlm, usd, idr};
            auto pathSize = assets.size();
            auto accounts = std::deque<TestAccount>{};

            auto setupAccount = [&](const std::string& name) {
                // setup account with required trustlines and money both in
                // native and assets

                auto account = root.create(name, initialBalance);
                account.changeTrust(idr, trustLineLimit);
                gateway.pay(account, idr, initialBalance);
                account.changeTrust(usd, trustLineLimit);
                gateway2.pay(account, usd, initialBalance);

                return account;
            };

            auto validateAccountAsset = [&](const TestAccount& account,
                                            int assetIndex, int difference,
                                            int feeCount) {
                if (assets[assetIndex].type() == ASSET_TYPE_NATIVE)
                {
                    REQUIRE(account.getBalance() ==
                            initialBalance + difference - feeCount * txFee);
                }
                else
                {
                    REQUIRE(loadTrustLine(account, assets[assetIndex], app)
                                ->getBalance() == initialBalance + difference);
                }
            };
            auto validateAccountAssets = [&](const TestAccount& account,
                                             int assetIndex, int difference,
                                             int feeCount) {
                for (size_t i = 0; i < pathSize; i++)
                {
                    validateAccountAsset(account, i,
                                         (assetIndex == i) ? difference : 0,
                                         feeCount);
                }
            };
            auto validateOffer = [offerAmount](
                const TestAccount& account, uint64_t offerId, int difference) {
                auto offer = account.loadOffer(offerId);
                REQUIRE(offer.amount == offerAmount + difference);
            };

            auto source = setupAccount("S");
            auto destination = setupAccount("D");

            auto validateSource = [&](int difference) {
                validateAccountAssets(source, 0, difference, 3);
            };
            auto validateDestination = [&](int difference) {
                validateAccountAssets(destination, 0, difference, 2);
            };

            for (size_t i = 0; i < pathSize;
                 i++) // create account for each known asset
            {
                accounts.emplace_back(
                    setupAccount(std::string{"C"} + std::to_string(i)));
                validateAccountAssets(accounts[i], 0, 0,
                                      2); // 2x change trust called
            }

            auto testPath = [&](const std::string& name, const Price& price,
                                int maxMultipler, bool overSendMax) {
                SECTION(name)
                {
                    auto offers = std::deque<uint64_t>{};
                    for (size_t i = 0; i < pathSize; i++)
                    {
                        offers.push_back(
                            market
                                .requireChangesWithOffer(
                                    {},
                                    [&] {
                                        return market.addOffer(
                                            accounts[i],
                                            {assets[i],
                                             assets[(i + 2) % pathSize], price,
                                             offerAmount});
                                    })
                                .key.offerID);
                        validateOffer(accounts[i], offers[i], 0);
                    }

                    for (size_t i = 0; i < pathSize; i++)
                    {
                        auto path = std::vector<Asset>{assets[1], assets[2]};
                        SECTION(std::string{"send with path ("} +
                                assetPathToString(assets) + ")")
                        {
                            auto destinationMultiplier = overSendMax ? 0 : 1;
                            auto sellerMultipler =
                                overSendMax ? Price{0, 1} : Price{1, 1};
                            auto buyerMultipler = sellerMultipler * price;

                            if (overSendMax)
                                REQUIRE_THROWS_AS(
                                    source.pay(destination, assets[0],
                                               maxMultipler * paymentAmount,
                                               assets[0], paymentAmount, path),
                                    ex_PATH_PAYMENT_OVER_SENDMAX);
                            else
                                source.pay(destination, assets[0],
                                           maxMultipler * paymentAmount,
                                           assets[0], paymentAmount, path);

                            for (size_t j = 0; j < pathSize; j++)
                            {
                                auto index = (pathSize - j) %
                                             pathSize; // it is done from
                                                       // end of path to
                                                       // begin of path
                                validateAccountAsset(accounts[index], index,
                                                     -paymentAmount *
                                                         sellerMultipler,
                                                     3); // sold asset
                                validateOffer(
                                    accounts[index], offers[index],
                                    -paymentAmount *
                                        sellerMultipler); // sold asset
                                validateAccountAsset(
                                    accounts[index], (index + 2) % pathSize,
                                    paymentAmount * buyerMultipler,
                                    3); // bought asset
                                validateAccountAsset(accounts[index],
                                                     (index + 1) % pathSize, 0,
                                                     3); // ignored asset
                                sellerMultipler = sellerMultipler * price;
                                buyerMultipler = buyerMultipler * price;
                            }

                            validateSource(-paymentAmount * sellerMultipler);
                            validateDestination(paymentAmount *
                                                destinationMultiplier);
                        }

                        // next cycle variant
                        rotateRight(assets);
                        rotateRight(accounts);
                        rotateRight(offers);
                    }
                }
            };

            // cycle with every asset on path costing half as much as
            // previous - 8 times gain
            testPath("arbitrage", Price(1, 2), 1, false);
            // cycle with every asset on path costing twice as much as
            // previous - 8 times loss - unacceptable
            testPath("anti-arbitrage", Price(2, 1), 1, true);
            // cycle with every asset on path costing twice as much as
            // previous - 8 times loss - acceptable (but not wise to do)
            testPath("anti-arbitrage with big sendmax", Price(2, 1), 8, false);
        }
    });
}
