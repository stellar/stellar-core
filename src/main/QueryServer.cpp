// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/QueryServer.h"
#include "bucket/BucketListSnapshot.h"
#include "bucket/BucketSnapshotManager.h"
#include "ledger/LedgerTxnImpl.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Config.h"
#include "util/ArchivalProofs.h"
#include "util/Logging.h"
#include "util/UnorderedSet.h"
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
                         Config const& cfg
#endif
                         )
    : mServer(address, port, maxClient, threadPoolSize)
#ifdef BUILD_TESTS
    , mRequireProofsForAllEvictedEntries(
          cfg.REQUIRE_PROOFS_FOR_ALL_EVICTED_ENTRIES)
    , mSimulateFilterMiss(cfg.ARTIFICIALLY_SIMULATE_ARCHIVE_FILTER_MISS)
#endif
{
    LOG_INFO(DEFAULT_LOG, "Listening on {}:{} for Query requests", address,
             port);

    mServer.add404(std::bind(&QueryServer::notFound, this, _1, _2, _3));
    addRoute("getledgerentryraw", &QueryServer::getLedgerEntryRaw);
    addRoute("getledgerentry", &QueryServer::getLedgerEntry);
    addRoute("getrestoreproof", &QueryServer::getRestoreProof);
    addRoute("getcreationproof", &QueryServer::getCreationProof);

    auto workerPids = mServer.start();
    for (auto pid : workerPids)
    {
        mBucketListSnapshots[pid] =
            bucketSnapshotManager.copySearchableLiveBucketListSnapshot();
        mHotArchiveBucketListSnapshots[pid] =
            bucketSnapshotManager.copySearchableHotArchiveBucketListSnapshot();
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
        auto& bl = *mBucketListSnapshots.at(std::this_thread::get_id());

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

    // First query LiveBucketList for the entries and TTLs
    auto& bl = *mBucketListSnapshots.at(std::this_thread::get_id());
    LedgerKeySet orderedKeys;
    for (auto const& key : keys)
    {
        LedgerKey k;
        fromOpaqueBase64(k, key);
        if (k.type() == TTL)
        {
            throw std::invalid_argument(
                "Must not query TTL keys. For live BucketList key-value "
                "lookup use getledgerentryraw");
        }

        orderedKeys.emplace(k);
        if (isSorobanEntry(k))
        {
            orderedKeys.emplace(getTTLKey(k));
        }
    }

    std::vector<LedgerEntry> loadedLiveKeys;
    uint32_t ledgerSeq;

    // If a snapshot ledger is specified, use it to get the ledger entry
    if (snapshotLedger)
    {
        ledgerSeq = *snapshotLedger;

        auto loadedKeysOp = bl.loadKeysFromLedger(orderedKeys, *snapshotLedger);

        // Return 404 if ledgerSeq not found
        if (!loadedKeysOp)
        {
            retStr = "LedgerSeq not found";
            return false;
        }

        loadedLiveKeys = std::move(*loadedKeysOp);
    }
    // Otherwise default to current ledger and use stable ledgerSeq for
    // later calls
    else
    {
        loadedLiveKeys =
            bl.loadKeysWithLimits(orderedKeys, /*lkMeter=*/nullptr);
        ledgerSeq = bl.getLedgerSeq();
    }

    root["ledgerSeq"] = ledgerSeq;

    UnorderedMap<LedgerKey, LedgerEntry const&> ttlEntries;
    UnorderedMap<LedgerKey, LedgerEntry const&> liveEntries;
    UnorderedSet<LedgerKey> deadOrArchivedEntries;
    for (auto const& le : loadedLiveKeys)
    {
        if (le.data.type() == TTL)
        {
            ttlEntries.emplace(LedgerEntryKey(le), le);
        }
        else
        {
            liveEntries.emplace(LedgerEntryKey(le), le);
        }
    }

    for (auto const& key : orderedKeys)
    {
        if (key.type() != TTL && liveEntries.find(key) == liveEntries.end())
        {
            deadOrArchivedEntries.emplace(key);
        }
    }

    // First process entries from the LiveBucketList
    for (auto const& [lk, le] : liveEntries)
    {
        Json::Value entry;
        entry["e"] = toOpaqueBase64(le);
        if (!isSorobanEntry(le.data))
        {
            entry["state"] = "live";
        }
        else
        {
            auto const& ttl = ttlEntries.at(getTTLKey(lk));
            if (isLive(ttl, ledgerSeq))
            {
                entry["state"] = "live";
            }
            // Dead entry, temp never require a proof
            else if (isTemporaryEntry(le.data))
            {
                entry["state"] = "new_entry_no_proof";
            }
            // Archived but not yet evicted entries do not require proofs
            else
            {
                entry["state"] = "archived_no_proof";
            }
        }

        root["entries"].append(entry);
    }

    // Next process all keys not found in live BucketList
    LedgerKeySet archivedOrNewSorobanKeys;
    for (auto const& key : deadOrArchivedEntries)
    {

        // Classic and temp never require proofs
        if (isSorobanEntry(key) && !isTemporaryEntry(key))
        {
            archivedOrNewSorobanKeys.emplace(key);
        }
        else
        {
            Json::Value entry;
            entry["e"] = toOpaqueBase64(key);
            entry["state"] = "new_entry_no_proof";
            root["entries"].append(entry);
        }
    }

    // Search Hot Archive for remaining persistent keys
    auto& hotBL = mHotArchiveBucketListSnapshots.at(std::this_thread::get_id());
    auto loadedHotArchiveEntriesOp =
        hotBL->loadKeysFromLedger(archivedOrNewSorobanKeys, ledgerSeq);

    if (!loadedHotArchiveEntriesOp)
    {
        retStr = "LedgerSeq not found";
        return false;
    }

    // Process entries currently marked as archived in the Hot Archive
    for (auto const& be : *loadedHotArchiveEntriesOp)
    {
        if (be.type() == HOT_ARCHIVE_ARCHIVED)
        {
            auto const& le = be.archivedEntry();
            Json::Value entry;
            entry["e"] = toOpaqueBase64(le);

            if (mRequireProofsForAllEvictedEntries)
            {
                entry["state"] = "archived_proof";
            }
            else
            {

                entry["state"] = "archived_no_proof";
            }
            root["entries"].append(entry);
            archivedOrNewSorobanKeys.erase(LedgerEntryKey(le));
        }
    }

    // At this point all entries remaining in archivedOrNewSorobanKeys are
    // persistent entries that do not exist
    for (auto const& key : archivedOrNewSorobanKeys)
    {
        Json::Value entry;
        entry["e"] = toOpaqueBase64(key);

#ifdef BUILD_TESTS
        if (mSimulateFilterMiss)
        {
            if (key.type() == CONTRACT_DATA &&
                key.contractData().key.type() == SCV_SYMBOL &&
                key.contractData().key.sym() == "miss")
            {
                entry["state"] = "new_entry_proof";
            }
            else
            {
                entry["state"] = "new_entry_no_proof";
            }
        }
        else
#endif
            entry["state"] = "new_entry_no_proof";

        root["entries"].append(entry);
    }

    retStr = Json::FastWriter().write(root);
    return true;
}

bool
QueryServer::getRestoreProof(std::string const& params, std::string const& body,
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

    xdr::xvector<ArchivalProof> proof;
    auto& hotBL = mHotArchiveBucketListSnapshots.at(std::this_thread::get_id());
    for (auto const& key : keys)
    {
        LedgerKey lk;
        fromOpaqueBase64(lk, key);
        if (!isPersistentEntry(lk))
        {
            throw std::invalid_argument(
                "Only persistent entries require restoration proofs");
        }

        if (!addRestorationProof(hotBL, lk, proof, snapshotLedger))
        {
            throw std::invalid_argument("No valid proof exists for key");
        }
    }

    root["ledger"] = hotBL->getLedgerSeq();
    root["proof"] = toOpaqueBase64(proof);

    retStr = Json::FastWriter().write(root);
    return true;
}

bool
QueryServer::getCreationProof(std::string const& params,
                              std::string const& body, std::string& retStr)
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

    auto& hotBL = mHotArchiveBucketListSnapshots.at(std::this_thread::get_id());
    xdr::xvector<ArchivalProof> proof;
    for (auto const& key : keys)
    {
        LedgerKey lk;
        fromOpaqueBase64(lk, key);
        if (!isPersistentEntry(lk) || lk.type() != CONTRACT_DATA)
        {
            throw std::invalid_argument("Only persistent contract data entries "
                                        "require creation proofs");
        }

        if (!addCreationProof(mSimulateFilterMiss, lk, proof))
        {
            throw std::invalid_argument("No valid proof exists for key");
        }
    }

    root["ledger"] = snapshotLedger ? *snapshotLedger : hotBL->getLedgerSeq();
    root["proof"] = toOpaqueBase64(proof);

    retStr = Json::FastWriter().write(root);
    return true;
}
}