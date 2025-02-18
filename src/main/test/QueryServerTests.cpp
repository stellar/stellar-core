// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "main/QueryServer.h"
#include "test/test.h"
#include "util/Math.h"
#include <json/json.h>

using namespace stellar;

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
// TODO: Better testing of errors, edge cases like an entry being in both live
// and archive bl, etc.
TEST_CASE("getledgerentry", "[queryserver]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.QUERY_SNAPSHOT_LEDGERS = 5;

    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);
    auto& lm = app->getLedgerManager();

    // Query Server is disabled by default in cfg. Instead of enabling it, we're
    // going to manage a versio here manually so we can directly call functions
    // and avoid sending network requests.
    auto qServer = std::make_unique<QueryServer>(
        "127.0.0.1", // Address
        0,           // port (0 = random)
        1,           // maxClient
        1,           // threadPoolSize
        app->getBucketManager().getBucketSnapshotManager(), true);

    std::unordered_map<LedgerKey, LedgerEntry> liveEntryMap;

    // Map code/data lk -> ttl value
    std::unordered_map<LedgerKey, uint32_t> liveTTLEntryMap;
    std::unordered_map<LedgerKey, LedgerEntry> archivedEntryMap;
    std::unordered_map<LedgerKey, LedgerEntry> evictedEntryMap;

    // Create some test entries
    for (auto i = 0; i < 15; ++i)
    {
        auto lcl = app->getLedgerManager().getLastClosedLedgerNum();
        auto liveEntries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_DATA, CONTRACT_CODE, ACCOUNT}, 5);

        std::vector<LedgerEntry> liveEntriesToInsert;
        for (auto const& le : liveEntries)
        {
            if (isSorobanEntry(le.data))
            {
                LedgerEntry ttl;
                ttl.data.type(TTL);
                ttl.data.ttl().keyHash = getTTLKey(le).ttl().keyHash;

                // Make half of the entries archived on the live BL
                if (rand_flip())
                {
                    ttl.data.ttl().liveUntilLedgerSeq = lcl + 100;
                }
                else
                {
                    ttl.data.ttl().liveUntilLedgerSeq = 0;
                }
                liveTTLEntryMap[LedgerEntryKey(le)] =
                    ttl.data.ttl().liveUntilLedgerSeq;
                liveEntriesToInsert.push_back(ttl);
            }

            liveEntriesToInsert.push_back(le);
            liveEntryMap[LedgerEntryKey(le)] = le;
        }

        auto archivedEntries =
            LedgerTestUtils::generateValidUniqueLedgerEntriesWithTypes(
                {CONTRACT_DATA, CONTRACT_CODE}, 5);
        for (auto const& le : archivedEntries)
        {
            archivedEntryMap[LedgerEntryKey(le)] = le;
        }

        lm.setNextLedgerEntryBatchForBucketTesting({}, liveEntriesToInsert, {});
        lm.setNextArchiveBatchForBucketTesting(archivedEntries, {}, {});
        closeLedger(*app);
    }

    // Lambda to build request body
    auto buildRequestBody =
        [](std::optional<uint32_t> ledgerSeq,
           std::vector<LedgerKey> const& keys) -> std::string {
        std::string body;
        if (ledgerSeq)
        {
            body = "ledgerSeq=" + std::to_string(*ledgerSeq);
        }

        for (auto const& key : keys)
        {
            body += (body.empty() ? "" : "&") + std::string("key=") +
                    toOpaqueBase64(key);
        }
        return body;
    };

    // Lambda to check entry details in response
    auto checkEntry = [](std::string const& retStr, LedgerEntry const& le,
                         std::optional<uint32_t> expectedTTL,
                         uint32_t ledgerSeq) -> bool {
        Json::Value root;
        Json::Reader reader;
        REQUIRE(reader.parse(retStr, root));
        REQUIRE(root.isMember("entries"));
        REQUIRE(root.isMember("ledgerSeq"));
        REQUIRE(root["ledgerSeq"].asUInt() == ledgerSeq);

        auto const& entries = root["entries"];
        for (auto const& entry : entries)
        {
            REQUIRE(entry.isMember("e"));
            REQUIRE(entry.isMember("state"));

            LedgerEntry responseLE;
            fromOpaqueBase64(responseLE, entry["e"].asString());
            if (responseLE == le)
            {
                std::string expectedState;
                if (!isSorobanEntry(le.data))
                {
                    expectedState = "live";
                }
                else
                {
                    if (expectedTTL)
                    {
                        if (ledgerSeq >= *expectedTTL)
                        {
                            expectedState = "archived";
                        }
                        else
                        {
                            expectedState = "live";
                        }
                    }
                    else
                    {
                        expectedState = "archived";
                    }
                }

                REQUIRE(entry["state"].asString() == expectedState);
                if (isSorobanEntry(le.data) && expectedState == "live")
                {
                    REQUIRE(entry.isMember("ttl"));
                    REQUIRE(entry["ttl"].asUInt() == *expectedTTL);
                }
                else
                {
                    REQUIRE(!entry.isMember("ttl"));
                }

                return true;
            }
        }
        return false;
    };

    // Lambda to check new entry response
    auto checkNewEntry = [](std::string const& retStr, LedgerKey const& key,
                            uint32_t ledgerSeq) -> bool {
        Json::Value root;
        Json::Reader reader;
        REQUIRE(reader.parse(retStr, root));
        REQUIRE(root.isMember("entries"));
        REQUIRE(root.isMember("ledgerSeq"));
        REQUIRE(root["ledgerSeq"].asUInt() == ledgerSeq);

        auto const& entries = root["entries"];
        for (auto const& entry : entries)
        {
            REQUIRE(entry.isMember("e"));
            REQUIRE(entry.isMember("state"));
            REQUIRE(entry["state"].asString() == "new");

            LedgerKey responseKey;
            fromOpaqueBase64(responseKey, entry["e"].asString());
            if (responseKey == key)
            {
                REQUIRE(!entry.isMember("ttl"));
                return true;
            }
        }
        return false;
    };

    UnorderedSet<LedgerKey> seenKeys;
    for (auto const& [lk, le] : liveEntryMap)
    {
        auto body = buildRequestBody(std::nullopt, {lk});
        std::string retStr;
        std::string empty;
        REQUIRE(qServer->getLedgerEntry(empty, body, retStr));

        auto ttlIter = liveTTLEntryMap.find(lk);
        std::optional<uint32_t> expectedTTL =
            ttlIter != liveTTLEntryMap.end()
                ? std::make_optional(ttlIter->second)
                : std::nullopt;
        REQUIRE(
            checkEntry(retStr, le, expectedTTL, lm.getLastClosedLedgerNum()));

        // Remove any duplicates we've already found
        archivedEntryMap.erase(lk);
        seenKeys.insert(lk);
    }

    for (auto const& [lk, le] : archivedEntryMap)
    {
        auto body = buildRequestBody(std::nullopt, {lk});
        std::string retStr;
        std::string empty;
        REQUIRE(qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(
            checkEntry(retStr, le, std::nullopt, lm.getLastClosedLedgerNum()));
        seenKeys.insert(lk);
    }

    // Now check for new entries
    auto newKeys = LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
        {TRUSTLINE, CONTRACT_DATA, CONTRACT_CODE}, 5, seenKeys);
    for (auto const& key : newKeys)
    {
        auto body = buildRequestBody(std::nullopt, {key});
        std::string retStr;
        std::string empty;
        REQUIRE(qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(checkNewEntry(retStr, key, lm.getLastClosedLedgerNum()));
    }
}
#endif