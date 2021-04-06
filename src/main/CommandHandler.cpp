// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandHandler.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "herder/Herder.h"
#include "history/HistoryArchiveManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/Maintainer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/SurveyManager.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/reporting/json_reporter.h"
#include "util/Decoder.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"

#include "ExternalQueue.h"

#ifdef BUILD_TESTS
#include "test/TestAccount.h"
#include "test/TxTests.h"
#endif
#include <regex>

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
    }
    else
    {
        mServer = std::make_unique<http::server::server>(
            app.getClock().getIOContext());
    }

    mServer->add404(std::bind(&CommandHandler::fileNotFound, this, _1, _2));

    if (mApp.getConfig().MODE_STORES_HISTORY)
    {
        addRoute("dropcursor", &CommandHandler::dropcursor);
        addRoute("getcursor", &CommandHandler::getcursor);
        addRoute("setcursor", &CommandHandler::setcursor);
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
    addRoute("self-check", &CommandHandler::selfCheck);

#ifdef BUILD_TESTS
    addRoute("generateload", &CommandHandler::generateLoad);
    addRoute("testacc", &CommandHandler::testAcc);
    addRoute("testtx", &CommandHandler::testTx);
    addRoute("getsurveyresult", &CommandHandler::getSurveyResult);
    addRoute("surveytopology", &CommandHandler::surveyTopology);
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
        retStr = fmt::format(R"({{"exception": "{}"}})", e.what());
    }
    catch (...)
    {
        retStr = R"({"exception": "generic"})";
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
optional<T>
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
                fmt::format("Failed to parse '{}' argument", key);
            throw std::runtime_error(errorMsg);
        }
        return make_optional<T>(val);
    }

    return nullopt<T>();
}

// If the key exists and the value successfully parses, return that value.
// If the key doesn't exist, return defaultValue.
// Otherwise, throws an error.
template <typename T>
T
parseOptionalParamOrDefault(std::map<std::string, std::string> const& map,
                            std::string const& key, T const& defaultValue)
{
    optional<T> res = parseOptionalParam<T>(map, key);
    if (res)
    {
        return *res;
    }
    else
    {
        return defaultValue;
    }
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
        std::string errorMsg = fmt::format("'{}' argument is required!", key);
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
                peerNode["address"] = peer.second->toString();
                peerNode["elapsed"] = (int)peer.second->getLifeTime().count();
                peerNode["latency"] = (int)peer.second->getPing().count();
                peerNode["ver"] = peer.second->getRemoteVersion();
                peerNode["olver"] = (int)peer.second->getRemoteOverlayVersion();
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
CommandHandler::info(std::string const&, std::string& retStr)
{
    ZoneScoped;
    retStr = mApp.getJsonInfo().toStyledString();
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    mApp.syncAllMetrics();
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
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
                peer->second->sendErrorAndDrop(
                    ERR_MISC, "dropped by user",
                    Peer::DropMode::IGNORE_WRITE_QUEUE);
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
        catch (std::exception)
        {
            retStr =
                fmt::format("could not parse upgradetime: '{}'", upgradeTime);
            return;
        }
        p.mUpgradeTime = VirtualClock::tmToSystemPoint(tm);

        p.mBaseFee = parseOptionalParam<uint32>(retMap, "basefee");
        p.mBaseReserve = parseOptionalParam<uint32>(retMap, "basereserve");
        p.mMaxTxSize = parseOptionalParam<uint32>(retMap, "maxtxsize");
        p.mProtocolVersion =
            parseOptionalParam<uint32>(retMap, "protocolversion");

        mApp.getHerder().setUpgrades(p);
    }
    else if (s == "clear")
    {
        Upgrades::UpgradeParameters p;
        mApp.getHerder().setUpgrades(p);
    }
    else
    {
        retStr = fmt::format("Unknown mode: {}", s);
    }
}

void
CommandHandler::selfCheck(std::string const&, std::string& retStr)
{
    ZoneScoped;
    mApp.getHistoryArchiveManager().scheduleHistoryArchiveReportWork();
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
    std::ostringstream output;

    const std::string prefix("?blob=");
    if (params.compare(0, prefix.size(), prefix) == 0)
    {
        TransactionEnvelope envelope;
        std::string blob = params.substr(prefix.size());
        std::vector<uint8_t> binBlob;
        decoder::decode_b64(blob, binBlob);
        xdr::xdr_from_opaque(binBlob, envelope);

        {
            auto lhhe = mApp.getLedgerManager().getLastClosedLedgerHeader();
            if (lhhe.header.ledgerVersion >= 13)
            {
                envelope = txbridge::convertForV13(envelope);
            }
        }

        auto transaction = TransactionFrameBase::makeTransactionFromWire(
            mApp.getNetworkID(), envelope);
        if (transaction)
        {
            // add it to our current set
            // and make sure it is valid
            TransactionQueue::AddResult status =
                mApp.getHerder().recvTransaction(transaction);

            output << "{"
                   << "\"status\": "
                   << "\"" << TX_STATUS_STRING[static_cast<int>(status)]
                   << "\"";
            if (status == TransactionQueue::AddResult::ADD_STATUS_ERROR)
            {
                std::string resultBase64;
                auto resultBin = xdr::xdr_to_opaque(transaction->getResult());
                resultBase64.reserve(decoder::encoded_size64(resultBin.size()) +
                                     1);
                resultBase64 = decoder::encode_b64(resultBin);

                output << " , \"error\": \"" << resultBase64 << "\"";
            }
            output << "}";
        }
    }
    else
    {
        throw std::invalid_argument("Must specify a tx blob: tx?blob=<tx in "
                                    "xdr format>\"}");
    }

    retStr = output.str();
}

void
CommandHandler::dropcursor(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.deleteCursor(id);
        retStr = "Done";
    }
}

void
CommandHandler::setcursor(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    uint32 cursor = parseRequiredParam<uint32>(map, "cursor");

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.setCursorForResource(id, cursor);
        retStr = "Done";
    }
}

void
CommandHandler::getcursor(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    Json::Value root;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    // the decision was made not to check validity here
    // because there are subsequent checks for that in
    // ExternalQueue and if an exception is thrown for
    // validity there, the ret format is technically more
    // correct for the mime type
    ExternalQueue ps(mApp);
    std::map<std::string, uint32> curMap;
    int counter = 0;
    ps.getCursorForResource(id, curMap);
    root["cursors"][0];
    for (auto cursor : curMap)
    {
        root["cursors"][counter]["id"] = cursor.first;
        root["cursors"][counter]["cursor"] = cursor.second;
        counter++;
    }

    retStr = root.toStyledString();
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

    retStr = fmt::format("Cleared {} metrics!", domain);
}

void
CommandHandler::surveyTopology(std::string const& params, std::string& retStr)
{
    ZoneScoped;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    auto duration =
        std::chrono::seconds(parseRequiredParam<uint32>(map, "duration"));
    auto idString = parseRequiredParam<std::string>(map, "node");
    NodeID id = KeyUtils::fromStrKey<PublicKey>(idString);

    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();

    bool success = surveyManager.startSurvey(
        SurveyMessageCommandType::SURVEY_TOPOLOGY, duration);

    surveyManager.addNodeToRunningSurveyBacklog(
        SurveyMessageCommandType::SURVEY_TOPOLOGY, duration, id);
    retStr = "Adding node.";

    retStr += success ? "Survey started " : "Survey already running!";
}

void
CommandHandler::stopSurvey(std::string const&, std::string& retStr)
{
    ZoneScoped;
    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    surveyManager.stopSurvey();
    retStr = "survey stopped";
}

void
CommandHandler::getSurveyResult(std::string const&, std::string& retStr)
{
    ZoneScoped;
    auto& surveyManager = mApp.getOverlayManager().getSurveyManager();
    retStr = surveyManager.getJsonResults().toStyledString();
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

        bool isCreate;
        std::string mode =
            parseOptionalParamOrDefault<std::string>(map, "mode", "create");
        if (mode == std::string("create"))
        {
            isCreate = true;
        }
        else if (mode == std::string("pay"))
        {
            isCreate = false;
        }
        else
        {
            throw std::runtime_error("Unknown mode.");
        }

        uint32_t nAccounts =
            parseOptionalParamOrDefault<uint32_t>(map, "accounts", 1000);
        uint32_t nTxs = parseOptionalParamOrDefault<uint32_t>(map, "txs", 0);
        uint32_t txRate =
            parseOptionalParamOrDefault<uint32_t>(map, "txrate", 10);
        uint32_t batchSize = parseOptionalParamOrDefault<uint32_t>(
            map, "batchsize", 100); // Only for account creations
        uint32_t offset =
            parseOptionalParamOrDefault<uint32_t>(map, "offset", 0);
        uint32_t spikeIntervalInt =
            parseOptionalParamOrDefault<uint32_t>(map, "spikeinterval", 0);
        std::chrono::seconds spikeInterval(spikeIntervalInt);
        uint32_t spikeSize =
            parseOptionalParamOrDefault<uint32_t>(map, "spikesize", 0);

        uint32_t numItems = isCreate ? nAccounts : nTxs;
        std::string itemType = isCreate ? "accounts" : "txs";

        if (batchSize > 100)
        {
            batchSize = 100;
            retStr = "Setting batch size to its limit of 100.";
        }

        mApp.generateLoad(isCreate, nAccounts, offset, nTxs, txRate, batchSize,
                          spikeInterval, spikeSize);

        retStr += fmt::format(" Generating load: {:d} {:s}, {:d} tx/s",
                              numItems, itemType, txRate);
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

        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, key.getPublicKey());
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

        TransactionFramePtr txFrame;
        if (create != retMap.end() && create->second == "true")
        {
            txFrame = fromAccount.tx({createAccount(toAccount, paymentAmount)});
        }
        else
        {
            txFrame = fromAccount.tx({payment(toAccount, paymentAmount)});
        }

        auto status = mApp.getHerder().recvTransaction(txFrame);
        root["status"] = TX_STATUS_STRING[static_cast<int>(status)];
        if (status == TransactionQueue::AddResult::ADD_STATUS_ERROR)
        {
            root["detail"] = xdr_to_string(txFrame->getResult().result.code(),
                                           "TransactionResultCode");
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
