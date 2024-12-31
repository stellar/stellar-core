// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandHandler.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshotManager.h"
#include "crypto/KeyUtils.h"
#include "herder/Herder.h"
#include "history/HistoryArchiveManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/NetworkConfig.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/Maintainer.h"
#include "main/QueryServer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/SurveyManager.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/reporting/json_reporter.h"
#include "util/Decoder.h"
#include "util/XDRCereal.h"
#include "util/XDRStream.h" // IWYU pragma: keep
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-transaction.h"
#include "xdrpp/marshal.h"

#ifdef BUILD_TESTS
#include "simulation/LoadGenerator.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#endif
#include <optional>

using std::placeholders::_1;
using std::placeholders::_2;

namespace stellar
{
CommandHandler::CommandHandler(Application& app) : mApp(app)
{
    if (mApp.getConfig().HTTP_PORT)
    {
        std::string ipStr;
        if (mApp.getConfig().PUBLIC_HTTP_PORT)
        {
            ipStr = "0.0.0.0";
        }
        else
        {
            ipStr = "127.0.0.1";
        }
        LOG_INFO(DEFAULT_LOG, "Listening on {}:{} for HTTP requests", ipStr,
                 mApp.getConfig().HTTP_PORT);

        int httpMaxClient = mApp.getConfig().HTTP_MAX_CLIENT;

        mServer = std::make_unique<http::server::server>(
            app.getClock().getIOContext(), ipStr, mApp.getConfig().HTTP_PORT,
            httpMaxClient);

        if (mApp.getConfig().HTTP_QUERY_PORT)
        {
            mQueryServer = std::make_unique<QueryServer>(
                ipStr, mApp.getConfig().HTTP_QUERY_PORT, httpMaxClient,
                mApp.getConfig().QUERY_THREAD_POOL_SIZE,
                mApp.getBucketManager().getBucketSnapshotManager());
        }
    }
    else
    {
        mServer = std::make_unique<http::server::server>(
            app.getClock().getIOContext());
    }

    mServer->add404(std::bind(&CommandHandler::fileNotFound, this, _1, _2));
    if (mApp.getConfig().modeStoresAnyHistory())
    {
        addRoute("maintenance", &CommandHandler::maintenance);
    }

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        addRoute("bans", &CommandHandler::bans);
        addRoute("connect", &CommandHandler::connect);
        addRoute("droppeer", &CommandHandler::dropPeer);
        addRoute("peers", &CommandHandler::peers);
        addRoute("quorum", &CommandHandler::quorum);
        addRoute("scp", &CommandHandler::scpInfo);
        addRoute("stopsurvey", &CommandHandler::stopSurvey);
#ifndef BUILD_TESTS
        addRoute("getsurveyresult", &CommandHandler::getSurveyResult);
        addRoute("surveytopology", &CommandHandler::surveyTopology);
        addRoute("startsurveycollecting",
                 &CommandHandler::startSurveyCollecting);
        addRoute("stopsurveycollecting", &CommandHandler::stopSurveyCollecting);
        addRoute("surveytopologytimesliced",
                 &CommandHandler::surveyTopologyTimeSliced);
#endif
        addRoute("unban", &CommandHandler::unban);
    }

    addRoute("clearmetrics", &CommandHandler::clearMetrics);
    addRoute("info", &CommandHandler::info);
    addRoute("ll", &CommandHandler::ll);
    addRoute("logrotate", &CommandHandler::logRotate);
    addRoute("manualclose", &CommandHandler::manualClose);
    addRoute("metrics", &CommandHandler::metrics);
    addRoute("tx", &CommandHandler::tx);
    addRoute("upgrades", &CommandHandler::upgrades);
    addRoute("dumpproposedsettings", &CommandHandler::dumpProposedSettings);
    addRoute("self-check", &CommandHandler::selfCheck);
    addRoute("sorobaninfo", &CommandHandler::sorobanInfo);

#ifdef BUILD_TESTS
    addRoute("generateload", &CommandHandler::generateLoad);
    addRoute("testacc", &CommandHandler::testAcc);
    addRoute("testtx", &CommandHandler::testTx);
    addRoute("getsurveyresult", &CommandHandler::getSurveyResult);
    addRoute("surveytopology", &CommandHandler::surveyTopology);
    addRoute("startsurveycollecting", &CommandHandler::startSurveyCollecting);
    addRoute("stopsurveycollecting", &CommandHandler::stopSurveyCollecting);
    addRoute("surveytopologytimesliced",
             &CommandHandler::surveyTopologyTimeSliced);
#endif
}

void
CommandHandler::addRoute(std::string const& name, HandlerRoute route)
{

    mServer->addRoute(
        name, std::bind(&CommandHandler::safeRouter, this, route, _1, _2));
}

void
CommandHandler::safeRouter(CommandHandler::HandlerRoute route,
                           std::string const& params, std::string& retStr)
{
    try
    {
        ZoneNamedN(httpZone, "HTTP command handler", true);
        route(this, params, retStr);
    }
    catch (std::exception const& e)
    {
        retStr = fmt::format(FMT_STRING(R"({{"exception": "{}"}})"), e.what());
    }
    catch (...)
    {
        retStr = R"({"exception": "generic"})";
    }
}

void
CommandHandler::ensureProtocolVersion(
    std::map<std::string, std::string> const& args, std::string const& argName,
    ProtocolVersion minVer)
{
    auto it = args.find(argName);
    if (it == args.end())
    {
        return;
    }
    ensureProtocolVersion(argName, minVer);
}

void
CommandHandler::ensureProtocolVersion(std::string const& errString,
                                      ProtocolVersion minVer)
{
    auto lhhe = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (protocolVersionIsBefore(lhhe.header.ledgerVersion, minVer))
    {
        throw std::invalid_argument(
            fmt::format("{} cannot be used before protocol v{}", errString,
                        static_cast<uint32>(minVer)));
        return;
    }
}

std::string
CommandHandler::manualCmd(std::string const& cmd)
{
    http::server::reply reply;
    http::server::request request;
    request.uri = cmd;
    mServer->handle_request(request, reply);
    LOG_INFO(DEFAULT_LOG, "{} -> {}", cmd, reply.content);
    return reply.content;
}

void
CommandHandler::fileNotFound(std::string const& params, std::string& retStr)
{
    retStr = "<b>Welcome to stellar-core!</b><p>";
    retStr +=
        "Supported HTTP commands are listed in the <a href=\""
        "https://github.com/stellar/stellar-core/blob/master/docs/software/"
        "commands.md#http-commands"
        "\">docs</a> as well as in the man pages.</p>"
        "<p>Have fun!</p>";
}

template <typename T>
std::optional<T>
parseOptionalParam(std::map<std::string, std::string> const& map,
                   std::string const& key)
{
    auto i = map.find(key);
    if (i != map.end())
    {
        std::stringstream str(i->second);
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

// If the key exists and the value successfully parses, return that value.
// If the key doesn't exist, return defaultValue.
// Otherwise, throws an error.
template <typename T>
T
parseOptionalParamOrDefault(std::map<std::string, std::string> const& map,
                            std::string const& key, T const& defaultValue)
{
    std::optional<T> res = parseOptionalParam<T>(map, key);
    if (res)
    {
        return *res;
    }
    else
    {
        return defaultValue;
    }
}

template <>
bool
parseOptionalParamOrDefault<bool>(std::map<std::string, std::string> const& map,
                                  std::string const& key,
                                  bool const& defaultValue)
{
    auto paramStr = parseOptionalParam<std::string>(map, key);
    if (!paramStr)
    {
        return defaultValue;
    }
    return *paramStr == "true";
}

// Return a value only if the key exists and the value parses.
// Otherwise, this throws an error.
template <typename T>
T
parseRequiredParam(std::map<std::string, std::string> const& map,
                   std::string const& key)
{
    auto res = parseOptionalParam<T>(map, key);
    if (!res)
    {
        std::string errorMsg =
            fmt::format(FMT_STRING("'{}' argument is required!"), key);
        throw std::runtime_error(errorMsg);
    }
    return *res;
}

void
CommandHandler::manualClose(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    if (!retMap.empty() && !mApp.getConfig().RUN_STANDALONE)
    {
        throw std::invalid_argument(
            "The 'manualclose' command accepts parameters only if the "
            "configuration includes RUN_STANDALONE=true.");
    }

    auto manualLedgerSeq = parseOptionalParam<uint32_t>(retMap, "ledgerSeq");
    auto manualCloseTime = parseOptionalParam<TimePoint>(retMap, "closeTime");

    retStr = mApp.manualClose(manualLedgerSeq, manualCloseTime);
}

void
CommandHandler::peers(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    bool fullKeys = retMap["fullkeys"] == "true";
    // compact should be true by default
    // as the response can be quite verbose.
    bool compact = retMap["compact"] != "false";
    Json::Value root;

    auto& pendingPeers = root["pending_peers"];
    auto addPendingPeers = [&](std::string const& direction,
                               std::vector<Peer::pointer> const& peers) {
        auto counter = 0;
        auto& node = pendingPeers[direction];
        for (auto const& peer : peers)
        {
            node[counter++] = peer->toString();
        }
    };
    addPendingPeers("outbound",
                    mApp.getOverlayManager().getOutboundPendingPeers());
    addPendingPeers("inbound",
                    mApp.getOverlayManager().getInboundPendingPeers());

    auto& authenticatedPeers = root["authenticated_peers"];
    auto addAuthenticatedPeers =
        [&](std::string const& direction,
            std::map<NodeID, Peer::pointer> const& peers) {
            auto counter = 0;
            auto& node = authenticatedPeers[direction];
            for (auto const& peer : peers)
            {
                auto& peerNode = node[counter++];
                peerNode = peer.second->getJsonInfo(compact);
                peerNode["id"] =
                    mApp.getConfig().toStrKey(peer.first, fullKeys);
            }
        };
    addAuthenticatedPeers(
        "outbound", mApp.getOverlayManager().getOutboundAuthenticatedPeers());
    addAuthenticatedPeers(
        "inbound", mApp.getOverlayManager().getInboundAuthenticatedPeers());

    retStr = root.toStyledString();
}

void
CommandHandler::info(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    retStr = mApp.getJsonInfo(retMap["compact"] == "false").toStyledString();
}

static bool
shouldEnable(std::set<std::string> const& toEnable,
             medida::MetricName const& name)
{
    // Enable individual metric name or a partition
    if (toEnable.find(name.domain()) == toEnable.end() &&
        toEnable.find(name.ToString()) == toEnable.end())
    {
        return false;
    }
    return true;
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    ZoneScoped;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    std::set<std::string> toEnable;

    // Filter which metrics to report based on the parameters
    auto metricToEnable = retMap.find("enable");
    if (metricToEnable != retMap.end())
    {
        std::stringstream ss(metricToEnable->second);
        std::string metric;

        while (getline(ss, metric, ','))
        {
            toEnable.insert(metric);
        }
    }

    mApp.syncAllMetrics();

    auto reportMetrics =
        [&](std::map<medida::MetricName,
                     std::shared_ptr<medida::MetricInterface>> const& metrics) {
            medida::reporting::JsonReporter jr(metrics);
            retStr = jr.Report();
        };

    if (!toEnable.empty())
    {
        std::map<medida::MetricName, std::shared_ptr<medida::MetricInterface>>
            metricsToReport;
        for (auto const& m : mApp.getMetrics().GetAllMetrics())
        {
            if (shouldEnable(toEnable, m.first))
            {
                metricsToReport.emplace(m);
            }
        }
        reportMetrics(metricsToReport);
    }
    else
    {
        reportMetrics(mApp.getMetrics().GetAllMetrics());
    }
}

void
CommandHandler::logRotate(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    retStr = "Log rotate...";

    Logging::rotate();
}

void
CommandHandler::connect(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerP = retMap.find("peer");
    auto portP = retMap.find("port");
    if (peerP != retMap.end() && portP != retMap.end())
    {
        std::stringstream str;
        str << peerP->second << ":" << portP->second;
        retStr = "Connect to: ";
        retStr += str.str();
        mApp.getOverlayManager().connectTo(
            PeerBareAddress::resolve(str.str(), mApp));
    }
    else
    {
        retStr = "Must specify a peer and port: connect&peer=PEER&port=PORT";
    }
}

void
CommandHandler::dropPeer(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    auto ban = retMap.find("ban");
    if (peerId != retMap.end())
    {
        auto found = false;
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            auto peers = mApp.getOverlayManager().getAuthenticatedPeers();
            auto peer = peers.find(n);
            if (peer != peers.end())
            {
                peer->second->sendErrorAndDrop(ERR_MISC, "dropped by user");
                if (ban != retMap.end() && ban->second == "1")
                {
                    retStr = "Drop and ban peer: ";
                    mApp.getBanManager().banNode(n);
                }
                else
                    retStr = "Drop peer: ";

                retStr += peerId->second;
                found = true;
            }
        }

        if (!found)
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: droppeer?node=NODE_ID";
    }
}

void
CommandHandler::bans(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    Json::Value root;

    root["bans"];
    int counter = 0;
    for (auto ban : mApp.getBanManager().getBans())
    {
        root["bans"][counter] = ban;

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::unban(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    if (peerId != retMap.end())
    {
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            retStr = "Unban peer: ";
            retStr += peerId->second;
            mApp.getBanManager().unbanNode(n);
        }
        else
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: unban?node=NODE_ID";
    }
}

void
CommandHandler::upgrades(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    auto s = retMap["mode"];
    if (s.empty())
    {
        retStr = "mode required";
        return;
    }
    if (s == "get")
    {
        retStr = mApp.getHerder().getUpgradesJson();
    }
    else if (s == "set")
    {
        Upgrades::UpgradeParameters p;

        auto upgradeTime = retMap["upgradetime"];
        std::tm tm;
        try
        {
            tm = VirtualClock::isoStringToTm(upgradeTime);
        }
        catch (std::exception&)
        {
            retStr = fmt::format(
                FMT_STRING("could not parse upgradetime: '{}'"), upgradeTime);
            return;
        }
        p.mUpgradeTime = VirtualClock::tmToSystemPoint(tm);

        p.mBaseFee = parseOptionalParam<uint32>(retMap, "basefee");
        p.mBaseReserve = parseOptionalParam<uint32>(retMap, "basereserve");
        p.mMaxTxSetSize = parseOptionalParam<uint32>(retMap, "maxtxsetsize");
        p.mProtocolVersion =
            parseOptionalParam<uint32>(retMap, "protocolversion");
        p.mFlags = parseOptionalParam<uint32>(retMap, "flags");

        auto configXdrIter = retMap.find("configupgradesetkey");
        if (configXdrIter != retMap.end())
        {
            ensureProtocolVersion(retMap, "configupgradesetkey",
                                  SOROBAN_PROTOCOL_VERSION);

            std::vector<uint8_t> buffer;
            decoder::decode_b64(configXdrIter->second, buffer);
            ConfigUpgradeSetKey key;
            xdr::xdr_from_opaque(buffer, key);
            auto ls = LedgerSnapshot(mApp);

            auto ptr = ConfigUpgradeSetFrame::makeFromKey(ls, key);

            if (!ptr ||
                ptr->isValidForApply() != Upgrades::UpgradeValidity::VALID)
            {
                retStr = "Error setting configUpgradeSet";
                return;
            }

            p.mConfigUpgradeSetKey = key;
        }
        ensureProtocolVersion(retMap, "maxsorobantxsetsize",
                              SOROBAN_PROTOCOL_VERSION);
        p.mMaxSorobanTxSetSize =
            parseOptionalParam<uint32>(retMap, "maxsorobantxsetsize");
        mApp.getHerder().setUpgrades(p);
    }
    else if (s == "clear")
    {
        Upgrades::UpgradeParameters p;
        mApp.getHerder().setUpgrades(p);
    }
    else
    {
        retStr = fmt::format(FMT_STRING("Unknown mode: {}"), s);
    }
}

void
CommandHandler::dumpProposedSettings(std::string const& params,
                                     std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    auto blob = retMap["blob"];
    if (!blob.empty())
    {
        ensureProtocolVersion("dumpproposedsettings", SOROBAN_PROTOCOL_VERSION);

        std::vector<uint8_t> buffer;
        decoder::decode_b64(blob, buffer);
        ConfigUpgradeSetKey key;
        xdr::xdr_from_opaque(buffer, key);
        auto ls = LedgerSnapshot(mApp);

        auto ptr = ConfigUpgradeSetFrame::makeFromKey(ls, key);

        if (!ptr || ptr->isValidForApply() != Upgrades::UpgradeValidity::VALID)
        {
            retStr = "configUpgradeSet is missing or invalid";
            return;
        }

        retStr = xdrToCerealString(ptr->toXDR().updatedEntry,
                                   "ConfigSettingsEntries");
    }
    else
    {
        throw std::invalid_argument(
            "Must specify a ConfigUpgradeSetKey blob: "
            "dumpproposedsettings?blob=<ConfigUpgradeSetKey in "
            "xdr format>");
    }
}

void
CommandHandler::selfCheck(std::string const&, std::string& retStr)
{
    ZoneScoped;
    // NB: this only runs the online "self-check" routine; running the
    // offline "self-check" from command-line will also do an expensive,
    // synchronous database-vs-bucketlist consistency check. We can't do
    // that online since it would block for so long that the node would
    // lose sync. So we just omit it here.
    mApp.scheduleSelfCheck(true);
}

void
CommandHandler::quorum(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    NodeID n;

    std::string nID = retMap["node"];

    if (nID.empty())
    {
        n = mApp.getConfig().NODE_SEED.getPublicKey();
    }
    else
    {
        if (!mApp.getHerder().resolveNodeID(nID, n))
        {
            throw std::invalid_argument("unknown name");
        }
    }

    Json::Value root;
    if (retMap["transitive"] == "true")
    {
        root = mApp.getHerder().getJsonTransitiveQuorumInfo(
            n, retMap["compact"] == "true", retMap["fullkeys"] == "true");
    }
    else
    {
        root = mApp.getHerder().getJsonQuorumInfo(
            n, retMap["compact"] == "true", retMap["fullkeys"] == "true", 0);
    }
    retStr = root.toStyledString();
}

void
CommandHandler::scpInfo(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    size_t lim = parseOptionalParamOrDefault<size_t>(retMap, "limit", 2);

    auto root = mApp.getHerder().getJsonInfo(lim, retMap["fullkeys"] == "true");
    retStr = root.toStyledString();
}

void
CommandHandler::sorobanInfo(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    auto& lm = mApp.getLedgerManager();

    if (lm.hasSorobanNetworkConfig())
    {
        std::map<std::string, std::string> retMap;
        http::server::server::parseParams(params, retMap);

        // Format is the only acceptable param, but it is optional
        if (retMap.count("format") != retMap.size())
        {
            retStr = "Invalid param";
            return;
        }

        auto format =
            parseOptionalParamOrDefault<std::string>(retMap, "format", "basic");
        if (format == "basic")
        {
            Json::Value res;
            auto const& conf = lm.getSorobanNetworkConfigReadOnly();

            // Contract size
            res["max_contract_size"] = conf.maxContractSizeBytes();

            // Contract data
            res["max_contract_data_key_size"] =
                conf.maxContractDataKeySizeBytes();
            res["max_contract_data_entry_size"] =
                conf.maxContractDataEntrySizeBytes();

            // Compute settings
            res["tx"]["max_instructions"] =
                static_cast<Json::Int64>(conf.txMaxInstructions());
            res["ledger"]["max_instructions"] =
                static_cast<Json::Int64>(conf.ledgerMaxInstructions());
            res["fee_rate_per_instructions_increment"] =
                static_cast<Json::Int64>(
                    conf.feeRatePerInstructionsIncrement());
            res["tx"]["memory_limit"] = conf.txMemoryLimit();

            // Ledger access settings
            res["ledger"]["max_read_ledger_entries"] =
                conf.ledgerMaxReadLedgerEntries();
            res["ledger"]["max_read_bytes"] = conf.ledgerMaxReadBytes();
            res["ledger"]["max_write_ledger_entries"] =
                conf.ledgerMaxWriteLedgerEntries();
            res["ledger"]["max_write_bytes"] = conf.ledgerMaxWriteBytes();
            res["tx"]["max_read_ledger_entries"] =
                conf.txMaxReadLedgerEntries();
            res["tx"]["max_read_bytes"] = conf.txMaxReadBytes();
            res["tx"]["max_write_ledger_entries"] =
                conf.txMaxWriteLedgerEntries();
            res["tx"]["max_write_bytes"] = conf.txMaxWriteBytes();

            // Fees
            res["fee_read_ledger_entry"] =
                static_cast<Json::Int64>(conf.feeReadLedgerEntry());
            res["fee_write_ledger_entry"] =
                static_cast<Json::Int64>(conf.feeWriteLedgerEntry());
            res["fee_read_1kb"] = static_cast<Json::Int64>(conf.feeRead1KB());
            res["fee_write_1kb"] = static_cast<Json::Int64>(conf.feeWrite1KB());
            res["fee_historical_1kb"] =
                static_cast<Json::Int64>(conf.feeHistorical1KB());

            // Contract events settings
            res["tx"]["max_contract_events_size_bytes"] =
                conf.txMaxContractEventsSizeBytes();
            res["fee_contract_events_size_1kb"] =
                static_cast<Json::Int64>(conf.feeContractEventsSize1KB());

            // Bandwidth related data settings
            res["ledger"]["max_tx_size_bytes"] =
                conf.ledgerMaxTransactionSizesBytes();
            res["tx"]["max_size_bytes"] = conf.txMaxSizeBytes();
            res["fee_transaction_size_1kb"] =
                static_cast<Json::Int64>(conf.feeTransactionSize1KB());

            // General execution ledger settings
            res["ledger"]["max_tx_count"] = conf.ledgerMaxTxCount();

            // State archival settings
            auto& archivalInfo = res["state_archival"];
            auto const& stateArchivalSettings = conf.stateArchivalSettings();
            archivalInfo["max_entry_ttl"] = stateArchivalSettings.maxEntryTTL;
            archivalInfo["min_temporary_ttl"] =
                stateArchivalSettings.minTemporaryTTL;
            archivalInfo["min_persistent_ttl"] =
                stateArchivalSettings.minPersistentTTL;

            archivalInfo["persistent_rent_rate_denominator"] =
                static_cast<Json::Int64>(
                    stateArchivalSettings.persistentRentRateDenominator);
            archivalInfo["temp_rent_rate_denominator"] =
                static_cast<Json::Int64>(
                    stateArchivalSettings.tempRentRateDenominator);

            archivalInfo["max_entries_to_archive"] =
                stateArchivalSettings.maxEntriesToArchive;
            archivalInfo["bucketlist_size_window_sample_size"] =
                stateArchivalSettings.bucketListSizeWindowSampleSize;

            archivalInfo["eviction_scan_size"] = static_cast<Json::UInt64>(
                stateArchivalSettings.evictionScanSize);
            archivalInfo["starting_eviction_scan_level"] =
                stateArchivalSettings.startingEvictionScanLevel;
            archivalInfo["bucket_list_size_snapshot_period"] =
                stateArchivalSettings.bucketListSizeWindowSampleSize;

            // non-configurable settings
            archivalInfo["average_bucket_list_size"] =
                static_cast<Json::UInt64>(conf.getAverageBucketListSize());
            retStr = res.toStyledString();
        }
        else if (format == "detailed")
        {
            LedgerSnapshot lsg(mApp);
            xdr::xvector<ConfigSettingEntry> entries;
            for (auto c : xdr::xdr_traits<ConfigSettingID>::enum_values())
            {
                auto entry =
                    lsg.load(configSettingKey(static_cast<ConfigSettingID>(c)));
                entries.emplace_back(entry.current().data.configSetting());
            }

            retStr = xdrToCerealString(entries, "ConfigSettingsEntries");
        }
        else if (format == "upgrade_xdr")
        {
            LedgerSnapshot lsg(mApp);

            ConfigUpgradeSet upgradeSet;
            for (auto c : xdr::xdr_traits<ConfigSettingID>::enum_values())
            {
                auto configSettingID = static_cast<ConfigSettingID>(c);
                if (SorobanNetworkConfig::isNonUpgradeableConfigSettingEntry(
                        configSettingID))
                {
                    continue;
                }
                auto entry = lsg.load(configSettingKey(configSettingID));
                upgradeSet.updatedEntry.emplace_back(
                    entry.current().data.configSetting());
            }

            retStr = decoder::encode_b64(xdr::xdr_to_opaque(upgradeSet));
        }
        else
        {
            retStr = "Invalid format option";
        }
    }
    else
    {
        retStr = "Soroban is not active in current ledger version";
    }
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    std::string levelStr = retMap["level"];
    std::string partition = retMap["partition"];
    if (!levelStr.size())
    {
        for (auto& p : Logging::kPartitionNames)
        {
            root[p] = Logging::getStringFromLL(Logging::getLogLevel(p));
        }
    }
    else
    {
        LogLevel level = Logging::getLLfromString(levelStr);
        if (partition.size())
        {
            partition = Logging::normalizePartition(partition);
            Logging::setLogLevel(level, partition.c_str());
            root[partition] = Logging::getStringFromLL(level);
        }
        else
        {
            Logging::setLogLevel(level, nullptr);
            root["Global"] = Logging::getStringFromLL(level);
        }
    }

    retStr = root.toStyledString();
}

void
CommandHandler::tx(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    Json::Value root;

    std::map<std::string, std::string> paramMap;
    http::server::server::parseParams(params, paramMap);
    std::string blob = paramMap["blob"];

    if (!blob.empty())
    {
        TransactionEnvelope envelope;
        std::vector<uint8_t> binBlob;
        decoder::decode_b64(blob, binBlob);
        xdr::xdr_from_opaque(binBlob, envelope);

        {
            auto lhhe = mApp.getLedgerManager().getLastClosedLedgerHeader();
            if (protocolVersionStartsFrom(lhhe.header.ledgerVersion,
                                          ProtocolVersion::V_13))
            {
                envelope = txbridge::convertForV13(envelope);
            }
        }

        auto transaction = TransactionFrameBase::makeTransactionFromWire(
            mApp.getNetworkID(), envelope);
        if (transaction)
        {
            // Add it to our current set and make sure it is valid.
            auto addResult =
                mApp.getHerder().recvTransaction(transaction, true);

            root["status"] = TX_STATUS_STRING[static_cast<int>(addResult.code)];
            if (addResult.code ==
                TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
            {
                std::string resultBase64;
                releaseAssertOrThrow(addResult.txResult);

                auto const& payload = addResult.txResult;
                auto resultBin = xdr::xdr_to_opaque(payload->getResult());
                resultBase64.reserve(decoder::encoded_size64(resultBin.size()) +
                                     1);
                resultBase64 = decoder::encode_b64(resultBin);
                root["error"] = resultBase64;
                if (mApp.getConfig().ENABLE_DIAGNOSTICS_FOR_TX_SUBMISSION &&
                    transaction->isSoroban() &&
                    !payload->getDiagnosticEvents().empty())
                {
                    auto diagsBin =
                        xdr::xdr_to_opaque(payload->getDiagnosticEvents());
                    auto diagsBase64 = decoder::encode_b64(diagsBin);
                    root["diagnostic_events"] = diagsBase64;
                }
            }
        }
    }
    else
    {
        throw std::invalid_argument("Must specify a tx blob: tx?blob=<tx in "
                                    "xdr format>");
    }

    retStr = Json::FastWriter().write(root);
}

void
CommandHandler::maintenance(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    if (map["queue"] == "true")
    {
        uint32_t count =
            parseOptionalParamOrDefault<uint32_t>(map, "count", 50000);

        mApp.getMaintainer().performMaintenance(count);
        retStr = "Done";
    }
    else
    {
        retStr = "No work performed";
    }
}

void
CommandHandler::clearMetrics(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    std::string domain =
        parseOptionalParamOrDefault<std::string>(map, "domain", "");

    mApp.clearMetrics(domain);

    retStr = fmt::format(FMT_STRING("Cleared {} metrics!"), domain);
}

void
CommandHandler::checkBooted() const
{
    if (mApp.getState() == Application::APP_CREATED_STATE ||
        mApp.getHerder().getState() == Herder::HERDER_BOOTING_STATE)
    {
        throw std::runtime_error(
            "Application is not fully booted, try again later");
    }
}

void
CommandHandler::surveyTopology(std::string const& params, std::string& retStr)
{
    ZoneScoped;

    CLOG_WARNING(
        Overlay,
        "`surveytopology` is deprecated and will be removed in a future "
        "release.  Please use the new time sliced survey interface.");

    checkBooted();

    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    auto duration =
        std::chrono::seconds(parseRequiredParam<uint32>(map, "duration"));
    auto idString = parseRequiredParam<std::string>(map, "node");
    NodeID id = KeyUtils::fromStrKey<NodeID>(idString);

    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();

    bool success = surveyManager.startSurveyReporting(
        SurveyMessageCommandType::SURVEY_TOPOLOGY, duration);

    surveyManager.addNodeToRunningSurveyBacklog(
        SurveyMessageCommandType::SURVEY_TOPOLOGY, duration, id, std::nullopt,
        std::nullopt);
    retStr = "Adding node.";

    retStr += success ? "Survey started " : "Survey already running!";
}

void
CommandHandler::stopSurvey(std::string const&, std::string& retStr)
{
    ZoneScoped;
    CLOG_WARNING(Overlay,
                 "`stopsurvey` is deprecated and will be removed in a future "
                 "release.  Please use the new time sliced survey interface.");
    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    surveyManager.stopSurveyReporting();
    retStr = "survey stopped";
}

void
CommandHandler::getSurveyResult(std::string const&, std::string& retStr)
{
    ZoneScoped;
    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    retStr = surveyManager.getJsonResults().toStyledString();
}

void
CommandHandler::startSurveyCollecting(std::string const& params,
                                      std::string& retStr)
{
    ZoneScoped;
    checkBooted();

    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    uint32_t const nonce = parseRequiredParam<uint32_t>(map, "nonce");

    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    if (surveyManager.broadcastStartSurveyCollecting(nonce))
    {
        retStr = "Requested network to start survey collecting.";
    }
    else
    {
        retStr = "Failed to start survey collecting. Another survey is active "
                 "on the network.";
    }
}

void
CommandHandler::stopSurveyCollecting(std::string const&, std::string& retStr)
{
    ZoneScoped;
    checkBooted();

    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    if (surveyManager.broadcastStopSurveyCollecting())
    {
        retStr = "Requested network to stop survey collecting.";
    }
    else
    {
        retStr = "Failed to stop survey collecting. No survey is active on the "
                 "network.";
    }
}

void
CommandHandler::surveyTopologyTimeSliced(std::string const& params,
                                         std::string& retStr)
{
    ZoneScoped;
    checkBooted();

    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    auto idString = parseRequiredParam<std::string>(map, "node");
    NodeID id = KeyUtils::fromStrKey<NodeID>(idString);
    auto inboundPeerIndex = parseRequiredParam<uint32>(map, "inboundpeerindex");
    auto outboundPeerIndex =
        parseRequiredParam<uint32>(map, "outboundpeerindex");

    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();

    bool success = surveyManager.startSurveyReporting(
        SurveyMessageCommandType::TIME_SLICED_SURVEY_TOPOLOGY,
        /*surveyDuration*/ std::nullopt);

    surveyManager.addNodeToRunningSurveyBacklog(
        SurveyMessageCommandType::TIME_SLICED_SURVEY_TOPOLOGY,
        /*surveyDuration*/ std::nullopt, id, inboundPeerIndex,
        outboundPeerIndex);
    retStr = "Adding node.";

    retStr += success ? "Survey started " : "Survey already running!";
}

#ifdef BUILD_TESTS
void
CommandHandler::generateLoad(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    if (mApp.getConfig().ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING)
    {
        std::map<std::string, std::string> map;
        http::server::server::parseParams(params, map);
        auto modeStr =
            parseOptionalParamOrDefault<std::string>(map, "mode", "create");
        // First check if a current run needs to be stopped
        if (modeStr == "stop")
        {
            mApp.getLoadGenerator().stop();
            retStr = "Stopped load generation";
            return;
        }

        GeneratedLoadConfig cfg;
        cfg.mode = LoadGenerator::getMode(modeStr);

        cfg.nAccounts =
            parseOptionalParamOrDefault<uint32_t>(map, "accounts", 1000);
        cfg.nTxs = parseOptionalParamOrDefault<uint32_t>(map, "txs", 0);
        cfg.txRate = parseOptionalParamOrDefault<uint32_t>(map, "txrate", 10);
        auto batchSize = parseOptionalParamOrDefault<uint32_t>(map, "batchsize",
                                                               MAX_OPS_PER_TX);
        if (batchSize != MAX_OPS_PER_TX)
        {
            CLOG_WARNING(LoadGen,
                         "LoadGenerator: batchSize is deprecated, "
                         "automatically set to {}",
                         MAX_OPS_PER_TX);
        }
        cfg.offset = parseOptionalParamOrDefault<uint32_t>(map, "offset", 0);
        uint32_t spikeIntervalInt =
            parseOptionalParamOrDefault<uint32_t>(map, "spikeinterval", 0);
        cfg.spikeInterval = std::chrono::seconds(spikeIntervalInt);
        cfg.spikeSize =
            parseOptionalParamOrDefault<uint32_t>(map, "spikesize", 0);
        cfg.maxGeneratedFeeRate =
            parseOptionalParam<uint32_t>(map, "maxfeerate");
        cfg.skipLowFeeTxs =
            parseOptionalParamOrDefault<bool>(map, "skiplowfeetxs", false);

        if (cfg.mode == LoadGenMode::MIXED_CLASSIC)
        {
            cfg.getMutDexTxPercent() =
                parseOptionalParamOrDefault<uint32_t>(map, "dextxpercent", 0);
        }

        if (cfg.isSoroban())
        {
            uint32_t minPercentSuccess = parseOptionalParamOrDefault<uint32_t>(
                map, "minpercentsuccess", 0);
            cfg.setMinSorobanPercentSuccess(minPercentSuccess);
            if (cfg.mode != LoadGenMode::SOROBAN_UPLOAD)
            {

                auto& sorobanCfg = cfg.getMutSorobanConfig();
                sorobanCfg.nInstances =
                    parseOptionalParamOrDefault<uint32_t>(map, "instances", 0);
                sorobanCfg.nWasms =
                    parseOptionalParamOrDefault<uint32_t>(map, "wasms", 0);
            }
        }

        if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
        {
            auto& upgradeCfg = cfg.getMutSorobanUpgradeConfig();
            upgradeCfg.maxContractSizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "mxcntrctsz", 0);
            upgradeCfg.maxContractDataKeySizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "mxcntrctkeysz", 0);
            upgradeCfg.maxContractDataEntrySizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "mxcntrctdatasz", 0);
            upgradeCfg.ledgerMaxInstructions =
                parseOptionalParamOrDefault<uint64_t>(map, "ldgrmxinstrc", 0);
            upgradeCfg.txMaxInstructions =
                parseOptionalParamOrDefault<uint64_t>(map, "txmxinstrc", 0);
            upgradeCfg.txMemoryLimit =
                parseOptionalParamOrDefault<uint64_t>(map, "txmemlim", 0);
            upgradeCfg.ledgerMaxReadLedgerEntries =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxrdntry", 0);
            upgradeCfg.ledgerMaxReadBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxrdbyt", 0);
            upgradeCfg.ledgerMaxWriteLedgerEntries =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxwrntry", 0);
            upgradeCfg.ledgerMaxWriteBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxwrbyt", 0);
            upgradeCfg.ledgerMaxTxCount =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxtxcnt", 0);
            upgradeCfg.txMaxReadLedgerEntries =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxrdntry", 0);
            upgradeCfg.txMaxReadBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxrdbyt", 0);
            upgradeCfg.txMaxWriteLedgerEntries =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxwrntry", 0);
            upgradeCfg.txMaxWriteBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxwrbyt", 0);
            upgradeCfg.txMaxContractEventsSizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxevntsz", 0);
            upgradeCfg.ledgerMaxTransactionsSizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "ldgrmxtxsz", 0);
            upgradeCfg.txMaxSizeBytes =
                parseOptionalParamOrDefault<uint32_t>(map, "txmxsz", 0);
            upgradeCfg.bucketListSizeWindowSampleSize =
                parseOptionalParamOrDefault<uint32_t>(map, "wndowsz", 0);
            upgradeCfg.evictionScanSize =
                parseOptionalParamOrDefault<uint64_t>(map, "evctsz", 0);
            upgradeCfg.startingEvictionScanLevel =
                parseOptionalParamOrDefault<uint32_t>(map, "evctlvl", 0);
        }

        if (cfg.mode == LoadGenMode::MIXED_CLASSIC_SOROBAN)
        {
            auto& mixCfg = cfg.getMutMixClassicSorobanConfig();
            mixCfg.payWeight =
                parseOptionalParamOrDefault<uint32_t>(map, "payweight", 0);
            mixCfg.sorobanUploadWeight = parseOptionalParamOrDefault<uint32_t>(
                map, "sorobanuploadweight", 0);
            mixCfg.sorobanInvokeWeight = parseOptionalParamOrDefault<uint32_t>(
                map, "sorobaninvokeweight", 0);
            if (!(mixCfg.payWeight || mixCfg.sorobanUploadWeight ||
                  mixCfg.sorobanInvokeWeight))
            {
                retStr = "At least one mix weight must be non-zero";
                return;
            }
        }

        if (cfg.maxGeneratedFeeRate)
        {
            auto baseFee = mApp.getLedgerManager().getLastTxFee();
            if (baseFee > *cfg.maxGeneratedFeeRate)
            {
                retStr = "maxfeerate is smaller than minimum base fee, load "
                         "generation skipped.";
                return;
            }
        }

        Json::Value res;
        res["status"] = cfg.getStatus();
        mApp.generateLoad(cfg);

        if (cfg.mode == LoadGenMode::SOROBAN_CREATE_UPGRADE)
        {
            auto configUpgradeKey =
                mApp.getLoadGenerator().getConfigUpgradeSetKey(
                    cfg.getSorobanUpgradeConfig());
            auto configUpgradeKeyStr = stellar::decoder::encode_b64(
                xdr::xdr_to_opaque(configUpgradeKey));
            res["config_upgrade_set_key"] = configUpgradeKeyStr;
        }

        retStr = res.toStyledString();
    }
    else
    {
        retStr = "Set ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true in "
                 "the stellar-core.cfg if you want this behavior";
    }
}

void
CommandHandler::testAcc(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    using namespace txtest;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    Json::Value root;
    auto accName = retMap.find("name");
    if (accName == retMap.end())
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: testacc?name=bob";
    }
    else
    {
        SecretKey key;
        if (accName->second == "root")
        {
            key = getRoot(mApp.getNetworkID());
        }
        else
        {
            key = getAccount(accName->second.c_str());
        }

        LedgerSnapshot lsg(mApp);
        auto acc = lsg.load(accountKey(key.getPublicKey()));
        if (acc)
        {
            auto const& ae = acc.current().data.account();
            root["name"] = accName->second;
            root["id"] = KeyUtils::toStrKey(ae.accountID);
            root["balance"] = (Json::Int64)ae.balance;
            root["seqnum"] = (Json::UInt64)ae.seqNum;
        }
    }
    retStr = root.toStyledString();
}

void
CommandHandler::testTx(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    using namespace txtest;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto to = retMap.find("to");
    auto from = retMap.find("from");
    auto amount = retMap.find("amount");
    auto create = retMap.find("create");

    Json::Value root;

    if (to != retMap.end() && from != retMap.end() && amount != retMap.end())
    {
        Hash const& networkID = mApp.getNetworkID();

        auto toAccount =
            to->second == "root"
                ? TestAccount{mApp, getRoot(networkID)}
                : TestAccount{mApp, getAccount(to->second.c_str())};
        auto fromAccount =
            from->second == "root"
                ? TestAccount{mApp, getRoot(networkID)}
                : TestAccount{mApp, getAccount(from->second.c_str())};

        uint64_t paymentAmount = 0;
        std::istringstream iss(amount->second);
        iss >> paymentAmount;

        root["from_name"] = from->second;
        root["to_name"] = to->second;
        root["from_id"] = KeyUtils::toStrKey(fromAccount.getPublicKey());
        root["to_id"] = KeyUtils::toStrKey(toAccount.getPublicKey());
        root["amount"] = (Json::UInt64)paymentAmount;

        TransactionTestFramePtr txFrame;
        if (create != retMap.end() && create->second == "true")
        {
            txFrame = fromAccount.tx({createAccount(toAccount, paymentAmount)});
        }
        else
        {
            txFrame = fromAccount.tx({payment(toAccount, paymentAmount)});
        }

        auto addResult = mApp.getHerder().recvTransaction(txFrame, true);
        root["status"] = TX_STATUS_STRING[static_cast<int>(addResult.code)];
        if (addResult.code == TransactionQueue::AddResultCode::ADD_STATUS_ERROR)
        {
            releaseAssert(addResult.txResult);
            root["detail"] = xdrToCerealString(
                addResult.txResult->getResultCode(), "TransactionResultCode");
        }
    }
    else
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: "
                         "testtx?from=root&to=bob&amount=1000000000";
    }
    retStr = root.toStyledString();
}
#endif
}
