#ifdef BUILD_TESTS
// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

namespace stellar
{

namespace
{
void
applyCheck(Application& app, std::vector<LedgerEntry> const& current,
           std::vector<LedgerEntry> const& previous, bool shouldPass)
{
    stellar::InvariantTestUtils::UpdateList updates;

    if (previous.empty())
    {
        updates = InvariantTestUtils::makeUpdateList(current, nullptr);
    }
    else if (current.empty())
    {
        updates = InvariantTestUtils::makeUpdateList(nullptr, previous);
    }
    else
    {
        std::vector<LedgerKey> curKeys;
        std::vector<LedgerKey> prevKeys;
        std::transform(current.begin(), current.end(),
                       std::back_inserter(curKeys), LedgerEntryKey);
        std::transform(previous.begin(), previous.end(),
                       std::back_inserter(prevKeys), LedgerEntryKey);
        REQUIRE(curKeys == prevKeys);
        updates = InvariantTestUtils::makeUpdateList(current, previous);
    }

    REQUIRE(InvariantTestUtils::store(app, updates, nullptr, nullptr) ==
            shouldPass);
}

LedgerEntry
createOffer(Asset const& ask, Asset const& bid, int64 amount,
            Price const& price, bool passive = false)
{
    auto offer = InvariantTestUtils::generateOffer(ask, bid, amount, price);
    offer.data.offer().flags = passive ? PASSIVE_FLAG : 0;
    return offer;
}

LedgerEntry
modifyOffer(LedgerEntry offer, bool passive)
{
    offer.data.offer().flags = passive ? PASSIVE_FLAG : 0;
    return offer;
}

LedgerEntry
modifyOffer(LedgerEntry offer, int64 amount, Price const& price)
{
    offer.data.offer().amount = amount;
    offer.data.offer().price = price;
    return offer;
}

LedgerEntry
modifyOffer(LedgerEntry offer, Asset const& ask, Asset const& bid, int64 amount,
            Price const& price)
{
    offer.data.offer().selling = ask;
    offer.data.offer().buying = bid;
    return modifyOffer(offer, amount, price);
}
}

TEST_CASE("Comparison of offers meets ordering requirements",
          "[invariant][OrderBookIsNotCrossed]")
{
    OrderBookIsNotCrossed::OfferEntryCmp cmp;
    OfferEntry l, r;

    l.price = Price{1, 1};
    l.flags = PASSIVE_FLAG;
    r.price = Price{2, 1};
    r.flags = 0;
    REQUIRE(cmp(l, r));
    REQUIRE(!cmp(r, l));

    l.price = Price{1, 1};
    l.flags = 0;
    l.offerID = 2;
    r.price = Price{1, 1};
    r.flags = PASSIVE_FLAG;
    r.offerID = 1;
    REQUIRE(cmp(l, r));
    REQUIRE(!cmp(r, l));
}

TEST_CASE("OrderBookIsNotCrossed in-memory order book is consistent with "
          "application behaviour",
          "[invariant][OrderBookIsNotCrossed]")
{
    VirtualClock clock;
    auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    // When testing the order book not crossed invariant, enable it and no other
    // invariants (these tests do things which violate other invariants).
    cfg.INVARIANT_CHECKS = {};
    auto app = createTestApplication(clock, cfg);
    OrderBookIsNotCrossed::registerAndEnableInvariant(*app);

    auto root = TestAccount::createRoot(*app);

    auto const cur1 = root.asset("CUR1");
    auto const cur2 = root.asset("CUR2");

    LedgerTxn ltxOuter{app->getLedgerTxnRoot()};

    auto const invariant = std::make_shared<OrderBookIsNotCrossed>();
    auto offer = InvariantTestUtils::generateOffer(cur1, cur2, 3, Price{3, 2});

    // create
    {
        LedgerTxn ltx{ltxOuter};
        ltx.create(offer);

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());
        auto const& orders = invariant->getOrderBook().at({cur1, cur2});

        REQUIRE(orders.size() == 1);

        auto const& offerToCheck = *orders.cbegin();

        REQUIRE(offerToCheck.amount == 3);
        REQUIRE(offerToCheck.price.n == 3);
        REQUIRE(offerToCheck.price.d == 2);

        ltx.commit();
    }

    // modify
    {
        LedgerTxn ltx{ltxOuter};

        offer.data.offer().amount = 2;
        offer.data.offer().price = Price{5, 3};
        auto entry = ltx.load(LedgerEntryKey(offer));
        entry.current() = offer;

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());
        auto const& orders = invariant->getOrderBook().at({cur1, cur2});

        REQUIRE(orders.size() == 1);

        auto const& offerToCheck = *orders.cbegin();

        REQUIRE(offerToCheck.amount == 2);
        REQUIRE(offerToCheck.price.n == 5);

        ltx.commit();
    }

    // delete
    {
        LedgerTxn ltx{ltxOuter};

        auto entry = ltx.load(LedgerEntryKey(offer));
        entry.erase();

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());

        REQUIRE(invariant->getOrderBook().at({cur1, cur2}).size() == 0);
    }
}

TEST_CASE("OrderBookIsNotCrossed properly throws if order book is crossed",
          "[invariant][OrderBookIsNotCrossed]")
{

    VirtualClock clock;
    auto cfg = getTestConfig(0, Config::TESTDB_IN_MEMORY);
    // When testing the order book not crossed invariant, enable it and no other
    // invariants (these tests do things which violate other invariants).
    cfg.INVARIANT_CHECKS = {};
    auto app = createTestApplication(clock, cfg);
    OrderBookIsNotCrossed::registerAndEnableInvariant(*app);

    auto root = TestAccount::createRoot(*app);

    auto const cur1 = root.asset("CUR1");
    auto const cur2 = root.asset("CUR2");
    auto const cur3 = root.asset("CUR3");
    auto const cur4 = root.asset("CUR4");
    auto const cur5 = root.asset("CUR5");

    // the initial set up for the order book follows where:
    // A = cur1, B = cur2, and C = cur3
    // A against B
    //     offer1: 3 A @  3/2 (B/A)
    //     offer2: 2 A @  5/3 (B/A)
    auto const offer1 = createOffer(cur1, cur2, 3, Price{3, 2});
    auto const offer2 = createOffer(cur1, cur2, 2, Price{5, 3});
    // B against A
    //     offer3: 4 B @  3/4 (A/B)
    //     offer4: 1 B @  4/5 (A/B)
    auto const offer3 = createOffer(cur2, cur1, 4, Price{3, 4});
    auto const offer4 = createOffer(cur2, cur1, 1, Price{4, 5});
    // B against C
    //     offer5: 3 B @  3/2 (C/B)
    //     offer6: 2 B @  5/3 (C/B)
    auto const offer5 = createOffer(cur2, cur3, 3, Price{3, 2});
    auto const offer6 = createOffer(cur2, cur3, 2, Price{5, 3});
    // C against B
    //     offer7: 4 C @  3/4 (B/C)
    //     offer8: 1 C @  4/5 (B/C)
    auto const offer7 = createOffer(cur3, cur2, 4, Price{3, 4});
    auto const offer8 = createOffer(cur3, cur2, 1, Price{4, 5});

    applyCheck(*app,
               {offer1, offer2, offer3, offer4, offer5, offer6, offer7, offer8},
               {}, true);

    SECTION("Not crossed when highest bid < lowest ask")
    {
        // Create - Offer 9: 7 A @  8/5 (B/A)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{8, 5});
        applyCheck(*app, {offer9}, {}, true);

        // Modify - Offer 10: 7 A @  16/11 (B/A)
        auto const offer10 = modifyOffer(offer9, 7, Price{16, 11});
        applyCheck(*app, {offer10}, {offer9}, true);

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true);
    }

    SECTION("Change an asset without crossing book")
    {
        auto const offer9 = modifyOffer(offer7, cur1, cur2, 4, Price{13, 10});
        applyCheck(*app, {offer9}, {offer7}, false);
    }

    SECTION("Cross book by changing an asset")
    {
        auto const offer9 = modifyOffer(offer7, cur1, cur2, 4, Price{3, 4});
        applyCheck(*app, {offer9}, {offer7}, false);
    }

    SECTION("Swap assets without crossing book")
    {
        auto const offer9 = modifyOffer(offer7, cur2, cur3, 4, Price{4, 3});
        applyCheck(*app, {offer9}, {offer7}, true);
    }

    SECTION("Cross book by swapping assets")
    {
        auto const offer9 = modifyOffer(offer8, cur2, cur3, 1, Price{5, 4});
        applyCheck(*app, {offer9}, {offer8}, false);
    }

    SECTION("Crossed where highest bid = lowest ask")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        applyCheck(*app, {offer9}, {}, false);

        // Modify - Offer 10: 3 A @ 4/3 (B/A)
        auto const offer10 = modifyOffer(offer9, 3, Price{4, 3});
        applyCheck(*app, {offer10}, {offer9}, false);

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true);
    }

    SECTION("Not crossed where highest bid = lowest ask but only because of "
            "passive offer(s)")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A) (passive)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{4, 3}, true);
        applyCheck(*app, {offer9}, {}, true);

        // Modify - Offer 10: 3 A @ 4/3 (B/A)
        auto const offer10 = modifyOffer(offer9, 3, Price{4, 3});
        applyCheck(*app, {offer10}, {offer9}, true);

        SECTION("Cross book by making an offer non-passive")
        {
            auto const offer11 = modifyOffer(offer10, false);
            applyCheck(*app, {offer11}, {offer10}, false);
        }

        // Create - Offer 11: 7 A @  1/1 (B/A) (passive)
        // This offer should cross even though it's passive, because the price
        // ratio between the ask and the bid is not exactly 1.
        auto const offer11 = createOffer(cur1, cur2, 7, Price{1, 1}, true);
        applyCheck(*app, {offer11}, {}, false);

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true);
    }

    SECTION("Crossed where highest bid > lowest ask")
    {
        // Create - Offer 9: 7 A @  1/1 (B/A)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{1, 1});
        applyCheck(*app, {offer9}, {}, false);

        // Modify - Offer 6: 3 A @ 100/76 (B/A)
        auto const offer10 = modifyOffer(offer9, 3, Price{100, 76});
        applyCheck(*app, {offer10}, {offer9}, false);

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true);
    }

    SECTION("Multiple assets not crossed (PathPayment)")
    {
        // Create - Offer 9: 7 A @  8/5 (B/A)
        // Create - Offer 10: 7 B @  8/5 (C/B)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{8, 5});
        auto const offer10 = createOffer(cur2, cur3, 7, Price{8, 5});
        applyCheck(*app, {offer9, offer10}, {}, true);

        // Modify - Offer 11: 7 A @  16/11 (B/A)
        // Modify - Offer 12: 7 B @  16/11 (C/B)
        auto const offer11 = modifyOffer(offer9, 7, Price{16, 11});
        auto const offer12 = modifyOffer(offer10, 7, Price{16, 11});
        applyCheck(*app, {offer11, offer12}, {offer9, offer10}, true);

        // Delete - Offer 11
        // Delete - Offer 12
        applyCheck(*app, {}, {offer11, offer12}, true);
    }

    SECTION("Multiple assets crossed where only one crosses (PathPayment)")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A) - CROSSED
        // Create - Offer 10: 7 B @  8/5 (C/B)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        auto const offer10 = createOffer(cur2, cur3, 7, Price{8, 5});
        applyCheck(*app, {offer9, offer10}, {}, false);

        // Modify - Offer 11: 3 A @  16/11 (B/A)
        // Modify - Offer 12: 3 B @  100/76 (C/B) - CROSSED
        auto const offer11 = modifyOffer(offer9, 3, Price{16, 11});
        auto const offer12 = modifyOffer(offer10, 3, Price{100, 76});
        applyCheck(*app, {offer11, offer12}, {offer9, offer10}, false);
    }

    SECTION("Multiple assets crossed where both crossed (PathPayment)")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A)
        // Create - Offer 10: 7 B @  4/3 (C/B)
        auto const offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        auto const offer10 = createOffer(cur2, cur3, 7, Price{4, 3});
        applyCheck(*app, {offer9, offer10}, {}, false);
    }

    SECTION("Multiple assets not crossed when deleting offers with allow trust")
    {
        // Delete both of offer3 and offer5, as if, for example, their creator's
        // trustline to B had been revoked.
        applyCheck(*app, {}, {offer3, offer5}, true);
    }
}
}
#endif // BUILD_TESTS
