// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Base58.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "main/Application.h"
#include "main/CommandHandler.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/make_unique.h"

#include "medida/reporting/json_reporter.h"
#include "xdrpp/marshal.h"

#include <regex>

using std::placeholders::_1;
using std::placeholders::_2;

namespace stellar
{
CommandHandler::CommandHandler(Application& app) : mApp(app)
{
    if (!mApp.getConfig().RUN_STANDALONE && mApp.getConfig().HTTP_PORT)
    {
        std::string ipStr;
        if(mApp.getConfig().PUBLIC_HTTP_PORT)
        { 
            ipStr = "0.0.0.0";
        }else ipStr = "127.0.0.1";
        LOG(INFO) << "Listening on " << ipStr << ":"
                  << mApp.getConfig().HTTP_PORT << " for HTTP requests";

        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService(), ipStr, mApp.getConfig().HTTP_PORT);
    }
    else
    {
        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService());
    }

    mServer->add404(std::bind(&CommandHandler::fileNotFound, this, _1, _2));

    mServer->addRoute("connect",
                      std::bind(&CommandHandler::connect, this, _1, _2));
    mServer->addRoute("info", std::bind(&CommandHandler::info, this, _1, _2));
    mServer->addRoute("ll", std::bind(&CommandHandler::ll, this, _1, _2));
    mServer->addRoute("logrotate",
                      std::bind(&CommandHandler::logRotate, this, _1, _2));
    mServer->addRoute("manualclose",
                      std::bind(&CommandHandler::manualClose, this, _1, _2));
    mServer->addRoute("metrics",
                      std::bind(&CommandHandler::metrics, this, _1, _2));
    mServer->addRoute("peers", std::bind(&CommandHandler::peers, this, _1, _2));
    mServer->addRoute("scp", std::bind(&CommandHandler::scpInfo, this, _1, _2));
    mServer->addRoute("stop", std::bind(&CommandHandler::stop, this, _1, _2));
    mServer->addRoute("tx", std::bind(&CommandHandler::tx, this, _1, _2));
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
    retStr += "supported commands:<p/>";

    retStr +=
        "<p><h1> /connect?peer=NAME&port=NNN</h1>"
        "triggers the instance to connect to peer NAME at port NNN."
        "</p><p><h1> /help</h1>"
        "give a list of currently supported commands"
        "</p><p><h1> /info</h1>"
        "returns information about the server in JSON format (sync state, "
        "connected peers, etc)"
        "</p><p><h1> /ll?level=L[&partition=P]</h1>"
        "adjust the log level for partition P (or all if no partition is "
        "specified).<br>"
        "level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE"
        "</p><p><h1> /logrotate</h1>"
        "rotate log files"
        "</p><p><h1> /manualclose</h1>"
        "close the current ledger; must be used with MANUAL_CLOSE set to true"
        "</p><p><h1> /metrics</h1>"
        "returns a snapshot of the metrics registry (for monitoring and "
        "debugging purpose)"
        "</p><p><h1> /peers</h1>"
        "returns the list of known peers in JSON format"
        "</p><p><h1> /scp</h1>"
        "returns a JSON object with the internal state of the SCP engine"
        "</p><p><h1> /stop</h1>"
        "stops the instance"
        "</p><p><h1> /tx?blob=HEX</h1>"
        "submit a transaction to the network.<br>"
        "blob is a hex encoded XDR serialized 'TransactionEnvelope'<br>"
        "returns a JSON object<br>"
        "wasReceived: boolean, true if transaction was queued properly<br>"
        "result: hex encoded, XDR serialized 'TransactionResult'<br>"
        "</p>"

        "<br>";

    retStr += "<p>Have fun!</p>";
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

void
CommandHandler::stop(std::string const& params, std::string& retStr)
{
    retStr = "Stopping...";
    mApp.gracefulStop();
}

void
CommandHandler::peers(std::string const& params, std::string& retStr)
{
    Json::Value root;

    root["peers"];
    int counter = 0;
    for (auto peer : mApp.getOverlayManager().getPeers())
    {
        binToHex(peer->getPeerID());
        root["peers"][counter]["ip"] = peer->getIP();
        root["peers"][counter]["port"] = (int)peer->getRemoteListeningPort();
        root["peers"][counter]["ver"] = peer->getRemoteVersion();
        root["peers"][counter]["pver"] = (int)peer->getRemoteProtocolVersion();
        root["peers"][counter]["id"] =
            toBase58Check(VER_ACCOUNT_ID, peer->getPeerID());

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::info(std::string const& params, std::string& retStr)
{

    std::string stateStrTable[] = {"Booting", "Synced", "Catching up"};
    Json::Value root;

    LedgerManager& lm = mApp.getLedgerManager();

    root["info"]["state"] = stateStrTable[mApp.getState()];
    root["info"]["ledger"]["num"] = (int)lm.getLedgerNum();
    root["info"]["ledger"]["hash"] =
        binToHex(lm.getLastClosedLedgerHeader().hash);
    root["info"]["ledger"]["closeTime"] =
        (int)lm.getLastClosedLedgerHeader().header.closeTime;
    root["info"]["ledger"]["age"] = (int)lm.secondsSinceLastLedgerClose();
    root["info"]["numPeers"] = (int)mApp.getOverlayManager().getPeers().size();

    retStr = root.toStyledString();
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
}

void
CommandHandler::logRotate(std::string const& params, std::string& retStr)
{
    retStr = "Log rotate...";
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
        mApp.getOverlayManager().connectTo(str.str());
    }
    else
    {
        retStr = "Must specify a peer and port: connect&peer=PEER&port=PORT";
    }
}

void
CommandHandler::scpInfo(std::string const& params, std::string& retStr)
{
    Json::Value root;

    mApp.getHerder().dumpInfo(root);

    retStr = root.toStyledString();
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    el::Level level = Logging::getLLfromString(retMap["level"]);
    std::string partition = retMap["partition"];
    if (partition.size())
    {
        Logging::setLogLevel(level, partition.c_str());
        retStr = partition;
        retStr += " set to ";
        retStr += Logging::getStringFromLL(level);
    }
    else
    {
        Logging::setLogLevel(level, nullptr);
        retStr = "Global Log level set to: ";
        retStr += Logging::getStringFromLL(level);
    }
}

void
CommandHandler::tx(std::string const& params, std::string& retStr)
{
    std::ostringstream output;

    const std::string prefix("?blob=");
    if (params.compare(0, prefix.size(), prefix) == 0)
    {
        TransactionEnvelope envelope;
        try
        {
            std::string blob = params.substr(prefix.size());
            std::vector<uint8_t> binBlob = hexToBin(blob);

            xdr::xdr_from_opaque(binBlob, envelope);
            TransactionFramePtr transaction =
                TransactionFrame::makeTransactionFromWire(envelope);
            if (transaction)
            {
                // add it to our current set
                // and make sure it is valid
                bool wasReceived =
                    mApp.getHerder().recvTransaction(transaction);

                if (wasReceived)
                {
                    StellarMessage msg;
                    msg.type(TRANSACTION);
                    msg.transaction() = envelope;
                    mApp.getOverlayManager().broadcastMessage(msg);
                }

                std::string resultHex =
                    binToHex(xdr::xdr_to_opaque(transaction->getResult()));

                output << "{"

                       << "\"wasReceived\": "
                       << (wasReceived ? "true" : "false") << ","

                       << "\"result\": \"" << resultHex << "\""

                       << "}";
            }
        }
        catch (std::exception& e)
        {
            output << "{\"exception\": \"" << e.what() << "\"}";
        }
        catch (...)
        {
            output << "{\"exception\": \"generic\"}";
        }
    }
    else
    {
        output << "{\"exception\": \"Must specify a tx blob: tx?blob=<tx in "
                  "xdr format>\"}";
    }

    retStr = output.str();
}
}
