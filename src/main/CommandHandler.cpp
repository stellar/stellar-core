// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandHandler.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/Maintainer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"
#include "util/StatusManager.h"

#include "medida/reporting/json_reporter.h"
#include "util/Decoder.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

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
        LOG(INFO) << "Listening on " << ipStr << ":"
                  << mApp.getConfig().HTTP_PORT << " for HTTP requests";

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

    addRoute("bans", &CommandHandler::bans);
    addRoute("connect", &CommandHandler::connect);
    addRoute("dropcursor", &CommandHandler::dropcursor);
    addRoute("droppeer", &CommandHandler::dropPeer);
    addRoute("getcursor", &CommandHandler::getcursor);
    addRoute("info", &CommandHandler::info);
    addRoute("ll", &CommandHandler::ll);
    addRoute("logrotate", &CommandHandler::logRotate);
    addRoute("maintenance", &CommandHandler::maintenance);
    addRoute("manualclose", &CommandHandler::manualClose);
    addRoute("metrics", &CommandHandler::metrics);
    addRoute("clearmetrics", &CommandHandler::clearMetrics);
    addRoute("peers", &CommandHandler::peers);
    addRoute("quorum", &CommandHandler::quorum);
    addRoute("setcursor", &CommandHandler::setcursor);
    addRoute("scp", &CommandHandler::scpInfo);
    addRoute("tx", &CommandHandler::tx);
    addRoute("upgrades", &CommandHandler::upgrades);
    addRoute("unban", &CommandHandler::unban);

#ifdef BUILD_TESTS
    addRoute("generateload", &CommandHandler::generateLoad);
    addRoute("testacc", &CommandHandler::testAcc);
    addRoute("testtx", &CommandHandler::testTx);
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
        route(this, params, retStr);
    }
    catch (std::exception& e)
    {
        retStr =
            (fmt::MemoryWriter() << "{\"exception\": \"" << e.what() << "\"}")
                .str();
    }
    catch (...)
    {
        retStr = "{\"exception\": \"generic\"}";
    }
}

void
CommandHandler::manualCmd(std::string const& cmd)
{
    http::server::reply reply;
    http::server::request request;
    request.uri = cmd;
    mServer->handle_request(request, reply);
    LOG(INFO) << cmd << " -> " << reply.content;
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

void
CommandHandler::manualClose(std::string const& params, std::string& retStr)
{
    if (mApp.manualClose())
    {
        retStr = "Forcing ledger to close...";
    }
    else
    {
        retStr =
            "Set MANUAL_CLOSE=true in the stellar-core.cfg if you want this "
            "behavior";
    }
}

template <typename T>
optional<T>
maybeParseParam(std::map<std::string, std::string> const& map,
                std::string const& key, T& defaultVal)
{
    auto i = map.find(key);
    if (i != map.end())
    {
        std::stringstream str(i->second);
        str >> defaultVal;

        // Throw an error if not all bytes were loaded into `val`
        if (str.fail() || !str.eof())
        {
            std::string errorMsg =
                fmt::format("Failed to parse '{}' argument", key);
            throw std::runtime_error(errorMsg);
        }
        return make_optional<T>(defaultVal);
    }

    return nullopt<T>();
}

template <typename T>
T
parseParam(std::map<std::string, std::string> const& map,
           std::string const& key)
{
    T val;
    auto res = maybeParseParam(map, key, val);
    if (!res)
    {
        std::string errorMsg = fmt::format("'{}' argument is required!", key);
        throw std::runtime_error(errorMsg);
    }
    return val;
}

void
CommandHandler::peers(std::string const&, std::string& retStr)
{
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
                peerNode["ver"] = peer.second->getRemoteVersion();
                peerNode["olver"] = (int)peer.second->getRemoteOverlayVersion();
                peerNode["id"] = mApp.getConfig().toStrKey(peer.first);
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
    retStr = mApp.getJsonInfo().toStyledString();
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    mApp.syncAllMetrics();
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
}

void
CommandHandler::logRotate(std::string const& params, std::string& retStr)
{
    retStr = "Log rotate...";

    Logging::rotate();
}

void
CommandHandler::connect(std::string const& params, std::string& retStr)
{
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
        p.mUpgradeTime = VirtualClock::tmToPoint(tm);

        uint32 baseFee;
        uint32 baseReserve;
        uint32 maxTxSize;
        uint32 protocolVersion;

        p.mBaseFee = maybeParseParam(retMap, "basefee", baseFee);
        p.mBaseReserve = maybeParseParam(retMap, "basereserve", baseReserve);
        p.mMaxTxSize = maybeParseParam(retMap, "maxtxsize", maxTxSize);
        p.mProtocolVersion =
            maybeParseParam(retMap, "protocolversion", protocolVersion);

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
CommandHandler::quorum(std::string const& params, std::string& retStr)
{
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

    auto root = mApp.getHerder().getJsonQuorumInfo(
        n, retMap["compact"] == "true", retMap["fullkeys"] == "true");
    retStr = root.toStyledString();
}

void
CommandHandler::scpInfo(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    size_t lim = 2;
    maybeParseParam(retMap, "limit", lim);

    auto root = mApp.getHerder().getJsonInfo(lim, retMap["fullkeys"] == "true");
    retStr = root.toStyledString();
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(std::string const& params, std::string& retStr)
{
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    std::string levelStr = retMap["level"];
    std::string partition = retMap["partition"];
    if (!levelStr.size())
    {
        root["Fs"] = Logging::getStringFromLL(Logging::getLogLevel("Fs"));
        root["SCP"] = Logging::getStringFromLL(Logging::getLogLevel("SCP"));
        root["Bucket"] =
            Logging::getStringFromLL(Logging::getLogLevel("Bucket"));
        root["Database"] =
            Logging::getStringFromLL(Logging::getLogLevel("Database"));
        root["History"] =
            Logging::getStringFromLL(Logging::getLogLevel("History"));
        root["Process"] =
            Logging::getStringFromLL(Logging::getLogLevel("Process"));
        root["Ledger"] =
            Logging::getStringFromLL(Logging::getLogLevel("Ledger"));
        root["Overlay"] =
            Logging::getStringFromLL(Logging::getLogLevel("Overlay"));
        root["Herder"] =
            Logging::getStringFromLL(Logging::getLogLevel("Herder"));
        root["Tx"] = Logging::getStringFromLL(Logging::getLogLevel("Tx"));
    }
    else
    {
        el::Level level = Logging::getLLfromString(levelStr);
        if (partition.size())
        {
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
    std::ostringstream output;

    const std::string prefix("?blob=");
    if (params.compare(0, prefix.size(), prefix) == 0)
    {
        TransactionEnvelope envelope;
        std::string blob = params.substr(prefix.size());
        std::vector<uint8_t> binBlob;
        decoder::decode_b64(blob, binBlob);

        xdr::xdr_from_opaque(binBlob, envelope);
        TransactionFramePtr transaction =
            TransactionFrame::makeTransactionFromWire(mApp.getNetworkID(),
                                                      envelope);
        if (transaction)
        {
            // add it to our current set
            // and make sure it is valid
            TransactionQueue::AddResult status =
                mApp.getHerder().recvTransaction(transaction);

            if (status == TransactionQueue::AddResult::STATUS_PENDING)
            {
                StellarMessage msg;
                msg.type(TRANSACTION);
                msg.transaction() = envelope;
                mApp.getOverlayManager().broadcastMessage(msg);
            }

            output << "{"
                   << "\"status\": "
                   << "\"" << TX_STATUS_STRING[static_cast<int>(status)]
                   << "\"";
            if (status == TransactionQueue::AddResult::STATUS_ERROR)
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
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    uint32 cursor = parseParam<uint32>(map, "cursor");

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
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    if (map["queue"] == "true")
    {
        uint32_t count = 50000;
        maybeParseParam(map, "count", count);

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
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);

    std::string domain;
    maybeParseParam(map, "domain", domain);

    mApp.clearMetrics(domain);

    retStr = fmt::format("Cleared {} metrics!", domain);
}

#ifdef BUILD_TESTS
void
CommandHandler::generateLoad(std::string const& params, std::string& retStr)
{
    if (mApp.getConfig().ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING)
    {
        uint32_t nAccounts = 1000;
        uint32_t nTxs = 0;
        uint32_t txRate = 10;
        uint32_t batchSize = 100; // Only for account creations
        uint32_t offset = 0;
        std::string mode = "create";

        std::map<std::string, std::string> map;
        http::server::server::parseParams(params, map);

        bool isCreate;
        maybeParseParam<std::string>(map, "mode", mode);
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

        maybeParseParam(map, "accounts", nAccounts);
        maybeParseParam(map, "txs", nTxs);
        maybeParseParam(map, "batchsize", batchSize);
        maybeParseParam(map, "offset", offset);
        maybeParseParam(map, "txrate", txRate);

        uint32_t numItems = isCreate ? nAccounts : nTxs;
        std::string itemType = isCreate ? "accounts" : "txs";
        double hours = (numItems / txRate) / 3600.0;

        if (batchSize > 100)
        {
            batchSize = 100;
            retStr = "Setting batch size to its limit of 100.";
        }
        mApp.generateLoad(isCreate, nAccounts, offset, nTxs, txRate, batchSize);
        retStr +=
            fmt::format(" Generating load: {:d} {:s}, {:d} tx/s = {:f} hours",
                        numItems, itemType, txRate, hours);
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

        switch (mApp.getHerder().recvTransaction(txFrame))
        {
        case TransactionQueue::AddResult::STATUS_PENDING:
            root["status"] = "pending";
            break;
        case TransactionQueue::AddResult::STATUS_DUPLICATE:
            root["status"] = "duplicate";
            break;
        case TransactionQueue::AddResult::STATUS_ERROR:
            root["status"] = "error";
            root["detail"] =
                xdr::xdr_to_string(txFrame->getResult().result.code());
            break;
        default:
            assert(false);
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
