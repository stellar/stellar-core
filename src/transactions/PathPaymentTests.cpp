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
#include <limits>

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

TEST_CASE("pathpayment", "[tx][pathpayment]")
{
    auto const& cfg = getTestConfig();

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    // set up world
    auto root = TestAccount::createRoot(*app);
    auto xlm = makeNativeAsset();
    auto txfee = app->getLedgerManager().getTxFee();

    auto const minBalanceNoTx = app->getLedgerManager().getMinBalance(0);
    auto const minBalance =
        app->getLedgerManager().getMinBalance(0) + 10 * txfee;

    auto const minBalance1 =
        app->getLedgerManager().getMinBalance(1) + 10 * txfee;
    auto const minBalance2 =
        app->getLedgerManager().getMinBalance(2) + 10 * txfee;
    auto const minBalance3 =
        app->getLedgerManager().getMinBalance(3) + 10 * txfee;
    auto const minBalance4 =
        app->getLedgerManager().getMinBalance(4) + 10 * txfee;
    auto const minBalance5 =
        app->getLedgerManager().getMinBalance(5) + 10 * txfee;

    auto const paymentAmount = minBalance3;
    auto const morePayment = paymentAmount / 2;
    auto const trustLineLimit = INT64_MAX;

    // sets up gateway account
    auto const gatewayPayment = minBalance2 + morePayment;
    auto gateway = root.create("gate", gatewayPayment);

    // sets up gateway2 account
    auto gateway2 = root.create("gate2", gatewayPayment);

    auto idr = makeAsset(gateway, "IDR");
    auto cur1 = makeAsset(gateway, "CUR1");
    auto cur2 = makeAsset(gateway, "CUR2");
    auto usd = makeAsset(gateway2, "USD");
    auto cur3 = makeAsset(gateway2, "CUR3");
    auto cur4 = makeAsset(gateway2, "CUR4");

    closeLedgerOn(*app, 2, 1, 1, 2016);

    SECTION("path payment destination amount 0")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, idr, 10, idr, 0, {}),
                              ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination amount negative")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, idr, 10, idr, -1, {}),
                              ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment send max 0")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, idr, 0, idr, 10, {}),
                              ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment send max negative")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, idr, -1, idr, 10, {}),
                              ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment send currency invalid")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                source.pay(destination, makeInvalidAsset(), 10, idr, 10, {}),
                ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination currency invalid")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                source.pay(destination, idr, 10, makeInvalidAsset(), 10, {}),
                ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination path currency invalid")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                source.pay(destination, idr, 10, idr, 10, {makeInvalidAsset()}),
                ex_PATH_PAYMENT_MALFORMED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("dest amount too big for XLM")
    {
        auto a = root.create("a", minBalance1);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(root.pay(a, xlm, 20, xlm,
                                       std::numeric_limits<int64_t>::max(), {}),
                              ex_PATH_PAYMENT_MALFORMED);
        });
    }

    SECTION("dest amount too big for asset")
    {
        auto a = root.create("a", minBalance1);
        a.changeTrust(idr, std::numeric_limits<int64_t>::max());
        gateway.pay(a, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(gateway.pay(a, idr, 20, idr,
                                          std::numeric_limits<int64_t>::max(),
                                          {}),
                              ex_PATH_PAYMENT_LINE_FULL);
        });
    }

    SECTION("path payment XLM with not enough funds")
    {
        auto market = TestMarket{*app};
        // see https://github.com/stellar/stellar-core/pull/1239
        auto minimumAccount =
            root.create("minimum-account", minBalanceNoTx + 2 * txfee + 20);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                minimumAccount.pay(root, xlm, txfee + 21, xlm, txfee + 21, {}),
                ex_PATH_PAYMENT_UNDERFUNDED);
            // clang-format off
            market.requireBalances(
                {{minimumAccount, {{xlm, minBalanceNoTx + txfee + 20}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment asset with not enough funds")
    {
        auto market = TestMarket{*app};
        auto minimumAccount = root.create("minimum-account", minBalance1);
        auto destination = root.create("destination", minBalance1);
        minimumAccount.changeTrust(idr, 20);
        destination.changeTrust(idr, 20);
        gateway.pay(minimumAccount, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(minimumAccount.pay(gateway, idr, 11, idr, 11, {}),
                              ex_PATH_PAYMENT_UNDERFUNDED);
            // clang-format off
            market.requireBalances(
                {{minimumAccount, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                minimumAccount.pay(destination, idr, 11, idr, 11, {}),
                ex_PATH_PAYMENT_UNDERFUNDED);
            // clang-format off
            market.requireBalances(
                {{minimumAccount, {{xlm, minBalance1 - 3 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment source does not have trustline")
    {
        auto market = TestMarket{*app};
        auto noSourceTrust = root.create("no-source-trust", minBalance);
        auto destination = root.create("destination", minBalance1);
        destination.changeTrust(idr, 20);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(noSourceTrust.pay(gateway, idr, 1, idr, 1, {}),
                              ex_PATH_PAYMENT_SRC_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{noSourceTrust, {{xlm, minBalance - txfee}, {idr, 0}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                noSourceTrust.pay(destination, idr, 1, idr, 1, {}),
                ex_PATH_PAYMENT_SRC_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{noSourceTrust, {{xlm, minBalance - 2 * txfee}, {idr, 0}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment source is not authorized")
    {
        auto market = TestMarket{*app};
        auto noAuthorizedSourceTrust =
            root.create("no-authorized-source-trust", minBalance1);
        auto destination = root.create("destination", minBalance1);
        auto setFlags = uint32_t{AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG};
        noAuthorizedSourceTrust.changeTrust(idr, 20);
        gateway.pay(noAuthorizedSourceTrust, idr, 10);
        destination.changeTrust(idr, 20);
        gateway.setOptions(nullptr, &setFlags, nullptr, nullptr, nullptr,
                           nullptr);
        gateway.denyTrust(idr, noAuthorizedSourceTrust);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                noAuthorizedSourceTrust.pay(gateway, idr, 10, idr, 10, {}),
                ex_PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{noAuthorizedSourceTrust, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                noAuthorizedSourceTrust.pay(destination, idr, 10, idr, 10, {}),
                ex_PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{noAuthorizedSourceTrust, {{xlm, minBalance1 - 3 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination does not exists")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                source.pay(
                    getAccount("non-existing-destination").getPublicKey(), idr,
                    10, idr, 10, {}),
                ex_PATH_PAYMENT_NO_DESTINATION);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination is issuer and does not exists for simple "
            "paths")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            gateway.merge(root);
            auto offers = source.pay(gateway, idr, 10, idr, 10, {});
            auto expected = std::vector<ClaimOfferAtom>{};
            REQUIRE(offers.success().offers == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination is issuer and does not exists for "
            "complex paths")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            gateway.merge(root);
            REQUIRE_THROWS_AS(source.pay(gateway, idr, 10, usd, 10, {}),
                              ex_PATH_PAYMENT_NO_DESTINATION);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination does not have trustline")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto noDestinationTrust =
            root.create("no-destination-trust", minBalance);
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                gateway.pay(noDestinationTrust, idr, 1, idr, 1, {}),
                ex_PATH_PAYMENT_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}},
                 {noDestinationTrust, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                source.pay(noDestinationTrust, idr, 1, idr, 1, {}),
                ex_PATH_PAYMENT_NO_TRUST);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {noDestinationTrust, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination is not authorized")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto noAuthorizedDestinationTrust =
            root.create("no-authorized-destination-trust", minBalance1);
        auto setFlags = uint32_t{AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG};
        source.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        noAuthorizedDestinationTrust.changeTrust(idr, 20);
        gateway.setOptions(nullptr, &setFlags, nullptr, nullptr, nullptr,
                           nullptr);
        gateway.denyTrust(idr, noAuthorizedDestinationTrust);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(
                gateway.pay(noAuthorizedDestinationTrust, idr, 10, idr, 10, {}),
                ex_PATH_PAYMENT_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}},
                 {noAuthorizedDestinationTrust, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(
                source.pay(noAuthorizedDestinationTrust, idr, 10, idr, 10, {}),
                ex_PATH_PAYMENT_NOT_AUTHORIZED);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {noAuthorizedDestinationTrust, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination line full")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(idr, 20);
        gateway.pay(source, idr, 10);
        gateway.pay(destination, idr, 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(gateway.pay(destination, idr, 1, idr, 11, {}),
                              ex_PATH_PAYMENT_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(source.pay(destination, idr, 11, idr, 11, {}),
                              ex_PATH_PAYMENT_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment destination line overflow")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(idr, std::numeric_limits<int64_t>::max());
        gateway.pay(source, idr, 10);
        gateway.pay(destination, idr, std::numeric_limits<int64_t>::max() - 10);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(gateway.pay(destination, idr, 1, idr, 11, {}),
                              ex_PATH_PAYMENT_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, std::numeric_limits<int64_t>::max() - 10}, {usd, 0}}}});
            // clang-format on
            REQUIRE_THROWS_AS(source.pay(destination, idr, 11, idr, 11, {}),
                              ex_PATH_PAYMENT_LINE_FULL);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, std::numeric_limits<int64_t>::max() - 10}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment send issuer missing")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(usd, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            gateway.merge(root);
            REQUIRE_THROWS_AS(
                source.pay(destination, idr, 11, usd, 11, {}, &idr),
                ex_PATH_PAYMENT_NO_ISSUER);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment middle issuer missing")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(usd, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            auto btc = makeAsset(getAccount("missing"), "BTC");
            REQUIRE_THROWS_AS(
                source.pay(destination, idr, 11, usd, 11, {btc}, &btc),
                ex_PATH_PAYMENT_NO_ISSUER);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment last issuer missing")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 20);
        destination.changeTrust(usd, 20);
        gateway.pay(source, idr, 10);
        for_all_versions(*app, [&] {
            gateway2.merge(root);
            REQUIRE_THROWS_AS(
                source.pay(destination, idr, 11, usd, 11, {}, &usd),
                ex_PATH_PAYMENT_NO_ISSUER);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment not enough offers for first exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_TOO_FEW_OFFERS);
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

    SECTION("path payment not enough offers for middle exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_TOO_FEW_OFFERS);
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

    SECTION("path payment not enough offers for last exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_TOO_FEW_OFFERS);
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

    SECTION("path payment crosses own offer for first exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance3 - 4 * txfee}, {cur1, 10}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment crosses own offer for middle exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment crosses own offer for last exchange")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_OFFER_CROSS_SELF);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 5 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment does not cross own offer if better is available for "
            "first exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
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

    SECTION("path payment does not cross own offer if better is available for "
            "middle exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
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

    SECTION("path payment does not cross own offer if better is available for "
            "last exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
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

    SECTION("path payment over send max XLM")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance);
        auto destination = root.create("destination", minBalance);
        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, xlm, 10, xlm, 11, {}),
                              ex_PATH_PAYMENT_OVER_SENDMAX);
            market.requireBalances(
                {{source, {{xlm, minBalance - txfee}, {idr, 0}, {usd, 0}}},
                 {destination, {{xlm, minBalance}, {idr, 0}, {usd, 0}}}});
        });
    }

    SECTION("path payment over send max asset")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance1);
        auto destination = root.create("destination", minBalance1);
        source.changeTrust(idr, 10);
        destination.changeTrust(idr, 10);
        gateway.pay(source, idr, 10);

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(source.pay(destination, idr, 9, idr, 10, {}),
                              ex_PATH_PAYMENT_OVER_SENDMAX);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {idr, 10}, {usd, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {idr, 0}, {usd, 0}}}});
            // clang-format on
        });
    }

    SECTION("path payment over send max with real path")
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

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, cur1, 10, cur4,
                                                 10, {cur1, cur2, cur3, cur4});
                                  }),
                              ex_PATH_PAYMENT_OVER_SENDMAX);
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

    SECTION("path payment to self XLM")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance + txfee + 20);

        for_versions_to(7, *app, [&] {
            auto offers = account.pay(account, xlm, 20, xlm, 20, {});
            auto expected = std::vector<ClaimOfferAtom>{};
            REQUIRE(offers.success().offers == expected);
            market.requireBalances({{account, {{xlm, minBalance}}}});
        });

        for_versions_from(8, *app, [&] {
            account.pay(account, xlm, 20, xlm, 20, {});
            market.requireBalances({{account, {{xlm, minBalance + 20}}}});
        });
    }

    SECTION("path payment to self asset")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance1 + 2 * txfee);
        account.changeTrust(idr, 20);
        gateway.pay(account, idr, 10);

        for_all_versions(*app, [&] {
            auto offers = account.pay(account, idr, 10, idr, 10, {});
            auto expected = std::vector<ClaimOfferAtom>{};
            REQUIRE(offers.success().offers == expected);
            market.requireBalances({{account, {{idr, 10}}}});
        });
    }

    SECTION("path payment to self asset over the limit")
    {
        auto market = TestMarket{*app};
        auto account = root.create("account", minBalance1 + 2 * txfee);
        account.changeTrust(idr, 20);
        gateway.pay(account, idr, 19);

        for_all_versions(*app, [&] {
            REQUIRE_THROWS_AS(account.pay(account, idr, 2, idr, 2, {}),
                              ex_PATH_PAYMENT_LINE_FULL);
            market.requireBalances({{account, {{idr, 19}}}});
        });
    }

    SECTION("path payment crosses destination offer for first exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 4 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment crosses destination offer for middle exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 4 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment crosses destination offer for last exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 10, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(10, 10),
                                                        o2.exchanged(10, 10),
                                                        o3.exchanged(10, 10)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance1 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 10}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 10}, {cur3, 0}, {cur4, 0}}},
                 {destination, {{xlm, minBalance4 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 10}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment uses whole best offer for first exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 10}},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(10, 20), o1b.exchanged(30, 60),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
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

    SECTION("path payment uses whole best offer for second exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, {cur3, cur2, Price{2, 1}, 5}},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(15, 30),
                o2b.exchanged(5, 10), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
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

    SECTION("path payment uses whole best offer for last exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, {cur4, cur3, Price{2, 1}, 2}}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(2, 4),
                o3b.exchanged(8, 16)};
            REQUIRE(actual == expected);
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

    SECTION("path payment reaches limit for offer for first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 40});
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

        mm12a.changeTrust(cur1, 5);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 2}},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(2, 4), o1b.exchanged(38, 76),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 4}, {cur2, 38}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 76}, {cur2, 2}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment reaches limit for offer for second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        mm23a.changeTrust(cur2, 5);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, {cur3, cur2, Price{2, 1}, 2}},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(2, 4),
                o2b.exchanged(18, 36), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0},  {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 4}, {cur3, 18}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 36}, {cur3, 2}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment reaches limit for offer for last exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        mm34a.changeTrust(cur3, 2);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, {cur4, cur3, Price{2, 1}, 1}}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(1, 2),
                o3b.exchanged(9, 18)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 2}, {cur4, 9}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 18}, {cur4, 1}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment missing trust line for offer for first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 40});
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

        SECTION("missing selling line")
        {
            mm12a.pay(gateway, cur2, 40);
            mm12a.changeTrust(cur2, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1a.key, OfferState::DELETED},
                     {o1b.key, OfferState::DELETED},
                     {o2.key, OfferState::DELETED},
                     {o3.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                    o2.exchanged(20, 40), o3.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12a, {{xlm, minBalance3 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }

        SECTION("missing buying line")
        {
            mm12a.changeTrust(cur1, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1a.key, OfferState::DELETED},
                     {o1b.key, OfferState::DELETED},
                     {o2.key, OfferState::DELETED},
                     {o3.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                    o2.exchanged(20, 40), o3.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }
    }

    SECTION("path payment missing trust line for offer for second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        SECTION("missing selling line")
        {
            mm23a.pay(gateway2, cur3, 20);
            mm23a.changeTrust(cur3, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1.key, OfferState::DELETED},
                     {o2a.key, OfferState::DELETED},
                     {o2b.key, OfferState::DELETED},
                     {o3.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1.exchanged(40, 80), o2a.exchanged(0, 0),
                    o2b.exchanged(20, 40), o3.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23a, {{xlm, minBalance3 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }

        SECTION("missing buying line")
        {
            mm23a.changeTrust(cur2, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1.key, OfferState::DELETED},
                     {o2a.key, OfferState::DELETED},
                     {o2b.key, OfferState::DELETED},
                     {o3.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1.exchanged(40, 80), o2a.exchanged(0, 0),
                    o2b.exchanged(20, 40), o3.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }
    }

    SECTION("path payment missing trust line for offer for last exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        SECTION("missing selling line")
        {
            mm34a.pay(gateway2, cur4, 10);
            mm34a.changeTrust(cur4, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1.key, OfferState::DELETED},
                     {o2.key, OfferState::DELETED},
                     {o3a.key, OfferState::DELETED},
                     {o3b.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1.exchanged(40, 80), o2.exchanged(20, 40),
                    o3a.exchanged(0, 0), o3b.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34a, {{xlm, minBalance3 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }

        SECTION("missing buying line")
        {
            mm34a.changeTrust(cur3, 0);

            for_all_versions(*app, [&] {
                auto actual = std::vector<ClaimOfferAtom>{};
                market.requireChanges(
                    {{o1.key, OfferState::DELETED},
                     {o2.key, OfferState::DELETED},
                     {o3a.key, OfferState::DELETED},
                     {o3b.key, OfferState::DELETED}},
                    [&] {
                        actual = source
                                     .pay(destination, cur1, 80, cur4, 10,
                                          {cur1, cur2, cur3, cur4})
                                     .success()
                                     .offers;
                    });
                auto expected = std::vector<ClaimOfferAtom>{
                    o1.exchanged(40, 80), o2.exchanged(20, 40),
                    o3a.exchanged(0, 0), o3b.exchanged(10, 20)};
                REQUIRE(actual == expected);
                // clang-format off
                market.requireBalances(
                    {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                     {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                     {mm34a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                     {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                     {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
                // clang-format on
            });
        }
    }

    SECTION("path payment empty trust line for selling asset for offer for "
            "first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 40});
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

        mm12a.pay(gateway, cur2, 40);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment empty trust line for selling asset for offer for "
            "second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        mm23a.pay(gateway2, cur3, 20);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(0, 0),
                o2b.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment empty trust line for selling asset for offer for "
            "last exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        mm34a.pay(gateway2, cur4, 10);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(0, 0),
                o3b.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment full trust line for buying asset for offer for "
            "first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 40});
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

        gateway.pay(mm12a, cur1, 200);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 200}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment full trust line for buying asset for offer for "
            "second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        gateway.pay(mm23a, cur2, 200);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(0, 0),
                o2b.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 200}, {cur3, 20}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment full trust line for buying asset for offer for last "
            "exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        gateway2.pay(mm34a, cur3, 200);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(0, 0),
                o3b.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 200}, {cur4, 10}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 in trust line for selling asset for offer for "
            "first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{1, 2}, 40});
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

        mm12a.pay(gateway, cur2, 39);

        for_versions_to(2, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 1}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
        for_versions_from(3, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 1}},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(1, 1), o1b.exchanged(39, 78),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 1}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 1}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 78}, {cur2, 1}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 in trust line for selling asset for offer for "
            "second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{1, 2}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        mm23a.pay(gateway2, cur3, 19);

        for_versions_to(2, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(0, 0),
                o2b.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 1}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
        for_versions_from(3, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, {cur2, cur1, Price{2, 1}, 1}},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, {cur3, cur2, Price{2, 1}, 1}},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(39, 78), o2a.exchanged(1, 1),
                o2b.exchanged(19, 38), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 2}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 78}, {cur2, 1}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 1}, {cur3, 0}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 38}, {cur3, 1}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 in trust line for selling asset for offer for last "
            "exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{1, 2}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        mm34a.pay(gateway2, cur4, 9);

        for_versions_to(2, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(0, 0),
                o3b.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 1}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
        for_versions_from(3, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, {cur2, cur1, Price{2, 1}, 2}},
                                   {o2.key, {cur3, cur2, Price{2, 1}, 1}},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, {cur4, cur3, Price{2, 1}, 1}}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(38, 76), o2.exchanged(19, 38), o3a.exchanged(1, 1),
                o3b.exchanged(9, 18)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 4}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 76}, {cur2, 2}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 38}, {cur3, 1}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 4 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 1}, {cur4, 0}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 18}, {cur4, 1}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 left in trust line for buying asset for offer for "
            "first exchange")
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
            return market.addOffer(mm12a, {cur2, cur1, Price{2, 1}, 40});
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

        gateway.pay(mm12a, cur1, 199);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1a.exchanged(0, 0), o1b.exchanged(40, 80),
                o2.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 199}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm12b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 left in trust line for buying asset for offer for "
            "second exchange")
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
            return market.addOffer(mm23a, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23b, {cur3, cur2, Price{2, 1}, 20});
        });
        auto o3 = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 10});
        });

        gateway.pay(mm23a, cur2, 199);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2a.key, OfferState::DELETED},
                                   {o2b.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2a.exchanged(0, 0),
                o2b.exchanged(20, 40), o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 199}, {cur3, 20}, {cur4, 0}}},
                 {mm23b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment 1 left in trust line for buying asset for offer for "
            "last exchange")
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
            return market.addOffer(mm34a, {cur4, cur3, Price{2, 1}, 10});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34b, {cur4, cur3, Price{2, 1}, 10});
        });

        gateway2.pay(mm34a, cur3, 199);

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(40, 80), o2.exchanged(20, 40), o3a.exchanged(0, 0),
                o3b.exchanged(10, 20)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance3 - 3 * txfee}, {cur1, 80}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 40}, {cur3, 0}, {cur4, 0}}},
                 {mm34a, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 199}, {cur4, 10}}},
                 {mm34b, {{xlm, minBalance3 - 3 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 20}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment takes all offers, one offer per exchange")
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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1.key, OfferState::DELETED},
                                   {o2.key, OfferState::DELETED},
                                   {o3.key, OfferState::DELETED}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 80, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{o1.exchanged(40, 80),
                                                        o2.exchanged(20, 40),
                                                        o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
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

    SECTION("path payment takes all offers, multiple offers per exchange")
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

        gateway.pay(source, cur1, 45);
        gateway.pay(mm12, cur2, 28);
        gateway2.pay(mm23, cur3, 17);
        gateway2.pay(mm34, cur4, 10);

        auto o1a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{3, 2}, 10});
        });
        auto o1b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 9});
        });
        auto o1c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{4, 3}, 9});
        });
        auto o2a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 5});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{3, 2}, 5});
        });
        auto o2c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{4, 3}, 7});
        });
        auto o3a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{4, 3}, 4});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{3, 2}, 3});
        });
        auto o3c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 3});
        });

        for_versions_to(2, *app, [&] {
            market.requireChanges({}, [&] {
                REQUIRE_THROWS_AS(source.pay(destination, cur1, 45, cur4, 10,
                                             {cur1, cur2, cur3, cur4}),
                                  ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            });
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 45}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 28}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 17}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
        for_versions_from(3, *app, [&] {
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
                                              .pay(destination, cur1, 45, cur4,
                                                   10, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1c.exchanged(9, 12), o1a.exchanged(10, 15),
                o1b.exchanged(9, 18), o2c.exchanged(7, 10),
                o2b.exchanged(5, 8),  o2a.exchanged(5, 10),
                o3a.exchanged(4, 6),  o3b.exchanged(3, 5),
                o3c.exchanged(3, 6)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 45}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 28}, {cur3, 0}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 17}, {cur4, 0}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}}});
            // clang-format on
        });
    }

    SECTION("path payment takes best offers, multiple offers per exchange")
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

        gateway.pay(source, cur1, 45);
        gateway.pay(mm12, cur2, 28);
        gateway2.pay(mm23, cur3, 17);
        gateway2.pay(mm34, cur4, 10);

        auto o1a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{3, 2}, 10});
        });
        auto o1b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{2, 1}, 9});
        });
        auto o1c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm12, {cur2, cur1, Price{4, 3}, 9});
        });
        auto o2a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{2, 1}, 5});
        });
        auto o2b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{3, 2}, 5});
        });
        auto o2c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm23, {cur3, cur2, Price{4, 3}, 7});
        });
        auto o3a = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{4, 3}, 4});
        });
        auto o3b = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{3, 2}, 3});
        });
        auto o3c = market.requireChangesWithOffer({}, [&] {
            return market.addOffer(mm34, {cur4, cur3, Price{2, 1}, 3});
        });

        for_versions_to(2, *app, [&] {
            market.requireChanges({}, [&] {
                REQUIRE_THROWS_AS(source.pay(destination, cur1, 29, cur4, 8,
                                             {cur1, cur2, cur3, cur4}),
                                  ex_PATH_PAYMENT_TOO_FEW_OFFERS);
            });
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 45}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 28}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 17}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 10}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 0}}}});
            // clang-format on
        });
        for_versions_from(3, *app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges({{o1a.key, OfferState::DELETED},
                                   {o1b.key, {cur2, cur1, Price{2, 1}, 8}},
                                   {o1c.key, OfferState::DELETED},
                                   {o2a.key, {cur3, cur2, Price{2, 1}, 4}},
                                   {o2b.key, OfferState::DELETED},
                                   {o2c.key, OfferState::DELETED},
                                   {o3a.key, OfferState::DELETED},
                                   {o3b.key, OfferState::DELETED},
                                   {o3c.key, {cur4, cur3, Price{2, 1}, 2}}},
                                  [&] {
                                      actual =
                                          source
                                              .pay(destination, cur1, 29, cur4,
                                                   8, {cur1, cur2, cur3, cur4})
                                              .success()
                                              .offers;
                                  });
            auto expected = std::vector<ClaimOfferAtom>{
                o1c.exchanged(9, 12), o1a.exchanged(10, 15),
                o1b.exchanged(1, 2),  o2c.exchanged(7, 10),
                o2b.exchanged(5, 8),  o2a.exchanged(1, 2),
                o3a.exchanged(4, 6),  o3b.exchanged(3, 5),
                o3c.exchanged(1, 2)};
            REQUIRE(actual == expected);
            // clang-format off
            market.requireBalances(
                {{source, {{xlm, minBalance4 - 2 * txfee}, {cur1, 16}, {cur2, 0}, {cur3, 0}, {cur4, 0}}},
                 {mm12, {{xlm, minBalance5 - 5 * txfee}, {cur1, 29}, {cur2, 8}, {cur3, 0}, {cur4, 0}}},
                 {mm23, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 20}, {cur3, 4}, {cur4, 0}}},
                 {mm34, {{xlm, minBalance5 - 5 * txfee}, {cur1, 0}, {cur2, 0}, {cur3, 13}, {cur4, 2}}},
                 {destination, {{xlm, minBalance1 - txfee}, {cur1, 0}, {cur2, 0}, {cur3, 0}, {cur4, 8}}}});
            // clang-format on
        });
    }

    SECTION("path payment uses all offers in a loop")
    {
        auto market = TestMarket{*app};
        auto source = root.create("source", minBalance4);
        auto destination = root.create("destination", minBalance1);
        auto mm12 = root.create("mm12", minBalance3);
        auto mm23 = root.create("mm23", minBalance3);
        auto mm34 = root.create("mm34", minBalance3);
        auto mm41 = root.create("mm41", minBalance3);

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

        for_all_versions(*app, [&] {
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges(
                {{o1.key, {cur2, cur1, Price{2, 1}, 320}},
                 {o2.key, {cur3, cur2, Price{2, 1}, 660}},
                 {o3.key, {cur4, cur3, Price{2, 1}, 830}},
                 {o4.key, {cur1, cur4, Price{2, 1}, 920}}},
                [&] {
                    actual = source
                                 .pay(destination, cur1, 2000, cur4, 10,
                                      {cur1, cur2, cur3, cur4, cur1, cur2, cur3,
                                       cur4})
                                 .success()
                                 .offers;
                });
            auto expected = std::vector<ClaimOfferAtom>{
                o1.exchanged(640, 1280), o2.exchanged(320, 640),
                o3.exchanged(160, 320),  o4.exchanged(80, 160),
                o1.exchanged(40, 80),    o2.exchanged(20, 40),
                o3.exchanged(10, 20)};
            REQUIRE(actual == expected);
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
    }

    SECTION("path payment with rounding errors")
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
        for_versions_to(2, *app, [&] {
            // bug, it should succeed
            REQUIRE_THROWS_AS(market.requireChanges(
                                  {},
                                  [&] {
                                      source.pay(destination, xlm, 1382068965,
                                                 cny, 20000000, path);
                                  }),
                              ex_PATH_PAYMENT_TOO_FEW_OFFERS);
        });
        for_versions_from(3, *app, [&] {
            auto sellerOfferRemaining =
                OfferState{cny, xlm, price, 145000000 - 20000000};
            auto actual = std::vector<ClaimOfferAtom>{};
            market.requireChanges(
                {{sellerOffer.key, sellerOfferRemaining}}, [&] {
                    actual = source
                                 .pay(destination, xlm, 1382068965, cny,
                                      20000000, path, nullptr)
                                 .success()
                                 .offers;
                });
            // 1379310345 = round up(20000000 * price)
            auto expected = std::vector<ClaimOfferAtom>{
                sellerOffer.exchanged(20000000, 1379310345)};
            REQUIRE(actual == expected);
            market.requireBalances(
                {{source, {{xlm, 1989999000 - 100 - 1379310345}}},
                 {seller, {{cny, 1680000000}}},
                 {destination, {{cny, 20000000}}}});
        });
    }

    SECTION("path with bogus offer, bogus offer shows on "
            "offers trail")
    {
        auto market = TestMarket{*app};

        auto paymentToReceive = 240000000;
        auto offerSize = paymentToReceive / 2;
        auto initialBalance =
            app->getLedgerManager().getMinBalance(10) + txfee * 10 + 1000000000;
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
            return market.addOffer(mm, {usd, xlm, Price{1, 2}, 2 * offerSize});
        });

        auto path = std::vector<Asset>{xlm, usd};

        for_versions_to(2, *app, [&] {
            auto res = source.pay(destination, xlm, 8 * paymentToReceive, idr,
                                  paymentToReceive, path);

            auto expected = std::vector<ClaimOfferAtom>{
                usdCurOffer.exchanged(90000000, 45000000),
                idrCurCheapOffer.exchanged(120000000, 30000000),
                idrCurMidBogusOffer.exchanged(0, 0),
                idrCurExpensiveOffer.exchanged(120000000, 60000000)};
            REQUIRE(res.success().offers == expected);
        });
        for_versions_from(3, *app, [&] {
            auto res = source.pay(destination, xlm, 8 * paymentToReceive, idr,
                                  paymentToReceive, path);

            auto expected = std::vector<ClaimOfferAtom>{
                usdCurOffer.exchanged(90000001, 45000001),
                idrCurCheapOffer.exchanged(120000000, 30000000),
                idrCurMidBogusOffer.exchanged(1, 1),
                idrCurExpensiveOffer.exchanged(119999999, 60000000)};
            REQUIRE(res.success().offers == expected);
        });
    }

    SECTION("path payment with cycle")
    {
        for_all_versions(*app, [&] {
            // Create 3 different cycles.
            // First cycle involves 3 transaction in which
            // buying price is
            // always half - so sender buys 8 times as much XLM
            // as he/she
            // sells (arbitrage).
            // Second cycle involves 3 transaction in which
            // buying price is
            // always two - so sender buys 8 times as much XLM
            // as he/she
            // sells (anti-arbitrage).
            // Thanks to send max option this transaction is
            // rejected.
            // Third cycle is similar to second, but send max is
            // set to a
            // high value, so transaction proceeds even if it
            // makes sender
            // lose a lot of XLM.

            // Each cycle is created in 3 variants (to check if
            // behavior
            // does not depend of nativeness of asset):
            // * XLM -> USD -> IDR -> XLM
            // * USD -> IDR -> XLM -> USD
            // * IDR -> XLM -> USD -> IDR
            // To create variants, rotateRight() function is
            // used on
            // accounts, offers and assets -
            // it greatly simplified index calculation in the
            // code.

            auto market = TestMarket{*app};
            auto paymentAmount = int64_t{100000000}; // amount of money that
                                                     // 'destination'
                                                     // account will receive
            auto offerAmount = 8 * paymentAmount;    // amount of money in
            // offer required to pass
            // - needs 8x of payment
            // for anti-arbitrage case
            auto initialBalance =
                2 * offerAmount; // we need twice as much money
                                 // as in the
            // offer because of Price{2, 1} that is
            // used in one case
            auto txFee = app->getLedgerManager().getTxFee();

            auto assets = std::deque<Asset>{xlm, usd, idr};
            int pathSize = (int)assets.size();
            auto accounts = std::deque<TestAccount>{};

            auto setupAccount = [&](const std::string& name) {
                // setup account with required trustlines and
                // money both in
                // native and assets

                auto account = root.create(name, initialBalance);
                account.changeTrust(idr, trustLineLimit);
                gateway.pay(account, idr, initialBalance);
                account.changeTrust(usd, trustLineLimit);
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
                    REQUIRE(loadTrustLine(account, assets[assetIndex], *app)
                                ->getBalance() == initialBalance + difference);
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
            auto validateOffer = [offerAmount](const TestAccount& account,
                                               uint64_t offerId,
                                               int64_t difference) {
                auto offer = account.loadOffer(offerId);
                REQUIRE(offer.amount == offerAmount + difference);
            };

            auto source = setupAccount("S");
            auto destination = setupAccount("D");

            auto validateSource = [&](int64_t difference) {
                validateAccountAssets(source, 0, difference, 3);
            };
            auto validateDestination = [&](int64_t difference) {
                validateAccountAssets(destination, 0, difference, 2);
            };

            for (int i = 0; i < pathSize;
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

                            for (int j = 0; j < pathSize; j++)
                            {
                                auto index = (pathSize - j) %
                                             pathSize; // it is done from
                                                       // end of path to
                                                       // begin of path
                                validateAccountAsset(accounts[index], index,
                                                     -paymentAmount *
                                                         sellerMultipler,
                                                     3); // sold asset
                                validateOffer(accounts[index], offers[index],
                                              -paymentAmount *
                                                  sellerMultipler); // sold
                                                                    // asset
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

            // cycle with every asset on path costing half as
            // much as
            // previous - 8 times gain
            testPath("arbitrage", Price(1, 2), 1, false);
            // cycle with every asset on path costing twice as
            // much as
            // previous - 8 times loss - unacceptable
            testPath("anti-arbitrage", Price(2, 1), 1, true);
            // cycle with every asset on path costing twice as
            // much as
            // previous - 8 times loss - acceptable (but not
            // wise to do)
            testPath("anti-arbitrage with big sendmax", Price(2, 1), 8, false);
        });
    }
}
