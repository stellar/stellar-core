// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/InvariantTestUtils.h"
#include "invariant/LiabilitiesMatchOffers.h"
#include "ledger/LedgerTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include <random>

using namespace stellar;
using namespace stellar::InvariantTestUtils;

LedgerEntry
updateAccountWithRandomBalance(LedgerEntry le, Application& app,
                               std::default_random_engine& gen,
                               bool exceedsMinimum, int32_t direction)
{
    auto& account = le.data.account();

    auto minBalance =
        app.getLedgerManager().getLastMinBalance(account.numSubEntries);

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
    account.balance = dist(gen);
    return le;
}

TEST_CASE("Create account above minimum balance",
          "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, gen, true, 0);
        REQUIRE(store(*app, makeUpdateList({le}, nullptr)));
    }
}

TEST_CASE("Create account below minimum balance",
          "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le = generateRandomAccount(2);
        le = updateAccountWithRandomBalance(le, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le}, nullptr)));
    }
}

TEST_CASE("Create account then decrease balance below minimum",
          "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, true, 0);
        REQUIRE(store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le2}, {le1})));
    }
}

TEST_CASE("Account below minimum balance increases but stays below minimum",
          "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, 1);
        REQUIRE(store(*app, makeUpdateList({le2}, {le1})));
    }
}

TEST_CASE("Account below minimum balance decreases",
          "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    for (uint32_t i = 0; i < 10; ++i)
    {
        VirtualClock clock;
        Application::pointer app = createTestApplication(clock, cfg);

        auto le1 = generateRandomAccount(2);
        le1 = updateAccountWithRandomBalance(le1, *app, gen, false, 0);
        REQUIRE(!store(*app, makeUpdateList({le1}, nullptr)));
        auto le2 = updateAccountWithRandomBalance(le1, *app, gen, false, -1);
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
                           bool authorized)
{
    auto const& oe = offer.data.offer();

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 2;

    if (oe.selling.type() == ASSET_TYPE_NATIVE)
    {
        auto account = LedgerTestUtils::generateValidAccountEntry();
        account.accountID = oe.sellerID;

        int64_t minBalance = 0;
        {
            LedgerTxn ltx(app.getLedgerTxnRoot());
            minBalance =
                getMinBalance(ltx.loadHeader(), account.numSubEntries) +
                oe.amount;
        }
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
        if (authorized)
        {
            trust.flags |= AUTHORIZED_FLAG;
        }
        else
        {
            trust.flags &= ~AUTHORIZED_FLAG;
        }
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
generateBuyingLiabilities(LedgerEntry offer, bool excess, bool authorized)
{
    auto const& oe = offer.data.offer();

    LedgerEntry le;
    le.lastModifiedLedgerSeq = 2;

    if (oe.buying.type() == ASSET_TYPE_NATIVE)
    {
        auto account = LedgerTestUtils::generateValidAccountEntry();
        account.accountID = oe.sellerID;
        auto maxBalance = INT64_MAX - oe.amount;
        account.balance = excess ? std::max(account.balance, maxBalance + 1)
                                 : std::min(account.balance, maxBalance);

        account.ext.v(1);
        account.ext.v1().liabilities = Liabilities{oe.amount, 0};

        le.data.type(ACCOUNT);
        le.data.account() = account;
    }
    else
    {
        auto trust = LedgerTestUtils::generateValidTrustLineEntry();
        trust.accountID = oe.sellerID;
        if (authorized)
        {
            trust.flags |= AUTHORIZED_FLAG;
        }
        else
        {
            trust.flags &= ~AUTHORIZED_FLAG;
        }
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

TEST_CASE("Invariant for liabilities", "[invariant][liabilitiesmatchoffers]")
{
    std::default_random_engine gen;
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"MinimumAccountBalance"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    Asset native;

    Asset cur1;
    cur1.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur1.alphaNum4().assetCode, "CUR1");

    Asset cur2;
    cur2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur2.alphaNum4().assetCode, "CUR2");

    SECTION("create then modify then delete offer")
    {
        auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
        auto selling = generateSellingLiabilities(*app, offer, false, true);
        auto buying = generateBuyingLiabilities(offer, false, true);
        std::vector<LedgerEntry> entries{offer, selling, buying};
        auto updates = makeUpdateList(entries, nullptr);
        REQUIRE(store(*app, updates));

        auto offer2 = generateOffer(cur1, cur2, 200, Price{1, 1});
        offer2.data.offer().sellerID = offer.data.offer().sellerID;
        offer2.data.offer().offerID = offer.data.offer().offerID;
        auto selling2 = generateSellingLiabilities(*app, offer2, false, true);
        auto buying2 = generateBuyingLiabilities(offer2, false, true);
        std::vector<LedgerEntry> entries2{offer2, selling2, buying2};
        auto updates2 = makeUpdateList(entries2, entries);
        REQUIRE(store(*app, updates2));

        auto updates3 = makeUpdateList(nullptr, entries2);
        REQUIRE(store(*app, updates3));
    }

    SECTION("create offer with excess liabilities")
    {
        auto verify = [&](bool excessSelling, bool authorizedSelling,
                          bool excessBuying, bool authorizedBuying) {
            auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
            auto selling = generateSellingLiabilities(
                *app, offer, excessSelling, authorizedSelling);
            auto buying = generateBuyingLiabilities(offer, excessBuying,
                                                    authorizedBuying);
            std::vector<LedgerEntry> entries{offer, selling, buying};
            auto updates = makeUpdateList(entries, nullptr);
            REQUIRE(!store(*app, updates));
        };

        SECTION("excess selling")
        {
            verify(true, true, false, true);
        }
        SECTION("unauthorized selling")
        {
            verify(false, false, false, true);
        }
        SECTION("excess buying")
        {
            verify(false, true, true, true);
        }
        SECTION("unauthorized buying")
        {
            verify(false, true, false, false);
        }
    }

    SECTION("modify offer to have excess liabilities")
    {
        auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
        auto selling = generateSellingLiabilities(*app, offer, false, true);
        auto buying = generateBuyingLiabilities(offer, false, true);
        std::vector<LedgerEntry> entries{offer, selling, buying};
        auto updates = makeUpdateList(entries, nullptr);
        REQUIRE(store(*app, updates));

        auto verify = [&](bool excessSelling, bool authorizedSelling,
                          bool excessBuying, bool authorizedBuying) {
            auto offer2 = generateOffer(cur1, cur2, 200, Price{1, 1});
            offer2.data.offer().sellerID = offer.data.offer().sellerID;
            offer2.data.offer().offerID = offer.data.offer().offerID;
            auto selling2 = generateSellingLiabilities(
                *app, offer, excessSelling, authorizedSelling);
            auto buying2 = generateBuyingLiabilities(offer, excessBuying,
                                                     authorizedBuying);
            std::vector<LedgerEntry> entries2{offer2, selling2, buying2};
            auto updates2 = makeUpdateList(entries2, entries);
            REQUIRE(!store(*app, updates2));
        };

        SECTION("excess selling")
        {
            verify(true, true, false, true);
        }
        SECTION("unauthorized selling")
        {
            verify(false, false, false, true);
        }
        SECTION("excess buying")
        {
            verify(false, true, true, true);
        }
        SECTION("unauthorized buying")
        {
            verify(false, true, false, false);
        }
    }

    SECTION("revoke authorization")
    {
        auto offer = generateOffer(cur1, cur2, 100, Price{1, 1});
        auto selling = generateSellingLiabilities(*app, offer, false, true);
        auto buying = generateBuyingLiabilities(offer, false, true);
        std::vector<LedgerEntry> entries{offer, selling, buying};
        auto updates = makeUpdateList(entries, nullptr);
        REQUIRE(store(*app, updates));

        SECTION("selling auth")
        {
            auto selling2 =
                generateSellingLiabilities(*app, offer, false, false);
            REQUIRE(!store(*app, makeUpdateList({selling2}, {selling})));
        }
        SECTION("buying auth")
        {
            auto buying2 = generateBuyingLiabilities(offer, false, false);
            REQUIRE(!store(*app, makeUpdateList({buying2}, {buying})));
        }
    }
}
