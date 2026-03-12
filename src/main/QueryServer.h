// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "lib/httpthreaded/server.hpp"

#include "ledger/LedgerStateSnapshot.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace stellar
{
class SearchableLiveBucketListSnapshot;
class AppConnector;

class QueryServer
{
  private:
    using HandlerRoute = std::function<bool(QueryServer*, std::string const&,
                                            std::string const&, std::string&)>;

    httpThreaded::server::server mServer;

    std::unordered_map<std::thread::id, LedgerStateSnapshot> mSnapshots;

    AppConnector& mAppConnector;

    std::atomic<bool> mIsReady{false};

    bool safeRouter(HandlerRoute route, std::string const& params,
                    std::string const& body, std::string& retStr);

    bool notFound(std::string const& params, std::string const& body,
                  std::string& retStr);

    void addRoute(std::string const& name, HandlerRoute route);

#ifdef BUILD_TESTS
  public:
#endif
    // Returns raw LedgerKeys for the given keys from the Live BucketList. Does
    // not query other BucketLists or reason about archival.
    bool getLedgerEntryRaw(std::string const& params, std::string const& body,
                           std::string& retStr);

    bool getLedgerEntry(std::string const& params, std::string const& body,
                        std::string& retStr);

  public:
    QueryServer(std::string const& address, unsigned short port, int maxClient,
                size_t threadPoolSize, AppConnector& appConnector
#ifdef BUILD_TESTS
                ,
                bool useMainThreadForTesting = false
#endif
    );

    void shutdown();

    // Called by CommandHandler::setReady() to unblock query endpoints.
    void setReady();
};
}
