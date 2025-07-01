// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "main/QueryServer.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/Math.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <json/json.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace stellar;

TEST_CASE("getledgerentry", "[queryserver]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.QUERY_SNAPSHOT_LEDGERS = 5;

    auto app = createTestApplication<BucketTestUtils::BucketTestApplication>(
        clock, cfg);
    auto& lm = app->getLedgerManager();

    // Query Server is disabled by default in cfg. Instead of enabling it, we're
    // going to manage a version here manually so we can directly call functions
    // and avoid sending network requests.
    auto qServer = std::make_unique<QueryServer>(
        "127.0.0.1", 0,
        1, // maxClient
        2, // threadPoolSize
        app->getBucketManager().getBucketSnapshotManager(), true);

    std::unordered_map<LedgerKey, LedgerEntry> liveEntryMap;

    // Map code/data lk -> ttl value
    std::unordered_map<LedgerKey, uint32_t> liveTTLEntryMap;
    std::unordered_map<LedgerKey, LedgerEntry> archivedEntryMap;

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
        for (auto& le : archivedEntries)
        {
            // Entries in hot archive can only be persistent
            if (!isPersistentEntry(le.data))
            {
                le.data.contractData().durability = PERSISTENT;
            }

            archivedEntryMap[LedgerEntryKey(le)] = le;
        }

        lm.setNextLedgerEntryBatchForBucketTesting(liveEntriesToInsert, {}, {});
        lm.setNextArchiveBatchForBucketTesting(archivedEntries, {});
        closeLedger(*app);
    }

    // Build HTTP request string body
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

    // Check response for entries that exist from returned JSON string
    auto checkEntry = [](auto const& entries, LedgerEntry const& le,
                         std::optional<uint32_t> expectedTTL,
                         uint32_t ledgerSeq) -> bool {
        for (auto const& entry : entries)
        {
            REQUIRE(entry.isMember("state"));
            if (entry["state"].asString() == "not-found")
            {
                continue;
            }

            REQUIRE(entry.isMember("entry"));

            LedgerEntry responseLE;
            fromOpaqueBase64(responseLE, entry["entry"].asString());
            if (responseLE == le)
            {
                std::string expectedState;
                // Classic entries are always live
                if (!isSorobanEntry(le.data))
                {
                    REQUIRE(!expectedTTL);
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

                // All soroban entries should have a TTL
                if (isSorobanEntry(le.data))
                {
                    REQUIRE(entry.isMember("liveUntilLedgerSeq"));
                    if (expectedState == "live")
                    {
                        REQUIRE(entry["liveUntilLedgerSeq"].asUInt() ==
                                *expectedTTL);
                    }
                    else if (expectedState == "archived")
                    {
                        // Archived Soroban entries should have TTL = 0
                        REQUIRE(entry["liveUntilLedgerSeq"].asUInt() == 0);
                    }
                }
                else
                {
                    // Classic entries should not have TTL
                    REQUIRE(!entry.isMember("liveUntilLedgerSeq"));
                }

                return true;
            }
        }
        return false;
    };

    // Check response for entries that should not exist. We return not-found
    // keys implicitly based on position, so just check structure correctness
    // and count here.
    auto checkNewEntryCount = [](auto const& entries,
                                 size_t expectedNewEntryCount) {
        size_t newEntryCount = 0;
        for (auto const& entry : entries)
        {
            REQUIRE(entry.isMember("state"));
            if (entry["state"].asString() != "not-found")
            {
                continue;
            }

            // For not-found entries, the 'entry' field should be omitted
            REQUIRE(!entry.isMember("entry"));
            REQUIRE(!entry.isMember("liveUntilLedgerSeq"));
            ++newEntryCount;
        }

        REQUIRE(newEntryCount == expectedNewEntryCount);
    };

    SECTION("lookup")
    {
        // Populate keysToSearch with new, live, and archived entries
        UnorderedSet<LedgerKey> generatedKeys;
        std::vector<LedgerKey> keysToSearch;
        for (auto const& [lk, le] : liveEntryMap)
        {
            generatedKeys.insert(lk);
            keysToSearch.push_back(lk);
        }
        for (auto const& [lk, le] : archivedEntryMap)
        {
            generatedKeys.insert(lk);
            keysToSearch.push_back(lk);
        }

        std::vector<LedgerKey> newKeys =
            LedgerTestUtils::generateValidUniqueLedgerKeysWithTypes(
                {CONTRACT_CODE, CONTRACT_DATA, ACCOUNT}, 15, generatedKeys);
        for (auto const& key : newKeys)
        {
            keysToSearch.push_back(key);
        }

        auto reqBody = buildRequestBody(std::nullopt, keysToSearch);
        std::string retStr;
        std::string empty;
        REQUIRE(qServer->getLedgerEntry(empty, reqBody, retStr));

        auto ledgerSeq = lm.getLastClosedLedgerNum();

        Json::Value root;
        Json::Reader reader;
        REQUIRE(reader.parse(retStr, root));
        REQUIRE(root.isMember("entries"));
        REQUIRE(root.isMember("ledgerSeq"));
        REQUIRE(root["ledgerSeq"].asUInt() == ledgerSeq);

        auto const& entries = root["entries"];

        for (auto const& [lk, le] : archivedEntryMap)
        {
            REQUIRE(liveEntryMap.find(lk) == liveEntryMap.end());
            REQUIRE(checkEntry(entries, le, std::nullopt, ledgerSeq));
        }

        auto expectedNewEntryCount = newKeys.size();
        for (auto const& [lk, le] : liveEntryMap)
        {
            std::optional<uint32_t> expectedTTL = std::nullopt;
            if (isSorobanEntry(lk))
            {
                auto ttlIter = liveTTLEntryMap.find(lk);
                REQUIRE(ttlIter != liveTTLEntryMap.end());
                expectedTTL = std::make_optional(ttlIter->second);

                // Expired temporary entries are considered "new"
                if (!isPersistentEntry(lk))
                {
                    if (ttlIter->second < ledgerSeq)
                    {
                        ++expectedNewEntryCount;
                        continue;
                    }
                }
            }

            REQUIRE(checkEntry(entries, le, expectedTTL, ledgerSeq));
        }

        checkNewEntryCount(entries, expectedNewEntryCount);
    }

    SECTION("snapshot lookup")
    {
        // Close a ledger with all possible state transitions to make sure
        // snapshots work correctly
        std::optional<LedgerEntry> entryToModify;
        std::optional<LedgerEntry> entryToDelete;
        std::optional<LedgerEntry> entryToRestore;
        std::optional<LedgerEntry> entryToArchive;
        LedgerEntry newEntry =
            LedgerTestUtils::generateValidLedgerEntryOfType(TRUSTLINE);

        for (auto const& [lk, le] : liveEntryMap)
        {
            if (le.data.type() == ACCOUNT)
            {
                if (!entryToModify)
                {
                    entryToModify = le;
                }
                else
                {
                    entryToDelete = le;
                }
            }
            else if (isSorobanEntry(le.data))
            {
                if (isPersistentEntry(le.data))
                {
                    if (liveTTLEntryMap[LedgerEntryKey(le)] >
                        lm.getLastClosedLedgerNum())
                    {
                        entryToArchive = le;
                    }
                    else
                    {
                        entryToRestore = le;
                    }
                }
            }
        }

        REQUIRE(entryToModify);
        REQUIRE(entryToDelete);
        REQUIRE(entryToRestore);
        REQUIRE(entryToArchive);

        auto oldLedger = lm.getLastClosedLedgerNum();

        LedgerEntry modifiedEntry = *entryToModify;
        modifiedEntry.lastModifiedLedgerSeq++;

        // Make a new TTL such that archived entry becomes live
        auto restoredTTL = oldLedger + 100;
        LedgerEntry entryToRestoreTTL;
        entryToRestoreTTL.data.type(TTL);
        entryToRestoreTTL.data.ttl().keyHash =
            getTTLKey(*entryToRestore).ttl().keyHash;
        entryToRestoreTTL.data.ttl().liveUntilLedgerSeq = restoredTTL;
        entryToRestoreTTL.lastModifiedLedgerSeq =
            modifiedEntry.lastModifiedLedgerSeq;

        // Make a new TTL such that live entry becomes archived
        LedgerEntry entryToArchiveTTL;
        entryToArchiveTTL.data.type(TTL);
        entryToArchiveTTL.data.ttl().keyHash =
            getTTLKey(*entryToArchive).ttl().keyHash;
        entryToArchiveTTL.data.ttl().liveUntilLedgerSeq = oldLedger - 1;
        entryToArchiveTTL.lastModifiedLedgerSeq =
            modifiedEntry.lastModifiedLedgerSeq;

        lm.setNextLedgerEntryBatchForBucketTesting(
            {newEntry}, {entryToArchiveTTL, entryToRestoreTTL, modifiedEntry},
            {LedgerEntryKey(*entryToDelete)});
        closeLedger(*app);

        auto newLedger = lm.getLastClosedLedgerNum();

        auto keysToSearch = {
            LedgerEntryKey(*entryToModify), LedgerEntryKey(*entryToDelete),
            LedgerEntryKey(*entryToRestore), LedgerEntryKey(*entryToArchive),
            LedgerEntryKey(newEntry)};

        SECTION("current values")
        {
            auto reqBody = buildRequestBody(newLedger, keysToSearch);
            std::string retStr;
            std::string empty;
            REQUIRE(qServer->getLedgerEntry(empty, reqBody, retStr));

            Json::Value root;
            Json::Reader reader;
            REQUIRE(reader.parse(retStr, root));
            REQUIRE(root.isMember("entries"));
            REQUIRE(root.isMember("ledgerSeq"));
            REQUIRE(root["ledgerSeq"].asUInt() == newLedger);

            auto const& entries = root["entries"];

            // Check that modified entry is modified
            REQUIRE(
                checkEntry(entries, modifiedEntry, std::nullopt, newLedger));

            // Check that deleted entry does not exist
            checkNewEntryCount(entries, 1);

            // Check that entry that was restored is live
            REQUIRE(
                checkEntry(entries, *entryToRestore, restoredTTL, newLedger));

            // Check that entry that was archived is archived
            REQUIRE(
                checkEntry(entries, *entryToArchive, std::nullopt, newLedger));

            // Check that newly created entry exists
            REQUIRE(checkEntry(entries, newEntry, std::nullopt, newLedger));
        }

        SECTION("snapshot values")
        {
            auto reqBody = buildRequestBody(oldLedger, keysToSearch);
            std::string retStr;
            std::string empty;
            REQUIRE(qServer->getLedgerEntry(empty, reqBody, retStr));

            Json::Value root;
            Json::Reader reader;
            REQUIRE(reader.parse(retStr, root));
            REQUIRE(root.isMember("entries"));
            REQUIRE(root.isMember("ledgerSeq"));
            REQUIRE(root["ledgerSeq"].asUInt() == oldLedger);

            auto const& entries = root["entries"];

            // Check modified entry original state
            REQUIRE(
                checkEntry(entries, *entryToModify, std::nullopt, oldLedger));

            // Check that deleted entry still exists
            REQUIRE(
                checkEntry(entries, *entryToDelete, std::nullopt, oldLedger));

            // Check that entry that was restored is archived
            REQUIRE(
                checkEntry(entries, *entryToRestore, std::nullopt, newLedger));

            // Check that entry that was archived is live
            REQUIRE(checkEntry(entries, *entryToArchive,
                               liveTTLEntryMap[LedgerEntryKey(*entryToArchive)],
                               newLedger));

            // Newly created entry should not exist
            checkNewEntryCount(entries, 1);
        }
    }

    SECTION("empty keys")
    {
        std::string retStr;
        std::string empty;
        auto body = "ledgerSeq=10"; // No keys provided
        REQUIRE(!qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(retStr ==
                "Must specify key in POST body: key=<LedgerKey in base64 "
                "XDR format>\n");
    }

    SECTION("TTL key")
    {
        LedgerKey ttlKey = LedgerEntryKey(
            LedgerTestUtils::generateValidLedgerEntryOfType(TTL));

        auto body = buildRequestBody(std::nullopt, {ttlKey});
        std::string retStr;
        std::string empty;
        REQUIRE(!qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(retStr == "TTL keys are not allowed\n");
    }

    SECTION("ledger not found")
    {
        auto liveEntry = liveEntryMap.begin()->first;
        auto currentLedger = lm.getLastClosedLedgerNum();
        auto body = buildRequestBody(currentLedger + 1000, {liveEntry});
        std::string retStr;
        std::string empty;
        REQUIRE(!qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(retStr == "Ledger not found\n");
    }

    SECTION("duplicate keys")
    {
        auto liveEntry = liveEntryMap.begin()->first;
        auto body = buildRequestBody(std::nullopt, {liveEntry, liveEntry});
        std::string retStr;
        std::string empty;
        REQUIRE(!qServer->getLedgerEntry(empty, body, retStr));
        REQUIRE(retStr == "Duplicate keys\n");
    }

    SECTION("response order matches request order")
    {
        UnorderedSet<LedgerKey> keySet;
        auto newKey =
            LedgerTestUtils::generateValidLedgerEntryKeysWithExclusions(
                {TTL, CONFIG_SETTING}, 1)
                .front();
        LedgerKey liveKey;
        for (auto const& [lk, le] : liveEntryMap)
        {
            if (isSorobanEntry(lk))
            {
                if (liveTTLEntryMap.at(lk) > lm.getLastClosedLedgerNum())
                {
                    liveKey = lk;
                    break;
                }
            }
        }

        auto testKeyOrder = [&](std::vector<LedgerKey> const& keyOrder,
                                bool liveFirst) {
            auto reqBody = buildRequestBody(std::nullopt, keyOrder);
            std::string retStr;
            std::string empty;
            REQUIRE(qServer->getLedgerEntry(empty, reqBody, retStr));

            Json::Value root;
            Json::Reader reader;
            REQUIRE(reader.parse(retStr, root));
            REQUIRE(root.isMember("entries"));

            auto entries = root["entries"];
            REQUIRE(entries.size() == keyOrder.size());

            auto checkLive = [&](auto entry) {
                REQUIRE(entry.isMember("state"));
                REQUIRE(entry["state"].asString() == "live");
                REQUIRE(entry.isMember("entry"));
            };

            auto checkNotFound = [&](auto entry) {
                REQUIRE(entry.isMember("state"));
                REQUIRE(entry["state"].asString() == "not-found");
                REQUIRE(!entry.isMember("entry"));
            };

            if (liveFirst)
            {
                checkLive(entries[0]);
                checkNotFound(entries[1]);
            }
            else
            {
                checkNotFound(entries[0]);
                checkLive(entries[1]);
            }
        };

        // Test both orderings
        testKeyOrder({liveKey, newKey}, true);
        testKeyOrder({newKey, liveKey}, false);
    }
}
