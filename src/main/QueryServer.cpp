// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/QueryServer.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDRStream.h" // IWYU pragma: keep
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <exception>
#include <json/json.h>
#include <unordered_map>

using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

namespace
{
template <typename T>
std::optional<T>
parseOptionalParam(std::map<std::string, std::vector<std::string>> const& map,
                   std::string const& key)
{
    auto i = map.find(key);
    if (i != map.end())
    {
        if (i->second.size() != 1)
        {
            std::string errorMsg = fmt::format(
                FMT_STRING("Expected exactly one '{}' argument"), key);
            throw std::runtime_error(errorMsg);
        }

        std::stringstream str(i->second.at(0));
        T val;
        str >> val;

        // Throw an error if not all bytes were loaded into `val`
        if (str.fail() || !str.eof())
        {
            std::string errorMsg =
                fmt::format(FMT_STRING("Failed to parse '{}' argument"), key);
            throw std::runtime_error(errorMsg);
        }
        return std::make_optional<T>(val);
    }

    return std::nullopt;
}
}

namespace stellar
{
QueryServer::QueryServer(const std::string& address, unsigned short port,
                         int maxClient, size_t threadPoolSize,
                         BucketSnapshotManager& bucketSnapshotManager
#ifdef BUILD_TESTS
                         ,
                         bool useMainThreadForTesting
#endif
                         )
    : mServer(address, port, maxClient, threadPoolSize)
    , mBucketSnapshotManager(bucketSnapshotManager)
{
    LOG_INFO(DEFAULT_LOG, "Listening on {}:{} for Query requests", address,
             port);

    mServer.add404(std::bind(&QueryServer::notFound, this, _1, _2, _3));
    addRoute("getledgerentryraw", &QueryServer::getLedgerEntryRaw);
    addRoute("getledgerentry", &QueryServer::getLedgerEntry);

#ifdef BUILD_TESTS
    if (useMainThreadForTesting)
    {
        mBucketListSnapshots[std::this_thread::get_id()] =
            bucketSnapshotManager.copySearchableLiveBucketListSnapshot();
        mHotArchiveBucketListSnapshots[std::this_thread::get_id()] =
            bucketSnapshotManager.copySearchableHotArchiveBucketListSnapshot();
    }
    else
#endif
    {
        auto workerPids = mServer.start();
        for (auto pid : workerPids)
        {
            mBucketListSnapshots[pid] =
                bucketSnapshotManager.copySearchableLiveBucketListSnapshot();
            mHotArchiveBucketListSnapshots[pid] =
                bucketSnapshotManager
                    .copySearchableHotArchiveBucketListSnapshot();
        }
    }
}

bool
QueryServer::notFound(std::string const& params, std::string const& body,
                      std::string& retStr)
{
    retStr = "<b>Welcome to stellar-core!</b><p>";
    retStr +=
        "Supported HTTP queries are listed in the <a href=\""
        "https://github.com/stellar/stellar-core/blob/master/docs/software/"
        "commands.md#http-commands"
        "\">docs</a> as well as in the man pages.</p>"
        "<p>Note that this port is for HTTP queries only, not commands.</p>"
        "<p>Have fun!</p>";

    // 404 never fails
    return true;
}

void
QueryServer::addRoute(std::string const& name, HandlerRoute route)
{
    mServer.addRoute(
        name, std::bind(&QueryServer::safeRouter, this, route, _1, _2, _3));
}

bool
QueryServer::safeRouter(HandlerRoute route, std::string const& params,
                        std::string const& body, std::string& retStr)
{
    try
    {
        ZoneNamedN(httpQueryZone, "HTTP query handler", true);
        return route(this, params, body, retStr);
    }
    catch (std::exception& e)
    {
        retStr = fmt::format("exception: {}", e.what());
    }
    catch (...)
    {
        retStr = R"({"exception": "generic"})";
    }

    // Return error
    return false;
}

bool
QueryServer::getLedgerEntryRaw(std::string const& params,
                               std::string const& body, std::string& retStr)
{
    ZoneScoped;
    Json::Value root;

    std::map<std::string, std::vector<std::string>> paramMap;
    httpThreaded::server::server::parsePostParams(body, paramMap);

    auto keys = paramMap["key"];
    auto snapshotLedger = parseOptionalParam<uint32_t>(paramMap, "ledgerSeq");

    if (!keys.empty())
    {
        auto& snapshotPtr = mBucketListSnapshots.at(std::this_thread::get_id());
        mBucketSnapshotManager.maybeCopySearchableBucketListSnapshot(
            snapshotPtr);

        auto& bl = *snapshotPtr;

        LedgerKeySet orderedKeys;
        for (auto const& key : keys)
        {
            LedgerKey k;
            fromOpaqueBase64(k, key);
            orderedKeys.emplace(k);
        }

        std::vector<LedgerEntry> loadedKeys;

        // If a snapshot ledger is specified, use it to get the ledger entry
        if (snapshotLedger)
        {
            root["ledgerSeq"] = *snapshotLedger;

            auto loadedKeysOp =
                bl.loadKeysFromLedger(orderedKeys, *snapshotLedger);

            // Return 404 if ledgerSeq not found
            if (!loadedKeysOp)
            {
                retStr = "Ledger not found\n";
                return false;
            }

            loadedKeys = std::move(*loadedKeysOp);
        }
        // Otherwise default to current ledger
        else
        {
            loadedKeys = bl.loadKeys(orderedKeys, "query");
            root["ledgerSeq"] = bl.getLedgerSeq();
        }

        for (auto const& le : loadedKeys)
        {
            Json::Value entry;
            entry["entry"] = toOpaqueBase64(le);
            root["entries"].append(entry);
        }
    }
    else
    {
        throw std::invalid_argument(
            "Must specify ledger key in POST body: key=<LedgerKey in base64 "
            "XDR format>");
    }
    retStr = Json::FastWriter().write(root);
    return true;
}

// This query needs to load all the given ledger entries and their "state"
// (live, archived, new). This requires loading an entry and TTL from
// the live BucketList and then checking the Hot Archive for any keys we didn't
// find. We do three passes:
// 1. Load all keys from the live BucketList
// 2. For any Soroban keys not in the live BucketList, load them from the Hot
//    Archive
// 3. Load TTL keys for any live Soroban entries found in 1.
bool
QueryServer::getLedgerEntry(std::string const& params, std::string const& body,
                            std::string& retStr)
{
    ZoneScoped;
    Json::Value root;

    std::map<std::string, std::vector<std::string>> paramMap;
    httpThreaded::server::server::parsePostParams(body, paramMap);

    auto const keys = paramMap["key"];
    auto snapshotLedger = parseOptionalParam<uint32_t>(paramMap, "ledgerSeq");

    if (keys.empty())
    {
        retStr = "Must specify key in POST body: key=<LedgerKey in base64 "
                 "XDR format>\n";
        return false;
    }

    auto& liveBl = mBucketListSnapshots.at(std::this_thread::get_id());
    auto& hotArchiveBl =
        mHotArchiveBucketListSnapshots.at(std::this_thread::get_id());

    LedgerKeySet keysToSearch;

    // Keep track of keys in their original order for response ordering
    std::vector<LedgerKey> inputOrderedKeys;

    for (auto const& key : keys)
    {
        LedgerKey k;
        fromOpaqueBase64(k, key);
        if (k.type() == TTL)
        {
            retStr = "TTL keys are not allowed\n";
            return false;
        }

        auto [_, inserted] = keysToSearch.emplace(k);
        if (!inserted)
        {
            retStr = "Duplicate keys\n";
            return false;
        }

        inputOrderedKeys.push_back(k);
    }

    mBucketSnapshotManager.maybeCopyLiveAndHotArchiveSnapshots(liveBl,
                                                               hotArchiveBl);

    std::vector<LedgerEntry> liveEntries;
    std::vector<HotArchiveBucketEntry> archivedEntries;
    uint32_t ledgerSeq =
        snapshotLedger ? *snapshotLedger : liveBl->getLedgerSeq();
    root["ledgerSeq"] = ledgerSeq;

    auto liveEntriesOp = liveBl->loadKeysFromLedger(keysToSearch, ledgerSeq);

    // Return 404 if ledgerSeq not found
    if (!liveEntriesOp)
    {
        retStr = "Ledger not found\n";
        return false;
    }

    liveEntries = std::move(*liveEntriesOp);
    liveEntriesOp->clear();

    // Remove keys found in live bucketList from subsequent searches
    for (auto const& le : liveEntries)
    {
        keysToSearch.erase(LedgerEntryKey(le));
    }

    LedgerKeySet hotArchiveKeysToSearch;
    for (auto const& lk : keysToSearch)
    {
        if (isSorobanEntry(lk))
        {
            hotArchiveKeysToSearch.emplace(lk);
        }
    }

    // Only query archive for soroban keys we didn't find in the live bucketList
    if (!hotArchiveKeysToSearch.empty())
    {
        auto archivedEntriesOp =
            hotArchiveBl->loadKeysFromLedger(hotArchiveKeysToSearch, ledgerSeq);
        if (!archivedEntriesOp)
        {
            retStr = "Ledger not found\n";
            return false;
        }
        archivedEntries = std::move(*archivedEntriesOp);
    }

    // Collect TTL keys for Soroban entries in the live BucketList
    LedgerKeySet ttlKeys;
    for (auto const& le : liveEntries)
    {
        if (isSorobanEntry(le.data))
        {
            ttlKeys.emplace(getTTLKey(le));
        }
    }

    std::vector<LedgerEntry> ttlEntries;
    if (!ttlKeys.empty())
    {
        // We haven't updated the live snapshot so we know the have a snapshot
        // available for ledgerSeq
        ttlEntries =
            std::move(liveBl->loadKeysFromLedger(ttlKeys, ledgerSeq).value());
    }

    std::unordered_map<LedgerKey, LedgerEntry> ttlMap;
    for (auto const& ttlEntry : ttlEntries)
    {
        ttlMap.emplace(LedgerEntryKey(ttlEntry), ttlEntry);
    }

    // Store key -> formatted response
    std::unordered_map<LedgerKey, Json::Value> responseEntries;

    for (auto const& le : liveEntries)
    {
        LedgerKey lk = LedgerEntryKey(le);
        Json::Value entry;

        // Check TTL to set state for Soroban entries
        if (isSorobanEntry(le.data))
        {
            auto ttlIter = ttlMap.find(getTTLKey(le));
            releaseAssertOrThrow(ttlIter != ttlMap.end());
            if (isLive(ttlIter->second, ledgerSeq))
            {
                entry["entry"] = toOpaqueBase64(le);
                entry["state"] = "live";
                entry["liveUntilLedgerSeq"] =
                    ttlIter->second.data.ttl().liveUntilLedgerSeq;
            }
            else if (isPersistentEntry(lk))
            {
                entry["entry"] = toOpaqueBase64(le);
                entry["state"] = "archived";
                entry["liveUntilLedgerSeq"] = 0;
            }
            // Archived temporary entries are considered "not-found"
            else
            {
                entry["state"] = "not-found";
            }
        }
        else
        {
            entry["entry"] = toOpaqueBase64(le);
            entry["state"] = "live";
        }

        responseEntries[lk] = entry;
    }

    for (auto const& be : archivedEntries)
    {
        auto const& le = be.archivedEntry();
        LedgerKey lk = LedgerEntryKey(le);

        // At this point we've "found" the key and know it's archived, so remove
        // it from our search set
        keysToSearch.erase(lk);

        Json::Value entry;
        entry["entry"] = toOpaqueBase64(le);
        entry["state"] = "archived";

        // Add placeholder TTL value for archived entries
        entry["liveUntilLedgerSeq"] = 0;

        responseEntries[lk] = entry;
    }

    // Since we removed entries found in the live BucketList and archived
    // entries found in the Hot Archive, any remaining keys must be not-found.
    for (auto const& key : keysToSearch)
    {
        Json::Value entry;
        entry["state"] = "not-found";

        responseEntries[key] = entry;
    }

    // Add entries to the response in the same order as the input keys
    for (auto const& key : inputOrderedKeys)
    {
        auto it = responseEntries.find(key);
        releaseAssertOrThrow(it != responseEntries.end());
        root["entries"].append(it->second);
    }

    retStr = Json::FastWriter().write(root);
    return true;
}
}