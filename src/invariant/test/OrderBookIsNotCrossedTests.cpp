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

#define SHOULD_THROW false
#define SHOULD_PASS true

// XNOR's truth table is as follows:
// | a | b | a XNOR b |
// |---|---|----------|
// | 0 | 0 |     1    |
// | 0 | 1 |     0    |
// | 1 | 0 |     0    |
// | 1 | 1 |     1    |
bool
XNOR(bool a, bool b)
{
    return (a && b) || (!a && !b);
}

void
genApplyCheckCreateOffer(Asset& ask, Asset& bid, int64 amount, Price price,
                         Application& app, bool shouldPass)
{
    auto offer = generateOffer(ask, bid, amount, price);
    auto op = txtest::manageOffer(offer.data.offer().offerID, ask, bid, price,
                                  amount);
    std::vector<LedgerEntry> current{offer};
    auto updates = makeUpdateList(current, nullptr);
    REQUIRE(XNOR(store(app, updates, nullptr, nullptr, &op), shouldPass));
}

void
genApplyCheckModifyOffer(Asset& ask, Asset& bid, int64 amount, Price price,
                         Application& app, int64 offerId, AccountID sellerId,
                         std::vector<LedgerEntry> previous, Operation& op,
                         bool shouldPass)
{
    auto offer = generateOffer(ask, bid, amount, price);
    offer.data.offer().sellerID = sellerId;
    offer.data.offer().offerID = offerId;
    std::vector<LedgerEntry> current{offer};
    auto updates = makeUpdateList(current, previous);
    REQUIRE(XNOR(store(app, updates, nullptr, nullptr, &op), shouldPass));
}

void
genApplyCheckDeleteOffer(Application& app, std::vector<LedgerEntry> previous,
                         Operation& op, bool shouldPass)
{
    op.body.manageSellOfferOp().amount = 0;
    auto updates = makeUpdateList(nullptr, previous);
    REQUIRE(XNOR(store(app, updates, nullptr, nullptr, &op), shouldPass));
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
    Application::pointer app = createTestApplication(clock, cfg);

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
    genApplyCheckCreateOffer(cur1, cur2, 3, Price{3, 2}, *app, SHOULD_PASS);
    genApplyCheckCreateOffer(cur1, cur2, 2, Price{5, 3}, *app, SHOULD_PASS);
    genApplyCheckCreateOffer(cur2, cur1, 4, Price{3, 4}, *app, SHOULD_PASS);
    genApplyCheckCreateOffer(cur2, cur1, 1, Price{4, 5}, *app, SHOULD_PASS);

    SECTION("Not crossed when highest bid < lowest ask")
    {
        SECTION("Create offer")
        {
            // Offer 5: 7 A @  8/5 (B/A)
            auto offer5 = generateOffer(cur1, cur2, 7, Price{8, 5});
            auto op5 = txtest::manageOffer(offer5.data.offer().offerID, cur1,
                                           cur2, Price{8, 5}, 7);
            std::vector<LedgerEntry> current5{offer5};
            auto updates5 = makeUpdateList(current5, nullptr);
            REQUIRE(store(*app, updates5, nullptr, nullptr, &op5));

            SECTION("Then modify offer")
            {
                // Offer 6: 7 A @  16/11 (B/A) - MODIFY
                genApplyCheckModifyOffer(cur1, cur2, 7, Price{16, 11}, *app,
                                         offer5.data.offer().offerID,
                                         offer5.data.offer().sellerID, current5,
                                         op5, SHOULD_PASS);
            }

            SECTION("Then delete offer")
            {
                // Offer 6: 0 A @  8/5 (B/A) - DELETE
                genApplyCheckDeleteOffer(*app, current5, op5, SHOULD_PASS);
            }
        }
    }
    SECTION("Crossed where highest bid = lowest ask")
    {
        SECTION("Create offer")
        {
            // Offer 5: 7 A @  4/3 (B/A)
            auto offer5 = generateOffer(cur1, cur2, 7, Price{4, 3});
            auto op5 = txtest::manageOffer(offer5.data.offer().offerID, cur1,
                                           cur2, Price{4, 3}, 7);
            std::vector<LedgerEntry> current5{offer5};
            auto updates5 = makeUpdateList(current5, nullptr);
            REQUIRE(!store(*app, updates5, nullptr, nullptr, &op5));

            SECTION("Then modify offer")
            {
                // Offer 6: 3 A @ 4/3 (B/A) - MODIFY
                genApplyCheckModifyOffer(cur1, cur2, 3, Price{4, 3}, *app,
                                         offer5.data.offer().offerID,
                                         offer5.data.offer().sellerID, current5,
                                         op5, SHOULD_THROW);
            }

            SECTION("Then delete offer and create offer crossing other side of "
                    "order book")
            {
                // Offer 6: 0 A @  4/3 (B/A) - DELETE
                genApplyCheckDeleteOffer(*app, current5, op5, SHOULD_PASS);

                // Offer 7: 2 B @  3/5 (A/B)
                genApplyCheckCreateOffer(cur2, cur1, 3, Price{3, 5}, *app,
                                         SHOULD_THROW);
            }
        }
    }

    SECTION("Crossed where highest bid > lowest ask")
    {
        SECTION("Create offer")
        {
            // Offer 5: 7 A @  1/1 (B/A)
            auto offer5 = generateOffer(cur1, cur2, 7, Price{1, 1});
            auto op5 = txtest::manageOffer(offer5.data.offer().offerID, cur1,
                                           cur2, Price{1, 1}, 7);
            std::vector<LedgerEntry> current5{offer5};
            auto updates5 = makeUpdateList(current5, nullptr);
            REQUIRE(!store(*app, updates5, nullptr, nullptr, &op5));

            SECTION("Then modify offer")
            {
                // Offer 6: 3 A @ 100/76 (B/A) - MODIFY
                genApplyCheckModifyOffer(cur1, cur2, 3, Price{100, 76}, *app,
                                         offer5.data.offer().offerID,
                                         offer5.data.offer().sellerID, current5,
                                         op5, SHOULD_THROW);
            }

            SECTION("Then delete offer and create offer crossing other side of "
                    "order book")
            {
                // Offer 6: 0 A @  1/1 (B/A) - DELETE
                genApplyCheckDeleteOffer(*app, current5, op5, SHOULD_PASS);

                // Offer 7: 2 B @  61/100 (A/B)
                genApplyCheckCreateOffer(cur2, cur1, 2, Price{61, 100}, *app,
                                         SHOULD_THROW);
            }
        }
    }
}
