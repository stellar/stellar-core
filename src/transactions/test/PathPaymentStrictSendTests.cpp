// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
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

namespace
{

int64_t operator*(int64_t x, const Price& y)
{
    bool xNegative = (x < 0);
    int64_t m = bigDivide(xNegative ? -x : x, y.n, y.d, Rounding::ROUND_DOWN);
    return xNegative ? -m : m;
}

Price operator*(const Price& x, const Price& y)
{
    int64_t n = int64_t(x.n) * int64_t(y.n);
    int64_t d = int64_t(x.d) * int64_t(y.d);
    assert(n <= std::numeric_limits<int32_t>::max());
    assert(n >= 0);
    assert(d <= std::numeric_limits<int32_t>::max());
    assert(d >= 1);
    return Price{(int32_t)n, (int32_t)d};
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
assetPathToString(const std::deque<Asset>& assets)
{
    auto r = assetToString(assets[0]);
    for (auto i = assets.rbegin(); i != assets.rend(); i++)
    {
        r += " -> " + assetToString(*i);
    }
    return r;
};

void
checkClaimedOffers(std::vector<ClaimOfferAtom> const& actual,
                   std::vector<ClaimOfferAtom> const& expected,
                   int64_t sendAmount, int64_t destMinAmount)
{
    REQUIRE(actual == expected);

    REQUIRE(std::adjacent_find(
                actual.begin(), actual.end(),
                [](ClaimOfferAtom const& lhs, ClaimOfferAtom const& rhs) {
                    if (!(lhs.assetBought == rhs.assetBought &&
                          lhs.assetSold == rhs.assetSold))
                    {
                        return false;
                    }

                    if (lhs.amountSold == 0)
                    {
                        return true;
                    }

                    // This is a heuristic. Due to price error its possible this
                    // is not satisfied, but it should work under reasonable
                    // assumptions.
                    return (lhs.amountBought * rhs.amountSold >
                            lhs.amountSold * rhs.amountBought);
                }) == actual.end());

    if (!actual.empty())
    {
        Asset const& sourceAsset = actual.front().assetBought;
        int64_t actualSendAmount = 0;
        for (auto iter = actual.begin();
             iter != actual.end() && iter->assetBought == sourceAsset; ++iter)
        {
            actualSendAmount += iter->amountBought;
        }
        REQUIRE(actualSendAmount == sendAmount);

        Asset const& destAsset = actual.back().assetSold;
        int64_t actualDestAmount = 0;
        for (auto iter = actual.rbegin();
             iter != actual.rend() && iter->assetSold == destAsset; ++iter)
        {
            actualDestAmount += iter->amountSold;
        }
        REQUIRE(actualDestAmount >= destMinAmount);
    }
}
}

TEST_CASE("pathpayment strict send", "[tx][pathpayment]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    app->start();

    auto& lm = app->getLedgerManager();
    auto const txfee = lm.getLastTxFee();
    auto const minBalance = lm.getLastMinBalance(0) + 10 * txfee;
    auto const minBalance1 = lm.getLastMinBalance(1) + 10 * txfee;
    auto const minBalance2 = lm.getLastMinBalance(2) + 10 * txfee;
    auto const minBalance3 = lm.getLastMinBalance(3) + 10 * txfee;
    auto const minBalance4 = lm.getLastMinBalance(4) + 10 * txfee;
    auto const minBalance5 = lm.getLastMinBalance(5) + 10 * txfee;

    auto root = TestAccount::createRoot(*app);
    auto gateway = root.create("gate1", minBalance5);
    auto gateway2 = root.create("gate2", minBalance5);

    auto xlm = makeNativeAsset();
    auto idr = makeAsset(gateway, "IDR");
    auto cur1 = makeAsset(gateway, "CUR1");
    auto cur2 = makeAsset(gateway, "CUR2");
    auto usd = makeAsset(gateway2, "USD");
    auto cur3 = makeAsset(gateway2, "CUR3");
    auto cur4 = makeAsset(gateway2, "CUR4");

    closeLedgerOn(*app, 2, 1, 1, 2016);

    SECTION("not supported before version 12")
    {
        for_versions_to(11, *app, [&] {
            REQUIRE_THROWS_AS(
                root.pathPaymentStrictSend(root, xlm, 1, xlm, 1, {}),
                ex_opNOT_SUPPORTED);
        });
    }

    SECTION("send amount constraints")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        for_versions_from(12, *app, [&] {
            int64_t i = 0;
            for (int64_t amount : std::vector<int64_t>({0, -1}))
            {
                REQUIRE_THROWS_AS(source.pathPaymentStrictSend(
                                      destination, xlm, amount, xlm, 1, {}),
                                  ex_PATH_PAYMENT_STRICT_SEND_MALFORMED);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance1 - (++i) * txfee}}},
                     {destination, {{xlm, minBalance}}}});
                // clang-format on
            }
        });
    }

    SECTION("destination minimum constraints")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        for_versions_from(12, *app, [&] {
            int64_t i = 0;
            for (int64_t amount : std::vector<int64_t>({0, -1}))
            {
                REQUIRE_THROWS_AS(source.pathPaymentStrictSend(
                                      destination, xlm, 1, xlm, amount, {}),
                                  ex_PATH_PAYMENT_STRICT_SEND_MALFORMED);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance1 - (++i) * txfee}}},
                     {destination, {{xlm, minBalance}}}});
                // clang-format on
            }
        });
    }

    SECTION("currency invalid")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(source.pathPaymentStrictSend(destination,
                                                           makeInvalidAsset(),
                                                           10, xlm, 10, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}}},
                 {destination, {{xlm, minBalance}}}});
            // clang-format on

            REQUIRE_THROWS_AS(source.pathPaymentStrictSend(destination, xlm, 10,
                                                           makeInvalidAsset(),
                                                           10, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}}},
                 {destination, {{xlm, minBalance}}}});
            // clang-format on

            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(destination, xlm, 10, xlm, 10,
                                             {makeInvalidAsset()}),
                ex_PATH_PAYMENT_STRICT_SEND_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 3 * txfee}}},
                 {destination, {{xlm, minBalance}}}});
            // clang-format on
        });
    }

    SECTION("send amount too big")
    {
        auto market = TestMarket{*app};
        auto a = root.create("a", minBalance1);
        auto dest = root.create("destination", minBalance1);
        a.changeTrust(idr, std::numeric_limits<int64_t>::max());
        dest.changeTrust(idr, std::numeric_limits<int64_t>::max());
        gateway.pay(a, idr, 10);

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(a.pathPaymentStrictSend(
                                  dest, xlm, minBalance1 + 1, xlm, 20, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_UNDERFUNDED);
            // clang-format off
            market.requireBalances(
                {{a, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}}},
                 {dest, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                a.pathPaymentStrictSend(gateway, idr, 11, idr, 20, {}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDERFUNDED);
            // clang-format off
            market.requireBalances(
                {{a, {{xlm, minBalance1 - 3 * txfee}, {idr, 10}}},
                 {dest, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("source does not have trustline")
    {
        auto market = TestMarket{*app};
        auto noSourceTrust = root.create("no-source-trust", minBalance);
        auto destination = root.create("destination", minBalance1);
        destination.changeTrust(idr, 20);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(noSourceTrust.pathPaymentStrictSend(
                                  gateway, idr, 1, idr, 1, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{noSourceTrust, {{xlm, minBalance - txfee}, {idr, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(noSourceTrust.pathPaymentStrictSend(
                                  destination, idr, 1, idr, 1, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_SRC_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{noSourceTrust, {{xlm, minBalance - 2 * txfee}, {idr, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("source is not authorized")
    {
        auto market = TestMarket{*app};
        auto noAuthorizedSourceTrust =
            root.create("no-authorized-source-trust", minBalance1);
        auto destination = root.create("destination", minBalance1);
        noAuthorizedSourceTrust.changeTrust(idr, 20);
        gateway.pay(noAuthorizedSourceTrust, idr, 10);
        destination.changeTrust(idr, 20);
        gateway.setOptions(
            setFlags(uint32_t{AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG}));
        gateway.denyTrust(idr, noAuthorizedSourceTrust);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(noAuthorizedSourceTrust.pathPaymentStrictSend(
                                  gateway, idr, 10, idr, 10, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{noAuthorizedSourceTrust, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(noAuthorizedSourceTrust.pathPaymentStrictSend(
                                  destination, idr, 10, idr, 10, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_SRC_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{noAuthorizedSourceTrust, {{xlm, minBalance1 - 3 * txfee}, {idr, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination does not exist")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(
                    getAccount("non-existing-destination").getPublicKey(), idr,
                    10, idr, 10, {}),
                ex_PATH_PAYMENT_STRICT_SEND_NO_DESTINATION);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}}}});
            // clang-format on
        });
    }

    SECTION("destination is issuer and does not exist for simple paths")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_versions_from(12, *app, [&] {
            gateway.merge(root);
            auto offers =
                source.pathPaymentStrictSend(gateway, idr, 10, idr, 10, {});
            auto expected = std::vector<ClaimOfferAtom>{};
            REQUIRE(offers.success().offers == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination is issuer and does not exist for complex paths")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_versions_from(12, *app, [&] {
            gateway.merge(root);
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(gateway, idr, 10, usd, 10, {}),
                ex_PATH_PAYMENT_STRICT_SEND_NO_DESTINATION);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination does not have trustline")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto noDestinationTrust =
            root.create("no-destination-trust", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(gateway.pathPaymentStrictSend(noDestinationTrust,
                                                            idr, 1, idr, 1, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}}},
                 {noDestinationTrust, {{xlm, minBalance}, {idr, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(source.pathPaymentStrictSend(noDestinationTrust,
                                                           idr, 1, idr, 1, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}}},
                 {noDestinationTrust, {{xlm, minBalance}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination is not authorized")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto noAuthorizedDestinationTrust =
            root.create("no-authorized-destination-trust", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        noAuthorizedDestinationTrust.changeTrust(idr, 20);
        gateway.setOptions(
            setFlags(uint32_t{AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG}));
        gateway.denyTrust(idr, noAuthorizedDestinationTrust);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                gateway.pathPaymentStrictSend(noAuthorizedDestinationTrust, idr,
                                              10, idr, 10, {}),
                ex_PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}},
                 {noAuthorizedDestinationTrust, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(noAuthorizedDestinationTrust, idr,
                                             10, idr, 10, {}),
                ex_PATH_PAYMENT_STRICT_SEND_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {noAuthorizedDestinationTrust, {{xlm, minBalance1 - txfee}, {idr, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination line full")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(idr, 20);
        gateway.pay(source, idr, 11);
        gateway.pay(destination, idr, 10);

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(gateway.pathPaymentStrictSend(destination, idr,
                                                            11, idr, 11, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 11}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(destination, idr, 11, idr, 11, {}),
                ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 11}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("destination line overflow")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(idr, INT64_MAX);
        gateway.pay(source, idr, 11);
        gateway.pay(destination, idr, INT64_MAX - 10);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(gateway.pathPaymentStrictSend(destination, idr,
                                                            11, idr, 11, {}),
                              ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 11}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, INT64_MAX - 10}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(destination, idr, 11, idr, 11, {}),
                ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 11}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, INT64_MAX - 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("issuer missing")
    {
        // look at the SECTION "uses all offers in a loop" for a
        // successful path payment test with no issuers

        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(usd, 20);
        gateway.pay(source, idr, 10);

        for_versions_from(12, *app, [&] {
            uint32_t ledgerVersion;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ledgerVersion = ltx.loadHeader().current().ledgerVersion;
            }

            auto pathPaymentStrictSend = [&](std::vector<Asset> const& path,
                                             Asset& noIssuer) {
                if (ledgerVersion < 13)
                {
                    REQUIRE_THROWS_AS(
                        source.pathPaymentStrictSend(destination, idr, 10, usd,
                                                     10, path, &noIssuer),
                        ex_PATH_PAYMENT_STRICT_SEND_NO_ISSUER);
                }
                else
                {
                    REQUIRE_THROWS_AS(
                        source.pathPaymentStrictSend(destination, idr, 10, usd,
                                                     10, path, &noIssuer),
                        ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
                }
            };

            SECTION("path payment send issuer missing")
            {
                gateway.merge(root);
                pathPaymentStrictSend({}, idr);
            }

            SECTION("path payment middle issuer missing")
            {
                auto btc = makeAsset(getAccount("missing"), "BTC");
                pathPaymentStrictSend({btc}, btc);
            }

            SECTION("path payment last issuer missing")
            {
                gateway2.merge(root);
                pathPaymentStrictSend({}, usd);
            }

            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("not enough offers for first exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 9});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("not enough offers for middle exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 9});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("not enough offers for last exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 9});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_TOO_FEW_OFFERS);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("crosses own offer for first exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance3);
        auto destination = root.create("destination", minBalance1);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        source.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(source, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur2, cur1, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance3 - 4 * txfee}, {cur1, 10}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("crosses own offer for middle exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        source.changeTrust(cur2, 20);
        source.changeTrust(cur3, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(source, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("crosses own offer for last exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);

        source.changeTrust(cur1, 20);
        source.changeTrust(cur3, 20);
        source.changeTrust(cur4, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(source, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur2, cur3});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION(
        "does not cross own offer if better is available for first exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance3);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 30);
        source.changeTrust(cur2, 30);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(source, cur2, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur2, cur1, Price{100, 99}, 10});
        });
        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION(
        "does not cross own offer if better is available for middle exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 30);
        source.changeTrust(cur2, 30);
        source.changeTrust(cur3, 30);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(source, cur3, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur3, cur2, Price{100, 99}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("does not cross own offer if better is available for last exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 30);
        source.changeTrust(cur3, 30);
        source.changeTrust(cur4, 30);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(source, cur4, 10);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(source, {cur4, cur3, Price{100, 99}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("under destination minimum XLM")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance);
        auto destination = root.create("destination", minBalance);
        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(destination, xlm, 10, xlm, 11, {}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
            market.requireBalances(
                {{source, {{xlm, minBalance - txfee}, {idr, 0}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
        });
    }

    SECTION("under destination minimum asset")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 10);
        destination.changeTrust(idr, 10);
        gateway.pay(source, idr, 10);

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                source.pathPaymentStrictSend(destination, idr, 9, idr, 10, {}),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("under destination minimum with real path")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 80);
        gateway.pay(mm12, cur2, 40);
        gateway2.pay(mm23, cur3, 20);
        gateway2.pay(mm34, cur4, 10);

        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 40});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 20});
        });
        market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            REQUIRE_THROWS_AS(
                market.requireChanges({},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 10, cur4, 10,
                                              {cur1, cur2, cur3, cur4});
                                      }),
                ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("to self XLM")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance + txfee + 20);

        for_versions_from(12, *app, [&] {
            account.pathPaymentStrictSend(account, xlm, 20, xlm, 20, {});
            market.requireBalances({{account, {{xlm, minBalance + 20}}}});
        });
    }

    SECTION("to self asset")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance1 + 2 * txfee);
        account.changeTrust(idr, 20);
        gateway.pay(account, idr, 10);

        for_versions_from(12, *app, [&] {
            auto offers =
                account.pathPaymentStrictSend(account, idr, 10, idr, 10, {});
            auto expected = std::vector<ClaimOfferAtom>{};
            REQUIRE(offers.success().offers == expected);
            market.requireBalances({{account, {{idr, 10}}}});
        });
    }

    SECTION("to self asset over the limit")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance1 + 2 * txfee);
        account.changeTrust(idr, 20);
        gateway.pay(account, idr, 19);

        for_versions_from(12, *app, [&] {
            REQUIRE_NOTHROW(
                account.pathPaymentStrictSend(account, idr, 2, idr, 2, {}));
            market.requireBalances({{account, {{idr, 19}}}});
        });
    }

    SECTION("crosses destination offer for first exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance4);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur1, 20);
        destination.changeTrust(cur2, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(destination, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(destination, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 4 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("crosses destination offer for middle exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance4);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm34.changeTrust(cur3, 20);
        mm34.changeTrust(cur4, 20);
        destination.changeTrust(cur2, 20);
        destination.changeTrust(cur3, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(destination, cur3, 10);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(destination, {cur3, cur2, Price{1, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 4 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("crosses destination offer for last exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance4);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);

        source.changeTrust(cur1, 20);
        mm12.changeTrust(cur1, 20);
        mm12.changeTrust(cur2, 20);
        mm23.changeTrust(cur2, 20);
        mm23.changeTrust(cur3, 20);
        destination.changeTrust(cur3, 20);
        destination.changeTrust(cur4, 20);

        gateway.pay(source, cur1, 10);
        gateway.pay(mm12, cur2, 10);
        gateway2.pay(mm23, cur3, 10);
        gateway2.pay(destination, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 10});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{1, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(destination, {cur4, cur3, Price{1, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 10,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            checkClaimedOffers(actual, expected, 10, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("uses whole best offer for first exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12a = root.create("mm12a", minBalance3);
        auto mm12b = root.create("mm12b", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 200);
        mm12a.changeTrust(cur1, 200);
        mm12a.changeTrust(cur2, 200);
        mm12b.changeTrust(cur1, 200);
        mm12b.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 80);
        gateway.pay(mm12a, cur2, 40);
        gateway.pay(mm12b, cur2, 40);
        gateway2.pay(mm23, cur3, 20);
        gateway2.pay(mm34, cur4, 10);

        auto o1a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 10});
        });
        auto o1b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12b, {cur2, cur1, Price{2, 1}, 40});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 10}},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 80,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(10, 20), o1b.exchanged(30, 60),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            checkClaimedOffers(actual, expected, 80, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 20}, {cur2, 30}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 60}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("uses whole best offer for second exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12a", minBalance3);
        auto mm23a = root.create("mm23a", minBalance3);
        auto mm23b = root.create("mm23b", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23a.changeTrust(cur2, 200);
        mm23a.changeTrust(cur3, 200);
        mm23b.changeTrust(cur2, 200);
        mm23b.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 80);
        gateway.pay(mm12, cur2, 40);
        gateway2.pay(mm23a, cur3, 20);
        gateway2.pay(mm23b, cur3, 20);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 40});
        });
        auto o2a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 15});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 10});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, {cur3, cur2, Price{2, 1}, 5}},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 80,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(15, 30),
                o2b.exchanged(5, 10), o3.exchanged(10, 20)};
            checkClaimedOffers(actual, expected, 80, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 30}, {cur3, 5}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 30}, {cur3, 5}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("uses whole best offer for last exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12a", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34a = root.create("mm34a", minBalance3);
        auto mm34b = root.create("mm34b", minBalance3);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34a.changeTrust(cur3, 200);
        mm34a.changeTrust(cur4, 200);
        mm34b.changeTrust(cur3, 200);
        mm34b.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 80);
        gateway.pay(mm12, cur2, 40);
        gateway2.pay(mm23, cur3, 20);
        gateway2.pay(mm34a, cur4, 10);
        gateway2.pay(mm34b, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 40});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 2});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, {cur4, cur3, Price{2, 1}, 2}}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 80,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(2, 4),
                o3b.exchanged(8, 16)};
            checkClaimedOffers(actual, expected, 80, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 4}, {cur4, 8}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 16}, {cur4, 2}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("takes all offers, one offer per exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 80);
        gateway.pay(mm12, cur2, 40);
        gateway2.pay(mm23, cur3, 20);
        gateway2.pay(mm34, cur4, 10);

        auto o1 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 40});
        });
        auto o2 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        for_versions_from(12, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual = source
                                                   .pathPaymentStrictSend(
                                                       destination, cur1, 80,
                                                       cur4, 10, {cur2, cur3})
                                                   .success()
                                                   .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(40, 80),
                                                        o2.exchanged(20, 40),
                                                        o3.exchanged(10, 20)};
            checkClaimedOffers(actual, expected, 80, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("takes all offers, multiple offers per exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance5);
        auto mm23 = root.create("mm23", minBalance5);
        auto mm34 = root.create("mm34", minBalance5);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 59);
        gateway.pay(mm12, cur2, 35);
        gateway2.pay(mm23, cur3, 23);
        gateway2.pay(mm34, cur4, 16);

        for_versions_from(12, *app, [&] {
            auto o1a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{3, 2}, 10});
            });
            auto o1b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 16});
            });
            auto o1c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{4, 3}, 9});
            });
            auto o2a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 5});
            });
            auto o2b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{3, 2}, 6});
            });
            auto o2c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{4, 3}, 12});
            });
            auto o3a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{4, 3}, 9});
            });
            auto o3b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{3, 2}, 6});
            });
            auto o3c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 1});
            });

            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, OfferState::DELETED},
                                   {o1c.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, OfferState::DELETED},
                                   {o2c.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED},
                                   {o3c.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pathPaymentStrictSend(
                                                  destination, cur1, 59, cur4,
                                                  16, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1c.exchanged(9, 12),  o1a.exchanged(10, 15),
                o1b.exchanged(16, 32), o2c.exchanged(12, 16),
                o2b.exchanged(6, 9),   o2a.exchanged(5, 10),
                o3a.exchanged(9, 12),  o3b.exchanged(6, 9),
                o3c.exchanged(1, 2)};
            checkClaimedOffers(actual, expected, 59, 16);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 59}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 35}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 23}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 16}}}});
            // clang-format on
        });
    }

    SECTION("takes best offers, multiple offers per exchange")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance5);
        auto mm23 = root.create("mm23", minBalance5);
        auto mm34 = root.create("mm34", minBalance5);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 58);
        gateway.pay(mm12, cur2, 35);
        gateway2.pay(mm23, cur3, 23);
        gateway2.pay(mm34, cur4, 16);

        for_versions_from(12, *app, [&] {
            auto o1a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{3, 2}, 10});
            });
            auto o1b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 16});
            });
            auto o1c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{4, 3}, 9});
            });
            auto o2a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 5});
            });
            auto o2b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{3, 2}, 6});
            });
            auto o2c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{4, 3}, 12});
            });
            auto o3a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{4, 3}, 9});
            });
            auto o3b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{3, 2}, 6});
            });
            auto o3c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 1});
            });

            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 2}},
                                   {o1c.key, OfferState::DELETED},
                                   {o2a.key, {cur3, cur2, Price{2, 1}, 1}},
                                   {o2b.key, OfferState::DELETED},
                                   {o2c.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED},
                                   {o3c.key, {cur4, cur3, Price{2, 1}, 1}}},
                                  [&] {
                                      actual =
                                          source
                                              .pathPaymentStrictSend(
                                                  destination, cur1, 55, cur4,
                                                  15, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1c.exchanged(9, 12),  o1a.exchanged(10, 15),
                o1b.exchanged(14, 28), o2c.exchanged(12, 16),
                o2b.exchanged(6, 9),   o2a.exchanged(4, 8),
                o3a.exchanged(9, 12),  o3b.exchanged(6, 9),
                o3c.exchanged(0, 1)};
            checkClaimedOffers(actual, expected, 55, 15);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 3}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 55}, {cur2, 2}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 33}, {cur3, 1}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 22}, {cur4, 1}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 15}}}});
            // clang-format on
        });
    }

    SECTION("takes best offers, multiple offers per exchange, last offer sends "
            "0 but adjusts to be deleted")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance5);
        auto mm23 = root.create("mm23", minBalance5);
        auto mm34 = root.create("mm34", minBalance5);
        auto mm34b = root.create("mm34b", minBalance5);

        source.changeTrust(cur1, 200);
        mm12.changeTrust(cur1, 200);
        mm12.changeTrust(cur2, 200);
        mm23.changeTrust(cur2, 200);
        mm23.changeTrust(cur3, 200);
        mm34.changeTrust(cur3, 200);
        mm34.changeTrust(cur4, 200);
        mm34b.changeTrust(cur3, 2);
        mm34b.changeTrust(cur4, 200);
        destination.changeTrust(cur4, 200);

        gateway.pay(source, cur1, 58);
        gateway.pay(mm12, cur2, 35);
        gateway2.pay(mm23, cur3, 23);
        gateway2.pay(mm34, cur4, 16);
        gateway2.pay(mm34b, cur4, 1);

        for_versions_from(12, *app, [&] {
            auto o1a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{3, 2}, 10});
            });
            auto o1b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 16});
            });
            auto o1c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{4, 3}, 9});
            });
            auto o2a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 5});
            });
            auto o2b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{3, 2}, 6});
            });
            auto o2c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{4, 3}, 12});
            });
            auto o3a = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{4, 3}, 9});
            });
            auto o3b = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{3, 2}, 6});
            });
            auto o3c = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 1});
            });

            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 2}},
                                   {o1c.key, OfferState::DELETED},
                                   {o2a.key, {cur3, cur2, Price{2, 1}, 1}},
                                   {o2b.key, OfferState::DELETED},
                                   {o2c.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED},
                                   {o3c.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pathPaymentStrictSend(
                                                  destination, cur1, 55, cur4,
                                                  15, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1c.exchanged(9, 12),  o1a.exchanged(10, 15),
                o1b.exchanged(14, 28), o2c.exchanged(12, 16),
                o2b.exchanged(6, 9),   o2a.exchanged(4, 8),
                o3a.exchanged(9, 12),  o3b.exchanged(6, 9),
                o3c.exchanged(0, 1)};
            checkClaimedOffers(actual, expected, 55, 15);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 3}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 55}, {cur2, 2}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 33}, {cur3, 1}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 21}, {cur4, 1}}},
                 {mm34b, {{xlm, minBalance5 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 1}, {cur4, 1}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 15}}}});
            // clang-format on
        });
    }

    SECTION("with rounding errors")
    {
        auto market = TestMarket{*app};
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
        for_versions_from(12, *app, [&] {
            auto sellerOfferRemaining =
                OfferState{cny, xlm, price, 145000000 - 20039999};
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges(
                {{sellerOffer.key, sellerOfferRemaining}}, [&] {
                    actual =
                        source
                            .pathPaymentStrictSend(destination, xlm, 1382068965,
                                                   cny, 20039999, path, nullptr)
                            .success()
                            .offers;
                });
            // 20039999 = floor(1382068965 * 29 / 2000)
            auto expected = std::vector<ClaimOfferAtom>{
                sellerOffer.exchanged(20039999, 1382068965)};
            checkClaimedOffers(actual, expected, 1382068965, 20039999);
            market.requireBalances(
                {{source, {{xlm, 1989999000 - 100 - 1382068965}}},
                 {seller, {{cny, 1700000000 - 20039999}}},
                 {destination, {{cny, 20039999}}}});
        });
    }

    SECTION("with cycle")
    {
        for_versions_from(12, *app, [&] {
            // Create 3 different cycles.
            // First cycle involves 3 transaction in which buying price is
            // always half - so sender buys 8 times as much XLM as he/she
            // sells (arbitrage).
            // Second cycle involves 3 transaction in which buying price is
            // always two - so sender buys 8 times as much XLM as he/she
            // sells (anti-arbitrage). Thanks to send max option this
            // transaction is rejected.
            // Third cycle is similar to second, but send max is set to a high
            // value, so transaction proceeds even if it makes sender lose a
            // lot of XLM.

            // Each cycle is created in 3 variants (to check if behavior does
            // not depend of nativeness of asset):
            // * XLM -> USD -> IDR -> XLM
            // * USD -> IDR -> XLM -> USD
            // * IDR -> XLM -> USD -> IDR
            // To create variants, rotateRight() function is used on accounts,
            // offers and assets - it greatly simplified index calculation in
            // the code.

            auto market = TestMarket{*app};
            // amount of money that 'source' account will send
            int64_t sendAmount = 100000000;
            // amount of money in offer required to pass - needs 8x of payment
            // for anti-arbitrage case so use 10x to make sure the offers exist
            auto offerAmount = 10 * sendAmount;
            // we need twice as much money as in the offer because of
            // Price{2, 1} that is used in one case
            auto initialBalance = 2 * offerAmount;
            auto txFee = app->getLedgerManager().getLastTxFee();

            auto assets = std::deque<Asset>{xlm, usd, idr};
            int pathSize = (int)assets.size();
            auto accounts = std::deque<TestAccount>{};

            auto setupAccount = [&](const std::string& name) {
                // setup account with required trustlines and money both in
                // native and assets
                auto account = root.create(name, initialBalance);
                account.changeTrust(idr, INT64_MAX);
                gateway.pay(account, idr, initialBalance);
                account.changeTrust(usd, INT64_MAX);
                gateway2.pay(account, usd, initialBalance);

                return account;
            };

            auto validateAccountAsset = [&](const TestAccount& account,
                                            int assetIndex, int64_t difference,
                                            int feeCount) {
                if (assets[assetIndex].type() == ASSET_TYPE_NATIVE)
                {
                    REQUIRE(account.getBalance() ==
                            initialBalance + difference - feeCount * txFee);
                }
                else
                {
                    REQUIRE(account.loadTrustLine(assets[assetIndex]).balance ==
                            initialBalance + difference);
                }
            };
            auto validateAccountAssets = [&](const TestAccount& account,
                                             int assetIndex, int64_t difference,
                                             int feeCount) {
                for (int i = 0; i < pathSize; i++)
                {
                    validateAccountAsset(account, i,
                                         (assetIndex == i) ? difference : 0,
                                         feeCount);
                }
            };
            auto validateOffer = [&](const TestAccount& account,
                                     int64_t offerId, int64_t difference) {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto offer =
                    stellar::loadOffer(ltx, account.getPublicKey(), offerId);
                auto const& oe = offer.current().data.offer();
                REQUIRE(oe.amount == offerAmount + difference);
            };

            auto source = setupAccount("S");
            auto destination = setupAccount("D");

            auto validateSource = [&](int64_t difference) {
                validateAccountAssets(source, 0, difference, 3);
            };
            auto validateDestination = [&](int64_t difference) {
                validateAccountAssets(destination, 0, difference, 2);
            };

            // create account for each known asset
            for (int i = 0; i < pathSize; i++)
            {
                accounts.emplace_back(
                    setupAccount(std::string{"C"} + std::to_string(i)));
                // 2x change trust called
                validateAccountAssets(accounts[i], 0, 0, 2);
            }

            auto testPath = [&](const std::string& name, const Price& price,
                                int64_t destAmount, bool underDestMin) {
                SECTION(name)
                {
                    auto offers = std::deque<int64_t>{};
                    for (int i = 0; i < pathSize; i++)
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

                    for (int i = 0; i < pathSize; i++)
                    {
                        auto path = std::vector<Asset>{assets[1], assets[2]};
                        SECTION(std::string{"send with path ("} +
                                assetPathToString(assets) + ")")
                        {
                            auto destinationMultiplier = underDestMin ? 0 : 1;
                            auto sellerMultiplier =
                                underDestMin ? Price{0, 1} : Price{1, 1};
                            auto buyerMultiplier = sellerMultiplier * price;

                            if (underDestMin)
                            {
                                REQUIRE_THROWS_AS(
                                    source.pathPaymentStrictSend(
                                        destination, assets[0], sendAmount,
                                        assets[0], destAmount, path),
                                    ex_PATH_PAYMENT_STRICT_SEND_UNDER_DESTMIN);
                            }
                            else
                            {
                                source.pathPaymentStrictSend(
                                    destination, assets[0], sendAmount,
                                    assets[0], destAmount, path);
                            }

                            for (int j = 0; j < pathSize; j++)
                            {
                                // it is done from end of path to begin of path
                                auto index = (pathSize - j) % pathSize;
                                // sold asset
                                validateAccountAsset(
                                    accounts[index], index,
                                    -destAmount * sellerMultiplier, 3);
                                validateOffer(accounts[index], offers[index],
                                              -destAmount * sellerMultiplier);
                                // bought asset
                                validateAccountAsset(
                                    accounts[index], (index + 2) % pathSize,
                                    destAmount * buyerMultiplier, 3);
                                // ignored asset
                                validateAccountAsset(accounts[index],
                                                     (index + 1) % pathSize, 0,
                                                     3);
                                sellerMultiplier = sellerMultiplier * price;
                                buyerMultiplier = buyerMultiplier * price;
                            }

                            validateSource(-destAmount * sellerMultiplier);
                            validateDestination(destAmount *
                                                destinationMultiplier);
                        }

                        // next cycle variant
                        rotateRight(assets);
                        rotateRight(accounts);
                        rotateRight(offers);
                    }
                }
            };

            // cycle with every asset on path costing half as
            // much as previous - 8 times gain
            testPath("arbitrage", Price(1, 2), sendAmount * 8, false);
            // cycle with every asset on path costing twice as
            // much as previous - 8 times loss - unacceptable
            testPath("anti-arbitrage", Price(2, 1), sendAmount / 8 + 1, true);
            // cycle with every asset on path costing twice as
            // much as previous - 8 times loss - acceptable (but not wise to do)
            testPath("anti-arbitrage with low destmin", Price(2, 1),
                     sendAmount / 8, false);
        });
    }

    SECTION("rounding")
    {
        auto source =
            root.create("source", app->getLedgerManager().getLastMinBalance(1) +
                                      10 * txfee);
        auto mm = root.create(
            "mm", app->getLedgerManager().getLastMinBalance(4) + 10 * txfee);
        auto destination = root.create(
            "destination",
            app->getLedgerManager().getLastMinBalance(1) + 10 * txfee);

        SECTION("exchangeV10 recalculate sheepValue: 1 offer")
        {
            source.changeTrust(cur1, 3);
            mm.changeTrust(cur1, 2000);
            mm.changeTrust(cur2, 2000);
            destination.changeTrust(cur2, 2000);

            gateway.pay(source, cur1, 3);
            gateway.pay(mm, cur2, 2000);

            auto market = TestMarket{*app};
            for_versions_from(12, *app, [&] {
                auto o1 = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(mm, {cur2, cur1, Price{5, 3}, 3});
                });

                market.requireChanges({{o1.key, OfferState::DELETED}}, [&] {
                    source.pathPaymentStrictSend(destination, cur1, 3, cur2, 1,
                                                 {});
                });
                market.requireBalances({{source, {{cur1, 0}}},
                                        {mm, {{cur1, 3}, {cur2, 1999}}},
                                        {destination, {{cur2, 1}}}});
            });
        }

        SECTION("exchangeV10 recalculate sheepValue: 2 offers")
        {
            source.changeTrust(cur1, 100);
            mm.changeTrust(cur1, 2000);
            mm.changeTrust(cur2, 2000);
            destination.changeTrust(cur2, 2000);

            gateway.pay(source, cur1, 100);
            gateway.pay(mm, cur2, 2000);

            auto market = TestMarket{*app};
            for_versions_from(12, *app, [&] {
                auto o1 = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(mm, {cur2, cur1, Price{5, 3}, 3});
                });
                auto o2 = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(mm, {cur2, cur1, Price{5, 3}, 6});
                });

                market.requireChanges({{o1.key, OfferState::DELETED},
                                       {o2.key, OfferState::DELETED}},
                                      [&] {
                                          source.pathPaymentStrictSend(
                                              destination, cur1, 8, cur2, 1,
                                              {});
                                      });
                market.requireBalances({{source, {{cur1, 92}}},
                                        {mm, {{cur1, 8}, {cur2, 1996}}},
                                        {destination, {{cur2, 4}}}});
            });
        }

        SECTION("send some, receive nothing")
        {
            source.changeTrust(cur1, 100);
            mm.changeTrust(cur1, 2000);
            mm.changeTrust(cur2, 2000);
            destination.changeTrust(cur2, 2000);

            gateway.pay(source, cur1, 100);
            gateway.pay(mm, cur2, 2000);

            auto market = TestMarket{*app};
            for_versions_from(12, *app, [&] {
                auto o1 = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(mm, {cur2, cur1, Price{5, 3}, 3});
                });
                auto o2 = market.requireChangesWithOffer({}, [&] {
                    return market.addOffer(mm, {cur2, cur1, Price{5, 3}, 6});
                });

                market.requireChanges(
                    {{o1.key, OfferState::DELETED},
                     {o2.key, {cur2, cur1, Price{5, 3}, 6}}},
                    [&] {
                        auto res = source.pathPaymentStrictSend(
                            destination, cur1, 6, cur2, 1, {});
                        REQUIRE(res.success().offers.back().amountSold == 0);
                        REQUIRE(res.success().offers.back().amountBought == 1);
                    });
                market.requireBalances({{source, {{cur1, 94}}},
                                        {mm, {{cur1, 6}, {cur2, 1997}}},
                                        {destination, {{cur2, 3}}}});
            });
        }
    }

    SECTION("liabilities")
    {
        SECTION("cannot pay balance below selling liabilities")
        {
            TestMarket market(*app);
            auto source = root.create("source", minBalance2);
            auto destination = root.create("destination", minBalance2);
            auto mm12 = root.create("mm12", minBalance3);

            source.changeTrust(cur1, 200);
            mm12.changeTrust(cur1, 200);
            mm12.changeTrust(cur2, 200);
            destination.changeTrust(cur2, 200);

            gateway.pay(source, cur1, 100);
            gateway.pay(mm12, cur2, 100);

            auto offer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(source, {cur1, xlm, Price{1, 1}, 50});
            });
            auto o2 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 100});
            });

            for_versions_from(12, *app, [&] {
                REQUIRE_THROWS_AS(
                    source.pathPaymentStrictSend(destination, cur1, 51, cur2,
                                                 51, {cur1, cur2}),
                    ex_PATH_PAYMENT_STRICT_SEND_UNDERFUNDED);
                source.pathPaymentStrictSend(destination, cur1, 50, cur2, 50,
                                             {cur1, cur2});
            });
        }

        SECTION("cannot receive such that balance + buying liabilities exceeds"
                " limit")
        {
            TestMarket market(*app);
            auto source = root.create("source", minBalance2);
            auto destination = root.create("destination", minBalance2);
            auto mm12 = root.create("mm12", minBalance3);

            source.changeTrust(cur1, 200);
            mm12.changeTrust(cur1, 200);
            mm12.changeTrust(cur2, 200);
            destination.changeTrust(cur2, 200);

            gateway.pay(source, cur1, 100);
            gateway.pay(mm12, cur2, 100);
            gateway.pay(destination, cur2, 100);

            auto offer = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(destination,
                                       {xlm, cur2, Price{1, 1}, 50});
            });
            auto o2 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{1, 1}, 100});
            });

            for_versions_from(12, *app, [&] {
                REQUIRE_THROWS_AS(
                    source.pathPaymentStrictSend(destination, cur1, 51, cur2,
                                                 51, {cur1, cur2}),
                    ex_PATH_PAYMENT_STRICT_SEND_LINE_FULL);
                source.pathPaymentStrictSend(destination, cur1, 50, cur2, 50,
                                             {cur1, cur2});
            });
        }
    }
}

TEST_CASE("pathpayment strict send uses all offers in a loop",
          "[tx][pathpayment]")
{
    // This test would downgrade the bucket protocol from >12 to 12
    // with USE_CONFIG_FOR_GENESIS.  Some other tests in this module,
    // however, rely on that being set, so we separate this one
    // out into a test case with its own Application object.
    Config cfg = getTestConfig();
    cfg.USE_CONFIG_FOR_GENESIS = false;
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();
    auto& lm = app->getLedgerManager();
    auto const txfee = lm.getLastTxFee();
    auto const minBalance1 = lm.getLastMinBalance(1) + 10 * txfee;
    auto const minBalance3 = lm.getLastMinBalance(3) + 10 * txfee;
    auto const minBalance4 = lm.getLastMinBalance(4) + 10 * txfee;
    auto const minBalance5 = lm.getLastMinBalance(5) + 10 * txfee;
    auto root = TestAccount::createRoot(*app);
    auto gateway = root.create("gate1", minBalance5);
    auto gateway2 = root.create("gate2", minBalance5);

    auto useAllOffersInLoop = [&](TestAccount* issuerToDelete) {
        for_versions_from(12, *app, [&] {
            auto market = TestMarket{*app};
            auto source = root.create("source", minBalance4);
            auto destination = root.create("destination", minBalance1);
            auto mm12 = root.create("mm12", minBalance3);
            auto mm23 = root.create("mm23", minBalance3);
            auto mm34 = root.create("mm34", minBalance3);
            auto mm41 = root.create("mm41", minBalance3);
            auto xlm = makeNativeAsset();
            auto cur1 = makeAsset(gateway, "CUR1");
            auto cur2 = makeAsset(gateway, "CUR2");
            auto cur3 = makeAsset(gateway2, "CUR3");
            auto cur4 = makeAsset(gateway2, "CUR4");

            source.changeTrust(cur1, 16000);
            mm12.changeTrust(cur1, 16000);
            mm12.changeTrust(cur2, 16000);
            mm23.changeTrust(cur2, 16000);
            mm23.changeTrust(cur3, 16000);
            mm34.changeTrust(cur3, 16000);
            mm34.changeTrust(cur4, 16000);
            mm41.changeTrust(cur4, 16000);
            mm41.changeTrust(cur1, 16000);
            destination.changeTrust(cur4, 16000);

            gateway.pay(source, cur1, 8000);
            gateway.pay(mm12, cur2, 8000);
            gateway2.pay(mm23, cur3, 8000);
            gateway2.pay(mm34, cur4, 8000);
            gateway.pay(mm41, cur1, 8000);

            auto o1 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 1000});
            });
            auto o2 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 1000});
            });
            auto o3 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 1000});
            });
            auto o4 = market.requireChangesWithOffer({}, [&] {
                return market.addOffer(mm41, {cur1, cur4, Price{2, 1}, 1000});
            });

            uint32_t ledgerVersion;
            {
                LedgerTxn ltx(app->getLedgerTxnRoot());
                ledgerVersion = ltx.loadHeader().current().ledgerVersion;
            }
            if (issuerToDelete && ledgerVersion >= 13)
            {
                closeLedgerOn(*app, 2, 1, 1, 2016);
                // remove issuer
                issuerToDelete->merge(root);
            }

            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges(
                {{o1.key, {cur2, cur1, Price{2, 1}, 320}},
                 {o2.key, {cur3, cur2, Price{2, 1}, 660}},
                 {o3.key, {cur4, cur3, Price{2, 1}, 830}},
                 {o4.key, {cur1, cur4, Price{2, 1}, 920}}},
                [&] {
                    actual = source
                                 .pathPaymentStrictSend(
                                     destination, cur1, 1280, cur4, 10,
                                     {cur2, cur3, cur4, cur1, cur2, cur3})
                                 .success()
                                 .offers;
                });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(640, 1280), o2.exchanged(320, 640),
                o3.exchanged(160, 320),  o4.exchanged(80, 160),
                o1.exchanged(40, 80),    o2.exchanged(20, 40),
                o3.exchanged(10, 20)};
            checkClaimedOffers(actual, expected, 1280, 10);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 6720}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 1360}, {cur2, 7320}, {cur3, 0}, {cur4, 0}}},
                {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 680}, {cur3, 7660}, {cur4, 0}}},
                {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 340}, {cur4, 7830}}},
                {mm41, {{xlm, minBalance3 - 3 * txfee}, {cur1, 7920}, {cur2, 0}, {cur3, 0}, {cur4, 160}}},
                {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    };

    SECTION("no issuers missing")
    {
        useAllOffersInLoop(nullptr);
    }

    SECTION("outside issuers missing")
    {
        useAllOffersInLoop(&gateway);
    }

    SECTION("inside issuers missing")
    {
        useAllOffersInLoop(&gateway2);
    }
}