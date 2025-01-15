// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/QueryServer.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/Logging.h"
#include "util/XDRStream.h" // IWYU pragma: keep
#include "util/types.h"
#include <exception>
#include <json/json.h>

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
                retStr = "LedgerSeq not found";
                return false;
            }

            loadedKeys = std::move(*loadedKeysOp);
        }
        // Otherwise default to current ledger
        else
        {
            loadedKeys =
                bl.loadKeysWithLimits(orderedKeys, /*lkMeter=*/nullptr);
            root["ledgerSeq"] = bl.getLedgerSeq();
        }

        for (auto const& le : loadedKeys)
        {
            Json::Value entry;
            entry["le"] = toOpaqueBase64(le);
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
// (live, archived, evicted, new). This requires a loading entry and TTL from
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

    auto keys = paramMap["key"];
    auto snapshotLedger = parseOptionalParam<uint32_t>(paramMap, "ledgerSeq");

    if (keys.empty())
    {
        throw std::invalid_argument(
            "Must specify ledger key in POST body: key=<LedgerKey in base64 "
            "XDR format>");
    }

    // Get snapshots for both live and hot archive bucket lists
    auto& liveBl = mBucketListSnapshots.at(std::this_thread::get_id());
    auto& hotArchiveBl =
        mHotArchiveBucketListSnapshots.at(std::this_thread::get_id());

    // orderedNotFoundKeys is a set of keys we have not yet found (not in live
    // BucketList or in an archived state in the Hot Archive)
    LedgerKeySet orderedNotFoundKeys;
    for (auto const& key : keys)
    {
        LedgerKey k;
        fromOpaqueBase64(k, key);

        // Check for TTL keys which are not allowed
        if (k.type() == TTL)
        {
            retStr = "TTL keys are not allowed";
            return false;
        }

        orderedNotFoundKeys.emplace(k);
    }

    mBucketSnapshotManager.maybeCopyLiveAndHotArchiveSnapshots(liveBl,
                                                               hotArchiveBl);

    std::vector<LedgerEntry> liveEntries;
    std::vector<HotArchiveBucketEntry> archivedEntries;
    uint32_t ledgerSeq =
        snapshotLedger ? *snapshotLedger : liveBl->getLedgerSeq();
    root["ledgerSeq"] = ledgerSeq;

    auto liveEntriesOp =
        liveBl->loadKeysFromLedger(orderedNotFoundKeys, ledgerSeq);

    // Return 404 if ledgerSeq not found
    if (!liveEntriesOp)
    {
        retStr = "LedgerSeq not found";
        return false;
    }

    liveEntries = std::move(*liveEntriesOp);

    // Remove keys found in live bucketList
    for (auto const& le : liveEntries)
    {
        orderedNotFoundKeys.erase(LedgerEntryKey(le));
    }

    LedgerKeySet hotArchiveKeysToSearch;
    for (auto const& lk : orderedNotFoundKeys)
    {
        if (isSorobanEntry(lk))
        {
            hotArchiveKeysToSearch.emplace(lk);
        }
    }

    // Only query archive for remaining keys
    if (!hotArchiveKeysToSearch.empty())
    {
        auto archivedEntriesOp =
            hotArchiveBl->loadKeysFromLedger(hotArchiveKeysToSearch, ledgerSeq);
        if (!archivedEntriesOp)
        {
            retStr = "LedgerSeq not found";
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
        // We haven't updated the live snapshot so we will never not have the
        // requested ledgerSeq and return nullopt.
        ttlEntries =
            std::move(liveBl->loadKeysFromLedger(ttlKeys, ledgerSeq).value());
    }

    std::unordered_map<LedgerKey, LedgerEntry> ttlMap;
    for (auto const& ttlEntry : ttlEntries)
    {
        ttlMap.emplace(LedgerEntryKey(ttlEntry), ttlEntry);
    }

    // Process live entries
    for (auto const& le : liveEntries)
    {
        Json::Value entry;
        entry["e"] = toOpaqueBase64(le);

        // Check TTL for Soroban entries
        if (isSorobanEntry(le.data))
        {
            auto ttlIter = ttlMap.find(getTTLKey(le));
            releaseAssertOrThrow(ttlIter != ttlMap.end());
            if (isLive(ttlIter->second, ledgerSeq))
            {
                entry["state"] = "live";
                entry["ttl"] = ttlIter->second.data.ttl().liveUntilLedgerSeq;
            }
            else
            {
                entry["state"] = "archived";
            }
        }
        else
        {
            entry["state"] = "live";
        }

        root["entries"].append(entry);
    }

    // Process archived entries - all are evicted since they come from hot
    // archive
    for (auto const& be : archivedEntries)
    {
        // If we get to this point, we know the key is not in the live
        // BucketList, so if we get a DELETED or RESTORED entry, the entry is
        // new wrt ledger state.
        if (be.type() != HOT_ARCHIVE_ARCHIVED)
        {
            continue;
        }

        auto const& le = be.archivedEntry();

        // At this point we've "found" the key and know it's archived, so remove
        // it from our search set
        orderedNotFoundKeys.erase(LedgerEntryKey(le));

        Json::Value entry;
        entry["e"] = toOpaqueBase64(le);
        entry["state"] = "evicted";
        root["entries"].append(entry);
    }

    // Since we removed entries found in the live BucketList and archived
    // entries found in the Hot Archive, any remaining keys must be new.
    for (auto const& key : orderedNotFoundKeys)
    {
        Json::Value entry;
        entry["e"] = toOpaqueBase64(key);
        entry["state"] = "new";
        root["entries"].append(entry);
    }

    retStr = Json::FastWriter().write(root);
    return true;
}
}