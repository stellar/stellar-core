#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/http/server.hpp"
#include "lib/httpthreaded/server.hpp"
#include "util/ProtocolVersion.h"
#include <map>
#include <string>

/*
handler functions for the http commands this server supports
*/

namespace stellar
{
class Application;
class SearchableBucketListSnapshot;

class CommandHandler
{
    typedef std::function<void(CommandHandler*, std::string const&,
                               std::string&)>
        HandlerRoute;

    // RPC supports post requests
    typedef std::function<void(CommandHandler*, std::string const&,
                               std::string const&, std::string&)>
        HandlerRouteRPC;

    Application& mApp;
    std::unique_ptr<http::server::server> mServer;
    std::unique_ptr<httpThreaded::server::server> mRpcServer;

    std::map<std::thread::id, std::shared_ptr<SearchableBucketListSnapshot>>
        mBucketListSnapshots;

    void addRoute(std::string const& name, HandlerRoute route);
    void addRPCRoute(std::string const& name, HandlerRouteRPC route);

    void safeRouter(HandlerRoute route, std::string const& params,
                    std::string& retStr);
    void safeRouterRPC(HandlerRouteRPC route, std::string const& params,
                       std::string const& body, std::string& retStr);

    void ensureProtocolVersion(std::map<std::string, std::string> const& args,
                               std::string const& argName,
                               ProtocolVersion minVer);
    void ensureProtocolVersion(std::string const& errString,
                               ProtocolVersion minVer);

    void
    initializeBucketListSnapshots(std::vector<std::thread::id> const& pids);

  public:
    CommandHandler(Application& app);

    std::string manualCmd(std::string const& cmd);

    void fileNotFound(std::string const& params, std::string& retStr);

    void fileNotFoundRPC(std::string const& params, std::string const& body,
                         std::string& retStr);

    void bans(std::string const& params, std::string& retStr);
    void connect(std::string const& params, std::string& retStr);
    void dropcursor(std::string const& params, std::string& retStr);
    void dropPeer(std::string const& params, std::string& retStr);
    void info(std::string const& params, std::string& retStr);
    void ll(std::string const& params, std::string& retStr);
    void logRotate(std::string const& params, std::string& retStr);
    void maintenance(std::string const& params, std::string& retStr);
    void manualClose(std::string const& params, std::string& retStr);
    void metrics(std::string const& params, std::string& retStr);
    void clearMetrics(std::string const& params, std::string& retStr);
    void peers(std::string const& params, std::string& retStr);
    void selfCheck(std::string const&, std::string& retStr);
    void quorum(std::string const& params, std::string& retStr);
    void setcursor(std::string const& params, std::string& retStr);
    void getcursor(std::string const& params, std::string& retStr);
    void scpInfo(std::string const& params, std::string& retStr);
    void tx(std::string const& params, std::string& retStr);
    void getLedgerEntry(std::string const& params, std::string& retStr);

    void getLedgerEntryInternal(std::string const& params,
                                std::string const& body, std::string& retStr);
    void getLedgerEntryBatch(std::string const& params, std::string const& body,
                             std::string& retStr);
    void unban(std::string const& params, std::string& retStr);
    void upgrades(std::string const& params, std::string& retStr);
    void dumpProposedSettings(std::string const& params, std::string& retStr);
    void surveyTopology(std::string const&, std::string& retStr);
    void stopSurvey(std::string const&, std::string& retStr);
    void getSurveyResult(std::string const&, std::string& retStr);
    void sorobanInfo(std::string const&, std::string& retStr);
    void startSurveyCollecting(std::string const& params, std::string& retStr);
    void stopSurveyCollecting(std::string const& params, std::string& retStr);
    void surveyTopologyTimeSliced(std::string const& params,
                                  std::string& retStr);

    // Checks if stellar-core is booted and throws an exception if not.
    void checkBooted() const;

#ifdef BUILD_TESTS
    void generateLoad(std::string const& params, std::string& retStr);
    void testAcc(std::string const& params, std::string& retStr);
    void testTx(std::string const& params, std::string& retStr);
#endif
};
}
