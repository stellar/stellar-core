// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/LiabilitiesMatchOffers.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Math.h"
#include <random>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

LedgerEntry
updateAccountWithRandomBalance(LedgerEntry le, Application& app,
                               bool exceedsMinimum, int32_t direction)
{
    auto& account = le.data.account();

    auto minBalance = getMinBalance(app, account);

    int64_t lbound = 0;
    int64_t ubound = std::numeric_limits<int64_t>::max();
    if (direction > 0)
    {
        lbound = account.balance + 1;
    }
    else if (direction < 0)
    {
        ubound = account.balance - 1;
    }
    if (exceedsMinimum)
    {
        lbound = std::max(lbound, minBalance);
    }
    else
    {
        ubound = std::min(ubound, minBalance - 1);
    }
    REQUIRE(lbound <= ubound);

    std::uniform_int_distribution<int64_t> dist(lbound, ubound);
    account.balance = dist(gRandomEngine);
    return le;
}

TEST_CASE("Create account above minimum balance",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, true, 0);
        REQUIRE(store(*app, makeUpdateList({le}, nullptr)));
    }
}

TEST_CASE("Create account below minimum balance",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
}

TEST_CASE("Create account then decrease balance below minimum",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, true, 0);
        REQUIRE(store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le2}, {le1})));
    }
}

TEST_CASE("Account below minimum balance increases but stays below minimum",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, false, 1);
        REQUIRE(store(*app, makeUpdateList({le2}, {le1})));
    }
}

TEST_CASE("Account below minimum balance decreases",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, false, -1);
        REQUIRE(!store(*app, makeUpdateList({le2}, {le1})));
    }
}

static LedgerEntry
generateOffer(Asset const& selling, Asset const& buying, int64_t amount,
              Price price)
{
    REQUIRE(!(selling == buying));
    REQUIRE(amount >= 1);

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 2;
    le.data.type(OFFER);

    auto offer = LedgerTestUtils::generateValidOfferEntry();
    offer.amount = amount;
    offer.price = price;
    offer.selling = selling;
    offer.buying = buying;

    le.data.offer() = offer;
    return le;
}

static LedgerEntry
generateSellingLiabilities(Application& app, LedgerEntry offer, bool excess,
                           uint32_t authorized)
{
    auto const& oe = offer.data.offer();

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 2;

    if (oe.selling.type() == ASSET_TYPE_NATIVE)
    {
        auto account = LedgerTestUtils::generateValidAccountEntry();
        account.accountID = oe.sellerID;

        auto minBalance = getMinBalance(app, account) + oe.amount;
        account.balance = excess ? std::min(account.balance, minBalance - 1)
                                 : std::max(account.balance, minBalance);

        account.ext.v(1);
        account.ext.v1().liabilities = Liabilities{0, oe.amount};

        le.data.type(ACCOUNT);
        le.data.account() = account;
    }
    else
    {
        auto trust = LedgerTestUtils::generateValidTrustLineEntry();
        trust.accountID = oe.sellerID;
        trust.flags = authorized;

        trust.asset = oe.selling;
        trust.balance = excess ? std::min(trust.balance, oe.amount - 1)
                               : std::max(trust.balance, oe.amount);
        trust.limit = std::max({trust.balance, trust.limit});

        trust.ext.v(1);
        trust.ext.v1().liabilities = Liabilities{0, oe.amount};

        le.data.type(TRUSTLINE);
        le.data.trustLine() = trust;
    }
    return le;
}

static LedgerEntry
generateBuyingLiabilities(Application& app, LedgerEntry offer, bool excess,
                          uint32_t authorized)
{
    auto const& oe = offer.data.offer();

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 2;

    if (oe.buying.type() == ASSET_TYPE_NATIVE)
    {
        auto account = LedgerTestUtils::generateValidAccountEntry();
        account.accountID = oe.sellerID;

        auto minBalance = getMinBalance(app, account);
        auto maxBalance = INT64_MAX - oe.amount;

        // There is a possible issue here where minBalance can be greater than
        // maxBalance, which would cause the test to fail. For this to happen,
        // oe.amount would need to be greater than INT64_MAX -
        // (numSponsoring * baseReserve (UINT32_MAX * 100000000 in the largest
        // case) - numSponsored * baseReserve + numSubEntries). This isn't an
        // issue now since the order sizes passed to this function at the moment
        // are < 1000, but it's something to keep in mind.

        account.balance =
            excess
                ? std::max(account.balance, maxBalance + 1)
                : std::min(std::max(account.balance, minBalance), maxBalance);

        account.ext.v(1);
        account.ext.v1().liabilities = Liabilities{oe.amount, 0};

        le.data.type(ACCOUNT);
        le.data.account() = account;
    }
    else
    {
        auto trust = LedgerTestUtils::generateValidTrustLineEntry();
        trust.accountID = oe.sellerID;
        trust.flags = authorized;

        trust.asset = oe.buying;
        trust.limit = std::max({trust.limit, oe.amount});
        auto maxBalance = trust.limit - oe.amount;
        trust.balance = excess ? std::max(trust.balance, maxBalance + 1)
                               : std::min(trust.balance, maxBalance);

        trust.ext.v(1);
        trust.ext.v1().liabilities = Liabilities{oe.amount, 0};

        le.data.type(TRUSTLINE);
        le.data.trustLine() = trust;
    }
    return le;
}

TEST_CASE("Create account then increase liabilities without changing balance",
          "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    Asset native;

    Asset cur1;
    cur1.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur1.alphaNum4().assetCode, "CUR1");

    auto offer = generateOffer(native, cur1, 100, Price{1, 1});
    auto selling =
        generateSellingLiabilities(*app, offer, true, AUTHORIZED_FLAG);

    // we want to set the balance to the absolute minimum balance required.
    selling.data.account().balance =
        getMinBalance(*app, selling.data.account()) + offer.data.offer().amount;

    auto buying =
        generateBuyingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
    std::vector<LedgerEntry> entries{offer, selling, buying};
    auto updates = makeUpdateList(entries, nullptr);
    REQUIRE(store(*app, updates));

    auto offer2 = offer;
    ++offer2.data.offer().amount;
    auto selling2 = selling;
    ++selling2.data.account().ext.v1().liabilities.selling;
    auto buying2 = buying;
    ++buying2.data.trustLine().ext.v1().liabilities.buying;
    std::vector<LedgerEntry> entries2{offer2, selling2, buying2};
    auto updates2 = makeUpdateList(entries2, entries);
    REQUIRE(!store(*app, updates2));
}

TEST_CASE("Invariant for liabilities", "[invariant][liabilitiesmatchoffers]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"LiabilitiesMatchOffers"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    Asset native;

    Asset cur1;
    cur1.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur1.alphaNum4().assetCode, "CUR1");

    Asset cur2;
    cur2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur2.alphaNum4().assetCode, "CUR2");

    auto assetCode = [](auto const& asset) {
        if (asset.type() == ASSET_TYPE_NATIVE)
        {
            return std::string("NATIVE");
        }
        else
        {
            std::string code;
            assetCodeToStr(asset.alphaNum4().assetCode, code);
            return code;
        }
    };

    auto forAssets = [&](auto f) {
        for (auto curA : {native, cur1, cur2})
        {
            for (auto curB : {native, cur1, cur2})
            {
                if (!(curA == curB))
                {
                    SECTION(assetCode(curA) + " and " + assetCode(curB))
                    {
                        f(curA, curB);
                    }
                }
            }
        }
    };

    SECTION("create then modify then delete offer")
    {
        forAssets([&](auto const& curA, auto const& curB) {
            auto offer = generateOffer(curA, curB, 100, Price{1, 1});
            auto selling =
                generateSellingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
            auto buying =
                generateBuyingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
            std::vector<LedgerEntry> entries{offer, selling, buying};
            auto updates = makeUpdateList(entries, nullptr);
            REQUIRE(store(*app, updates));

            auto offer2 = generateOffer(curA, curB, 200, Price{1, 1});
            offer2.data.offer().sellerID = offer.data.offer().sellerID;
            offer2.data.offer().offerID = offer.data.offer().offerID;
            auto selling2 = generateSellingLiabilities(*app, offer2, false,
                                                       AUTHORIZED_FLAG);
            auto buying2 =
                generateBuyingLiabilities(*app, offer2, false, AUTHORIZED_FLAG);
            std::vector<LedgerEntry> entries2{offer2, selling2, buying2};
            auto updates2 = makeUpdateList(entries2, entries);
            REQUIRE(store(*app, updates2));

            auto updates3 = makeUpdateList(nullptr, entries2);
            REQUIRE(store(*app, updates3));
        });
    }

    SECTION("create offer with excess liabilities")
    {
        forAssets([&](auto const& curA, auto const& curB) {
            auto verify = [&](bool excessSelling, uint32_t authorizedSelling,
                              bool excessBuying, uint32_t authorizedBuying) {
                auto offer = generateOffer(curA, curB, 100, Price{1, 1});
                auto selling = generateSellingLiabilities(
                    *app, offer, excessSelling, authorizedSelling);
                auto buying = generateBuyingLiabilities(
                    *app, offer, excessBuying, authorizedBuying);
                std::vector<LedgerEntry> entries{offer, selling, buying};
                auto updates = makeUpdateList(entries, nullptr);
                REQUIRE(!store(*app, updates));
            };

            SECTION("excess selling")
            {
                verify(true, AUTHORIZED_FLAG, false, AUTHORIZED_FLAG);
            }

            if (curA.type() != ASSET_TYPE_NATIVE)
            {
                SECTION("unauthorized selling")
                {
                    verify(false, 0, false, AUTHORIZED_FLAG);
                }
            }

            SECTION("excess buying")
            {
                verify(false, AUTHORIZED_FLAG, true, AUTHORIZED_FLAG);
            }

            if (curB.type() != ASSET_TYPE_NATIVE)
            {
                SECTION("unauthorized buying")
                {
                    verify(false, AUTHORIZED_FLAG, false, 0);
                }
            }
        });
    }

    SECTION("modify offer to have excess liabilities")
    {
        forAssets([&](auto const& curA, auto const& curB) {
            auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
            auto selling =
                generateSellingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
            auto buying =
                generateBuyingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
            std::vector<LedgerEntry> entries{offer, selling, buying};
            auto updates = makeUpdateList(entries, nullptr);
            REQUIRE(store(*app, updates));

            auto verify = [&](bool excessSelling, uint32_t authorizedSelling,
                              bool excessBuying, uint32_t authorizedBuying) {
                auto offer2 = generateOffer(cur1, cur2, 200, Price{1, 1});
                offer2.data.offer().sellerID = offer.data.offer().sellerID;
                offer2.data.offer().offerID = offer.data.offer().offerID;
                auto selling2 = generateSellingLiabilities(
                    *app, offer, excessSelling, authorizedSelling);
                auto buying2 = generateBuyingLiabilities(
                    *app, offer, excessBuying, authorizedBuying);
                std::vector<LedgerEntry> entries2{offer2, selling2, buying2};
                auto updates2 = makeUpdateList(entries2, entries);
                REQUIRE(!store(*app, updates2));
            };

            SECTION("excess selling")
            {
                verify(true, AUTHORIZED_FLAG, false, AUTHORIZED_FLAG);
            }

            if (curA.type() != ASSET_TYPE_NATIVE)
            {
                SECTION("unauthorized selling")
                {
                    verify(false, 0, false, AUTHORIZED_FLAG);
                }
            }

            SECTION("excess buying")
            {
                verify(false, AUTHORIZED_FLAG, true, AUTHORIZED_FLAG);
            }

            if (curB.type() != ASSET_TYPE_NATIVE)
            {
                SECTION("unauthorized buying")
                {
                    verify(false, AUTHORIZED_FLAG, false, 0);
                }
            }
        });
    }

    SECTION("revoke authorization")
    {
        auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
        auto selling =
            generateSellingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
        auto buying =
            generateBuyingLiabilities(*app, offer, false, AUTHORIZED_FLAG);
        std::vector<LedgerEntry> entries{offer, selling, buying};
        auto updates = makeUpdateList(entries, nullptr);
        REQUIRE(store(*app, updates));

        SECTION("selling auth")
        {
            auto selling2 = generateSellingLiabilities(*app, offer, false, 0);
            REQUIRE(!store(*app, makeUpdateList({selling2}, {selling})));
        }
        SECTION("buying auth")
        {
            auto buying2 = generateBuyingLiabilities(*app, offer, false, 0);
            REQUIRE(!store(*app, makeUpdateList({buying2}, {buying})));
        }
    }

    SECTION("increase liabilities with AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG")
    {
        auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
        auto selling = generateSellingLiabilities(*app, offer, false, true);
        auto buying = generateBuyingLiabilities(*app, offer, false, true);
        std::vector<LedgerEntry> entries{offer, selling, buying};
        auto updates = makeUpdateList(entries, nullptr);
        REQUIRE(store(*app, updates));

        auto offer2 = generateOffer(cur1, cur2, 200, Price{1, 1});
        offer2.data.offer().sellerID = offer.data.offer().sellerID;
        offer2.data.offer().offerID = offer.data.offer().offerID;

        SECTION("selling auth")
        {
            auto selling2 = generateSellingLiabilities(
                *app, offer2, false, AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG);
            REQUIRE(!store(*app, makeUpdateList({selling2}, {selling})));
        }
        SECTION("buying auth")
        {
            auto buying2 = generateBuyingLiabilities(
                *app, offer2, false, AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG);
            REQUIRE(!store(*app, makeUpdateList({buying2}, {buying})));
        }
    }
}
