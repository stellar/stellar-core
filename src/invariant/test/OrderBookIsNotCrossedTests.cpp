#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "test/TestUtils.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::InvariantTestUtils;

LedgerEntry
genApplyCheckCreateOffer(Asset const& ask, Asset const& bid, int64 amount,
                         Price price, Application& app, bool shouldPass)
{
    auto offer = generateOffer(ask, bid, amount, price);
    std::vector<LedgerEntry> current{offer};
    auto const& updates = makeUpdateList(current, nullptr);
    REQUIRE(store(app, updates, nullptr, nullptr) == shouldPass);

    return offer;
}

LedgerEntry
genApplyCheckModifyOffer(LedgerEntry offer, Price price, Application& app,
                         bool shouldPass)
{
    std::vector<LedgerEntry> previous{offer};
    offer.data.offer().price = price;
    std::vector<LedgerEntry> current{offer};
    auto const& updates = makeUpdateList(current, previous);
    REQUIRE(store(app, updates, nullptr, nullptr) == shouldPass);

    return offer;
}

void
applyCheckDeleteOffer(Application& app, LedgerEntry offer, bool shouldPass)
{
    std::vector<LedgerEntry> previous{offer};
    auto const& updates = makeUpdateList(nullptr, previous);
    REQUIRE(store(app, updates, nullptr, nullptr) == shouldPass);
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

    auto invariant = std::make_shared<OrderBookIsNotCrossed>();
    auto offer = generateOffer(cur1, cur2, 3, Price{3, 2});
    auto const& offerID = offer.data.offer().offerID;

    // create
    {
        LedgerTxn ltx{ltxroot};
        ltx.create(offer);

        invariant->checkOnOperationApply({}, OperationResult{}, ltx.getDelta());
        auto const& orders = invariant->getOrderBook().at(cur1).at(cur2);

        REQUIRE(orders.size() == 1);
        REQUIRE(orders.at(offerID).amount == 3);
        REQUIRE(orders.at(offerID).price.n == 3);
        REQUIRE(orders.at(offerID).price.d == 2);

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
        REQUIRE(orders.at(offerID).amount == 2);
        REQUIRE(orders.at(offerID).price.n == 5);
        REQUIRE(orders.at(offerID).price.d == 3);

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

    // the initial state for the order book is:
    //     3 A @  3/2 (B/A)
    //     2 A @  5/3 (B/A)
    //     4 B @  3/4 (A/B)
    //     1 B @  4/5 (A/B)
    // where A = cur1 and B = cur2
    genApplyCheckCreateOffer(cur1, cur2, 3, Price{3, 2}, *app, true);
    genApplyCheckCreateOffer(cur1, cur2, 2, Price{5, 3}, *app, true);
    genApplyCheckCreateOffer(cur2, cur1, 4, Price{3, 4}, *app, true);
    genApplyCheckCreateOffer(cur2, cur1, 1, Price{4, 5}, *app, true);

    SECTION("Not crossed when highest bid < lowest ask")
    {
        // Create - Offer 5: 7 A @  8/5 (B/A)
        auto offer5 =
            genApplyCheckCreateOffer(cur1, cur2, 7, Price{8, 5}, *app, true);

        // Modify - Offer 6: 7 A @  16/11 (B/A)
        auto offer6 =
            genApplyCheckModifyOffer(offer5, Price{16, 11}, *app, true);

        // Delete - Offer 6
        applyCheckDeleteOffer(*app, offer6, true);
    }

    SECTION("Crossed where highest bid = lowest ask")
    {
        // Create - Offer 5: 7 A @  4/3 (B/A)
        auto offer5 =
            genApplyCheckCreateOffer(cur1, cur2, 7, Price{4, 3}, *app, false);

        // Modify - Offer 6: 3 A @ 4/3 (B/A)
        auto offer6 =
            genApplyCheckModifyOffer(offer5, Price{5, 4}, *app, false);

        // Delete - Offer 6
        applyCheckDeleteOffer(*app, offer6, true);
    }

    SECTION("Crossed where highest bid > lowest ask")
    {
        // Create - Offer 5: 7 A @  1/1 (B/A)
        auto offer5 =
            genApplyCheckCreateOffer(cur1, cur2, 7, Price{4, 3}, *app, false);

        // Modify - Offer 6: 3 A @ 100/76 (B/A)
        auto offer6 =
            genApplyCheckModifyOffer(offer5, Price{100, 76}, *app, false);

        // Delete - Offer 6
        applyCheckDeleteOffer(*app, offer6, true);
    }
}
#endif // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION