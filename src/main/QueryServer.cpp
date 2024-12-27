// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/QueryServer.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxnImpl.h"
#include "util/Logging.h"
#include "util/XDRStream.h" // IWYU pragma: keep
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
                         BucketSnapshotManager& bucketSnapshotManager)
    : mServer(address, port, maxClient, threadPoolSize)
    , mBucketSnapshotManager(bucketSnapshotManager)
{
    LOG_INFO(DEFAULT_LOG, "Listening on {}:{} for Query requests", address,
             port);

    mServer.add404(std::bind(&QueryServer::notFound, this, _1, _2, _3));
    addRoute("getledgerentryraw", &QueryServer::getLedgerEntryRaw);

    auto workerPids = mServer.start();
    for (auto pid : workerPids)
    {
        mBucketListSnapshots[pid] = std::move(
            bucketSnapshotManager.copySearchableLiveBucketListSnapshot());
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
}