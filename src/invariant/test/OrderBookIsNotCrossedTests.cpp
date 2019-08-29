#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::InvariantTestUtils;

// convert a single offer into an offer operation
Operation
opFromLedgerEntries(LedgerEntry const& le)
{
    auto const& offer = le.data.offer();
    return txtest::manageBuyOffer(offer.offerID, offer.selling, offer.buying,
                                  offer.price, offer.amount);
}

// convert a vector of offers into a path payment operation
Operation
opFromLedgerEntries(std::vector<LedgerEntry> const& les)
{
    // for given assets A, Bs, C
    //      send: A
    //      dest: C
    //      path: {Bs}
    // where Bs represents from 0 to 5 offers inclusive
    Asset const& send = les.front().data.offer().selling;
    Asset const& dest = les.back().data.offer().buying;
    std::vector<Asset> path;

    for (int i = 1; i < les.size(); i++)
    {
        path.emplace_back(les[i].data.offer().selling);
    }

    // public key and amounts are not important
    return txtest::pathPayment(PublicKey{}, send, 1, dest, 1, path);
}

void
applyCheck(Application& app, std::vector<LedgerEntry> const& current,
           std::vector<LedgerEntry> const& previous, bool shouldPass,
           Operation const& op)
{
    stellar::InvariantTestUtils::UpdateList updates;

    if (previous.empty())
    {
        updates = makeUpdateList(current, nullptr);
    }
    else if (current.empty())
    {
        updates = makeUpdateList(nullptr, previous);
    }
    else
    {
        updates = makeUpdateList(current, previous);
    }

    REQUIRE(store(app, updates, nullptr, nullptr, &op) == shouldPass);
}

LedgerEntry
createOffer(Asset const& ask, Asset const& bid, int64 amount,
            Price const& price)
{
    return generateOffer(ask, bid, amount, price);
}

LedgerEntry
modifyOffer(LedgerEntry offer, int64 amount, Price const& price)
{
    offer.data.offer().amount = amount;
    offer.data.offer().price = price;
    return offer;
}

TEST_CASE("OrderBookIsNotCrossed in-memory order book is consistent with "
          "application behaviour",
          "[invariant][OrderBookIsNotCrossed]")
{
    Asset cur1;
    cur1.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur1.alphaNum4().assetCode, "CUR1");

    Asset cur2;
    cur2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur2.alphaNum4().assetCode, "CUR2");

    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig(0));
    LedgerTxn ltxroot{app->getLedgerTxnRoot()};

    auto const& invariant = std::make_shared<OrderBookIsNotCrossed>();
    auto offer = generateOffer(cur1, cur2, 3, Price{3, 2});

    // create
    {
        LedgerTxn ltx{ltxroot};
        ltx.create(offer);

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());
        auto const& orders = invariant->getOrderBook().at(cur1).at(cur2);

        REQUIRE(orders.size() == 1);

        auto const& offerToCheck = *orders.cbegin();

        REQUIRE(offerToCheck.amount == 3);
        REQUIRE(offerToCheck.price.n == 3);
        REQUIRE(offerToCheck.price.d == 2);

        ltx.commit();
    }

    // modify
    {
        LedgerTxn ltx{ltxroot};

        offer.data.offer().amount = 2;
        offer.data.offer().price = Price{5, 3};
        auto entry = ltx.load(LedgerEntryKey(offer));
        entry.current() = offer;

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());
        auto const& orders = invariant->getOrderBook().at(cur1).at(cur2);

        REQUIRE(orders.size() == 1);

        auto const& offerToCheck = *orders.cbegin();

        REQUIRE(offerToCheck.amount == 2);
        REQUIRE(offerToCheck.price.n == 5);

        ltx.commit();
    }

    // delete
    {
        LedgerTxn ltx{ltxroot};

        auto entry = ltx.load(LedgerEntryKey(offer));
        entry.erase();

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());

        REQUIRE(invariant->getOrderBook().at(cur1).at(cur2).size() == 0);
    }
}

TEST_CASE("OrderBookIsNotCrossed properly throws if order book is crossed",
          "[invariant][OrderBookIsNotCrossed]")
{
    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"OrderBookIsNotCrossed"};

    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    Asset cur1;
    cur1.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur1.alphaNum4().assetCode, "CUR1");

    Asset cur2;
    cur2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur2.alphaNum4().assetCode, "CUR2");

    Asset cur3;
    cur3.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur3.alphaNum4().assetCode, "CUR3");

    // the initial set up for the order book follows where:
    // A = cur1, B = cur2, and C = cur3
    // A against B
    //     offer1: 3 A @  3/2 (B/A)
    //     offer2: 2 A @  5/3 (B/A)
    auto const& offer1 = createOffer(cur1, cur2, 3, Price{3, 2});
    auto const& offer2 = createOffer(cur1, cur2, 2, Price{5, 3});
    // B against A
    //     offer3: 4 B @  3/4 (A/B)
    //     offer4: 1 B @  4/5 (A/B)
    auto const& offer3 = createOffer(cur2, cur1, 4, Price{3, 4});
    auto const& offer4 = createOffer(cur2, cur1, 1, Price{4, 5});
    // B against C
    //     offer5: 3 B @  3/2 (C/B)
    //     offer6: 2 B @  5/3 (C/B)
    auto const& offer5 = createOffer(cur2, cur3, 3, Price{3, 2});
    auto const& offer6 = createOffer(cur2, cur3, 2, Price{5, 3});
    // C against B
    //     offer7: 4 C @  3/4 (B/C)
    //     offer8: 1 C @  4/5 (B/C)
    auto const& offer7 = createOffer(cur3, cur2, 4, Price{3, 4});
    auto const& offer8 = createOffer(cur3, cur2, 1, Price{4, 5});

    applyCheck(*app,
               {offer1, offer2, offer3, offer4, offer5, offer6, offer7, offer8},
               {}, true, opFromLedgerEntries(offer4));

    SECTION("Not crossed when highest bid < lowest ask")
    {
        // Create - Offer 9: 7 A @  8/5 (B/A)
        auto offer9 = createOffer(cur1, cur2, 7, Price{8, 5});
        applyCheck(*app, {offer9}, {}, true, opFromLedgerEntries(offer9));

        // Modify - Offer 10: 7 A @  16/11 (B/A)
        auto const& offer10 = modifyOffer(offer9, 7, Price{16, 11});
        applyCheck(*app, {offer10}, {offer9}, true,
                   opFromLedgerEntries(offer10));

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true, opFromLedgerEntries(offer10));
    }

    SECTION("Crossed where highest bid = lowest ask")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A)
        auto offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        applyCheck(*app, {offer9}, {}, false, opFromLedgerEntries(offer9));

        // Modify - Offer 10: 3 A @ 4/3 (B/A)
        auto const& offer10 = modifyOffer(offer9, 3, Price{4, 3});
        applyCheck(*app, {offer10}, {offer9}, false,
                   opFromLedgerEntries(offer10));

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true, opFromLedgerEntries(offer10));
    }

    SECTION("Crossed where highest bid > lowest ask")
    {
        // Create - Offer 9: 7 A @  1/1 (B/A)
        auto offer9 = createOffer(cur1, cur2, 7, Price{1, 1});
        applyCheck(*app, {offer9}, {}, false, opFromLedgerEntries(offer9));

        // Modify - Offer 6: 3 A @ 100/76 (B/A)
        auto const& offer10 = modifyOffer(offer9, 3, Price{100, 76});
        applyCheck(*app, {offer10}, {offer9}, false,
                   opFromLedgerEntries(offer10));

        // Delete - Offer 10
        applyCheck(*app, {}, {offer10}, true, opFromLedgerEntries(offer10));
    }

    SECTION("Multiple assets not crossed (PathPayment)")
    {
        // Create - Offer 9: 7 A @  8/5 (B/A)
        // Create - Offer 10: 7 B @  8/5 (C/B)
        auto offer9 = createOffer(cur1, cur2, 7, Price{8, 5});
        auto offer10 = createOffer(cur2, cur3, 7, Price{8, 5});
        applyCheck(*app, {offer9, offer10}, {}, true,
                   opFromLedgerEntries({offer9, offer10}));

        // Modify - Offer 11: 7 A @  16/11 (B/A)
        // Modify - Offer 12: 7 B @  16/11 (C/B)
        auto const& offer11 = modifyOffer(offer9, 7, Price{16, 11});
        auto const& offer12 = modifyOffer(offer10, 7, Price{16, 11});
        applyCheck(*app, {offer11, offer12}, {offer9, offer10}, true,
                   opFromLedgerEntries({offer11, offer12}));

        // Delete - Offer 11
        // Delete - Offer 12
        applyCheck(*app, {}, {offer11, offer12}, true,
                   opFromLedgerEntries({offer11, offer12}));
    }

    SECTION("Multiple assets crossed where only one crosses (PathPayment)")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A) - CROSSED
        // Create - Offer 10: 7 B @  8/5 (C/B)
        auto offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        auto offer10 = createOffer(cur2, cur3, 7, Price{8, 5});
        applyCheck(*app, {offer9, offer10}, {}, false,
                   opFromLedgerEntries({offer9, offer10}));

        // Modify - Offer 11: 3 A @  16/11 (B/A)
        // Modify - Offer 12: 3 B @  100/76 (C/B) - CROSSED
        auto const& offer11 = modifyOffer(offer9, 3, Price{16, 11});
        auto const& offer12 = modifyOffer(offer10, 3, Price{100, 76});
        applyCheck(*app, {offer11, offer12}, {offer9, offer10}, false,
                   opFromLedgerEntries({offer11, offer12}));
    }

    SECTION("Multiple assets crossed where both crossed (PathPayment)")
    {
        // Create - Offer 9: 7 A @  4/3 (B/A)
        // Create - Offer 10: 7 B @  4/3 (C/B)
        auto const& offer9 = createOffer(cur1, cur2, 7, Price{4, 3});
        auto const& offer10 = createOffer(cur2, cur3, 7, Price{4, 3});
        applyCheck(*app, {offer9, offer10}, {}, false,
                   opFromLedgerEntries({offer9, offer10}));
    }

    SECTION("Multiple assets not crossed when deleting offers with allow trust")
    {
        // revoke creator of offer3 and offer5's trustline to B deleting both of
        // these offers
        auto const& op = txtest::allowTrust(PublicKey{}, cur2, false);
        applyCheck(*app, {}, {offer3, offer5}, true, op);
    }
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION