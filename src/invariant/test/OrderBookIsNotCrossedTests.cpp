#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "invariant/InvariantDoesNotHold.h"
#include "invariant/InvariantManager.h"
#include "invariant/OrderBookIsNotCrossed.h"
#include "invariant/test/InvariantTestUtils.h"
#include "ledger/LedgerTxn.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;
using namespace stellar::InvariantTestUtils;

AssetId
getAssetIDForAlphaNum4(Asset const& asset)
{
    size_t res = 0;
    auto const& tl4 = asset.alphaNum4();
    res ^= stellar::shortHash::computeHash(
        stellar::ByteSlice(tl4.issuer.ed25519().data(), 8));
    res ^= stellar::shortHash::computeHash(stellar::ByteSlice(tl4.assetCode));
    return res;
}

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
    const auto cur1AssetId = "CUR1-" + KeyUtils::toStrKey(getIssuer(cur1));

    Asset cur2;
    cur2.type(ASSET_TYPE_CREDIT_ALPHANUM4);
    strToAssetCode(cur2.alphaNum4().assetCode, "CUR2");
    const auto cur2AssetId = "CUR2-" + KeyUtils::toStrKey(getIssuer(cur2));

    Config cfg = getTestConfig(0);
    cfg.INVARIANT_CHECKS = {"OrderBookIsNotCrossed"};

    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    auto invariant = std::make_shared<OrderBookIsNotCrossed>();

    SECTION("Create offer")
    {
        Operation op{};
        OperationResult opRes{};
        auto offer1 = generateOffer(cur1, cur2, 3, Price{3, 2});

        auto ltxStore = std::make_unique<LedgerTxn>(app->getLedgerTxnRoot());
        auto ltxPtr = ltxStore.get();
        auto entry = ltxPtr->create(offer1);

        invariant->checkOnOperationApply(op, opRes, ltxPtr->getDelta());
        auto orderbook = invariant->getOrderBook();
        auto orders = orderbook[cur1AssetId][cur2AssetId];

        REQUIRE(orders.size() == 1);
        REQUIRE(orders.at(offer1.data.offer().offerID).amount == 3);
        REQUIRE(orders.at(offer1.data.offer().offerID).price.n == 3);
        REQUIRE(orders.at(offer1.data.offer().offerID).price.d == 2);

        ltxPtr->commit();

        SECTION("Then modify offer")
        {
            auto offer2 = generateOffer(cur1, cur2, 2, Price{5, 3});
            offer2.data.offer().sellerID = offer1.data.offer().sellerID;
            offer2.data.offer().offerID = offer1.data.offer().offerID;

            auto ltxStore =
                std::make_unique<LedgerTxn>(app->getLedgerTxnRoot());
            auto ltxPtr = ltxStore.get();
            auto entry = ltxPtr->load(LedgerEntryKey(offer1));
            entry.current() = offer2;

            invariant->checkOnOperationApply(op, opRes, ltxPtr->getDelta());
            auto orderbook = invariant->getOrderBook();
            auto orders = orderbook[cur1AssetId][cur2AssetId];

            REQUIRE(orders.size() == 1);
            REQUIRE(orders.at(offer2.data.offer().offerID).amount == 2);
            REQUIRE(orders.at(offer2.data.offer().offerID).price.n == 5);
            REQUIRE(orders.at(offer2.data.offer().offerID).price.d == 3);
        }

        SECTION("Then delete offer")
        {
            auto ltxStore =
                std::make_unique<LedgerTxn>(app->getLedgerTxnRoot());
            auto ltxPtr = ltxStore.get();
            auto entry = ltxPtr->load(LedgerEntryKey(offer1));
            entry.erase();

            invariant->checkOnOperationApply(op, opRes, ltxPtr->getDelta());
            auto orderbook = invariant->getOrderBook();
            auto orders = orderbook[cur1AssetId][cur2AssetId];

            REQUIRE(orders.size() == 0);
        }
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