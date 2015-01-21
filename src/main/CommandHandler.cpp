// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/CommandHandler.h"
#include "main/Application.h"
#include "main/Config.h"
#include "lib/http/server.hpp"
#include "util/Logging.h"
#include "util/make_unique.h"
#include "medida/reporting/json_reporter.h"
#include "overlay/PeerMaster.h"

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
        LOG(INFO) << "Listening on " << ipStr << ":" << mApp.getConfig().HTTP_PORT
                  << " for HTTP requests";

        mServer = stellar::make_unique<http::server::server>(
            app.getMainIOService(), ipStr, mApp.getConfig().HTTP_PORT);

        mServer->addRoute("stop",
                          std::bind(&CommandHandler::stop, this, _1, _2));
        mServer->addRoute("peers",
                          std::bind(&CommandHandler::peers, this, _1, _2));
        mServer->addRoute("info",
                          std::bind(&CommandHandler::info, this, _1, _2));
        mServer->addRoute("metrics",
                          std::bind(&CommandHandler::metrics, this, _1, _2));
        mServer->addRoute("reload_cfg",
                          std::bind(&CommandHandler::reloadCfg, this, _1, _2));
        mServer->addRoute("logrotate",
                          std::bind(&CommandHandler::logRotate, this, _1, _2));
        mServer->addRoute("connect",
                          std::bind(&CommandHandler::connect, this, _1, _2));
        mServer->addRoute("tx", std::bind(&CommandHandler::tx, this, _1, _2));
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
    retStr = "Peers...";
}
void
CommandHandler::info(const std::string& params, std::string& retStr)
{
    retStr = "Info...";
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
        retStr = "Must specify a filename reload_cfg&file=????";
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
    std::string addr = params.substr(6);
    if(addr.size())
    {
        retStr = "Connect to";
        mApp.getPeerMaster().connectTo(addr);
    } else
    {
        retStr = "Must specify a filename connect&peer=????";
    }
}

void
CommandHandler::tx(const std::string& params, std::string& retStr)
{
    retStr = "Submitting Transaction...";
    // TODO.2 
    std::string addr = params.substr(6);
    if(addr.size())
    {
        retStr = "Connect to";
        mApp.getPeerMaster().connectTo(addr);
    } else
    {
        retStr = "Must specify a filename connect&peer=????";
    }
}
}
