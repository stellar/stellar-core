// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/TxAdverts.h"
#include "test/TestUtils.h"
#include "test/test.h"

namespace stellar
{
TEST_CASE("advert queue", "[flood][pullmode][acceptance]")
{
    VirtualClock clock;
    Config cfg = getTestConfig(0);
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 200;
    auto app = createTestApplication(clock, cfg);
    TxAdverts pullMode(*app);

    bool flushed = false;
    pullMode.start([&flushed](std::shared_ptr<StellarMessage const> msg) {
        flushed = true;
    });

    auto limit = app->getLedgerManager().getLastMaxTxSetSizeOps();
    auto getHash = [](auto i) { return sha256(std::to_string(i)); };

    SECTION("incoming adverts")
    {
        std::list<Hash> retry;

        // Check the trimming logic for incoming adverts.
        TxAdvertVector hashes;
        for (uint32_t i = 0; i < limit; i++)
        {
            hashes.push_back(getHash(i));
            retry.push_back(getHash(limit + i));
        }
        pullMode.queueIncomingAdvert(hashes, LedgerManager::GENESIS_LEDGER_SEQ);
        REQUIRE(pullMode.size() == limit);

        pullMode.retryIncomingAdvert(retry);
        REQUIRE(pullMode.size() == limit);

        for (uint32_t i = 0; i < limit; i++)
        {
            // Since the advert queue is "FIFO",
            // the retry hashes gets popped first.
            // Therefore, we should only have the new hashes.
            auto h = pullMode.popIncomingAdvert().first;
            REQUIRE(h == getHash(i));
        }
        REQUIRE(pullMode.size() == 0);
        hashes.clear();
        retry.clear();
        for (uint32_t i = 0; i < (limit / 2); i++)
        {
            hashes.push_back(getHash(i));
            retry.push_back(getHash(limit + i));
        }
        pullMode.queueIncomingAdvert(hashes, LedgerManager::GENESIS_LEDGER_SEQ);
        pullMode.retryIncomingAdvert(retry);
        REQUIRE(pullMode.size() == ((limit / 2) * 2));
        for (uint32_t i = 0; i < limit / 2; i++)
        {
            // We pop retry hashes first.
            auto h = pullMode.popIncomingAdvert().first;
            REQUIRE(h == getHash(limit + i));
        }
        for (uint32_t i = 0; i < limit / 2; i++)
        {
            // We pop new hashes next.
            auto h = pullMode.popIncomingAdvert().first;
            REQUIRE(h == getHash(i));
        }
        REQUIRE(pullMode.size() == 0);
    }
    SECTION("outgoing adverts")
    {
        // Check that the timer flushes the queue
        SECTION("flush advert after some time")
        {
            pullMode.queueOutgoingAdvert(getHash(0));
            REQUIRE(1 < TX_ADVERT_VECTOR_MAX_SIZE);
            REQUIRE(1 < pullMode.getMaxAdvertSize());

            REQUIRE(!flushed);
            REQUIRE(pullMode.outgoingSize() == 1);
            testutil::crankFor(clock, std::chrono::seconds(1));
            REQUIRE(pullMode.outgoingSize() == 0);
            REQUIRE(flushed);
        }
        SECTION("flush advert when at capacity")
        {
            auto maxAdvert = pullMode.getMaxAdvertSize();
            for (uint32_t i = 0; i < maxAdvert - 1; i++)
            {
                pullMode.queueOutgoingAdvert(getHash(i));
            }

            REQUIRE(!flushed);
            REQUIRE(pullMode.outgoingSize() == maxAdvert - 1);
            pullMode.queueOutgoingAdvert(getHash(maxAdvert));
            REQUIRE(pullMode.outgoingSize() == 0);

            // Move the clock forward to fire the callback
            testutil::crankFor(clock, std::chrono::seconds(1));
            REQUIRE(flushed);
        }
        SECTION("ensure outgoing queue is capped")
        {
            VirtualClock clock2;
            Config cfg2 = getTestConfig(1);
            // Set max tx set size to something really high
            cfg2.TESTING_UPGRADE_MAX_TX_SET_SIZE =
                TX_ADVERT_VECTOR_MAX_SIZE * 100;
            auto app2 = createTestApplication(clock2, cfg2);
            TxAdverts pullMode2(*app2);
            // getMaxAdvertSize takes the limit into account
            REQUIRE(pullMode2.getMaxAdvertSize() <= TX_ADVERT_VECTOR_MAX_SIZE);

            for (uint32_t i = 0; i < TX_ADVERT_VECTOR_MAX_SIZE; i++)
            {
                pullMode.queueOutgoingAdvert(getHash(i));
            }

            REQUIRE(pullMode.outgoingSize() == 0);
        }
    }
}
}
