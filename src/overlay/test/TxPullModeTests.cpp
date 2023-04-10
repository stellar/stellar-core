// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/TxPullMode.h"
#include "test/TestUtils.h"
#include "test/test.h"

namespace stellar
{
TEST_CASE("advert queue", "[flood][pullmode][acceptance]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig(0);
    auto app = createTestApplication(clock, cfg);
    std::weak_ptr<Peer> weak;
    TxPullMode advertQueue(*app, weak);
    auto limit = app->getLedgerManager().getLastMaxTxSetSizeOps();
    auto getHash = [](auto i) { return sha256(std::to_string(i)); };

    std::list<Hash> retry;

    // Check the trimming logic for incoming adverts.
    TxAdvertVector hashes;
    for (uint32_t i = 0; i < limit; i++)
    {
        hashes.push_back(getHash(i));
        retry.push_back(getHash(limit + i));
    }
    advertQueue.queueIncomingAdvert(hashes, LedgerManager::GENESIS_LEDGER_SEQ);
    REQUIRE(advertQueue.size() == limit);

    advertQueue.retryIncomingAdvert(retry);
    REQUIRE(advertQueue.size() == limit);

    for (uint32_t i = 0; i < limit; i++)
    {
        // Since the advert queue is "FIFO",
        // the retry hashes gets popped first.
        // Therefore, we should only have the new hashes.
        auto h = advertQueue.popIncomingAdvert().first;
        REQUIRE(h == getHash(i));
    }
    REQUIRE(advertQueue.size() == 0);
    hashes.clear();
    retry.clear();
    for (uint32_t i = 0; i < (limit / 2); i++)
    {
        hashes.push_back(getHash(i));
        retry.push_back(getHash(limit + i));
    }
    advertQueue.queueIncomingAdvert(hashes, LedgerManager::GENESIS_LEDGER_SEQ);
    advertQueue.retryIncomingAdvert(retry);
    REQUIRE(advertQueue.size() == ((limit / 2) * 2));
    for (uint32_t i = 0; i < limit / 2; i++)
    {
        // We pop retry hashes first.
        auto h = advertQueue.popIncomingAdvert().first;
        REQUIRE(h == getHash(limit + i));
    }
    for (uint32_t i = 0; i < limit / 2; i++)
    {
        // We pop new hashes next.
        auto h = advertQueue.popIncomingAdvert().first;
        REQUIRE(h == getHash(i));
    }
    REQUIRE(advertQueue.size() == 0);
}
}
