// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
        ipStr = "127.0.0.1";
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

    mServer->addRoute("stop", std::bind(&CommandHandler::stop, this, _1, _2));
    mServer->addRoute("peers", std::bind(&CommandHandler::peers, this, _1, _2));
    mServer->addRoute("info", std::bind(&CommandHandler::info, this, _1, _2));
    mServer->addRoute("metrics",
                      std::bind(&CommandHandler::metrics, this, _1, _2));
    mServer->addRoute("reload_cfg",
                      std::bind(&CommandHandler::reloadCfg, this, _1, _2));
    mServer->addRoute("logrotate",
                      std::bind(&CommandHandler::logRotate, this, _1, _2));
    mServer->addRoute("connect",
                      std::bind(&CommandHandler::connect, this, _1, _2));
    mServer->addRoute("tx", std::bind(&CommandHandler::tx, this, _1, _2));
    mServer->addRoute("ll", std::bind(&CommandHandler::ll, this, _1, _2));
}

void
CommandHandler::manualCmd(const std::string& cmd)
{
    http::server::reply reply;
    http::server::request request;
    request.uri = cmd;
    mServer->handle_request(request, reply);
    LOG(INFO) << cmd << " -> " << reply.content;
}

void
CommandHandler::fileNotFound(const std::string& params, std::string& retStr)
{
    retStr = "<b>Welcome to stellar-core!</b><p>";
    retStr += "supported commands:  <p><ul>";
    retStr += "<li>/stop</li>";
    retStr += "<li><a href='/peers'>/peers</a> see list of peers we are "
              "connected to.</li>";
    retStr += "<li><a href='/info'>/info</a></li>";
    retStr += "<li><a href='/metrics'>/metrics</a></li>";
    retStr += "<li><a href='/manualClose'>/manualClose</a>  if in manual mode "
              "will force the ledger to close.</li>";
    retStr += "<li>/connect?peer=###.###.###.###&port=###  connect to a "
              "particular peer.</li>";
    retStr += "<li>/tx?blob=[tx in xdr] submit a transaction.</li>";
    retStr += "<li>/ll?level=[level]&partition=[name]  set the log level. "
              "partition is optional.</li>";
    retStr += "</ul><p>Have fun!";
}

void
CommandHandler::manualClose(const std::string& params, std::string& retStr)
{
    if (mApp.manualClose())
    {
        retStr = "Forcing ledger to close...";
    }
    else
    {
        retStr = "Set MANUAL_CLOSE=true in the stellar-core.cfg if you want this "
                 "behavior";
    }
}

void
CommandHandler::stop(const std::string& params, std::string& retStr)
{
    retStr = "Stopping...";
    mApp.gracefulStop();
}

void
CommandHandler::peers(const std::string& params, std::string& retStr)
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
CommandHandler::info(const std::string& params, std::string& retStr)
{

    std::string stateStrTable[] = {"Booting", "Connecting", "Connected",
                                   "Catching up", "Synced"};
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
CommandHandler::metrics(const std::string& params, std::string& retStr)
{
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
}

void
CommandHandler::reloadCfg(const std::string& params, std::string& retStr)
{
    std::string filename = params.substr(6);
    if (filename.size())
    {
        retStr = "Loading new Config file";
        // GRAYDON: do we want to call this from some other thread?
        // mApp.mConfig.load(filename);
    }
    else
    {
        retStr = "Must specify a filename: reload_cfg&file=????";
    }
}

void
CommandHandler::logRotate(const std::string& params, std::string& retStr)
{
    retStr = "Log rotate...";
}

void
CommandHandler::connect(const std::string& params, std::string& retStr)
{
    static std::regex re("\\?peer=([[:alnum:].]+)&port=([0-9]+)");
    std::smatch m;

    if (std::regex_search(params, m, re) && !m.empty())
    {
        std::stringstream str;
        str << m[1] << ":" << m[2];
        retStr = "Connect to: ";
        retStr += str.str();
        mApp.getOverlayManager().connectTo(str.str());
    }
    else
    {
        retStr = "Must specify a peer and port: connect&peer=PEER&port=PORT";
    }
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(const std::string& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    el::Level level = Logging::getLLfromString(retMap["level"]);
    std::string partition = retMap["partition"];
    Logging::setLogLevel(level, partition.c_str());
    retStr = "Log level set";
}

void
CommandHandler::tx(const std::string& params, std::string& retStr)
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
