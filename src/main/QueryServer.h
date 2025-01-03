#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/httpthreaded/server.hpp"

#include "bucket/BucketSnapshotManager.h"
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace stellar
{
class SearchableLiveBucketListSnapshot;
class BucketSnapshotManager;

class QueryServer
{
  private:
    using HandlerRoute = std::function<bool(QueryServer*, std::string const&,
                                            std::string const&, std::string&)>;

    httpThreaded::server::server mServer;

    std::unordered_map<std::thread::id, SearchableSnapshotConstPtr>
        mBucketListSnapshots;

    BucketSnapshotManager& mBucketSnapshotManager;

    bool safeRouter(HandlerRoute route, std::string const& params,
                    std::string const& body, std::string& retStr);

    bool notFound(std::string const& params, std::string const& body,
                  std::string& retStr);

    void addRoute(std::string const& name, HandlerRoute route);

    // Returns raw LedgerKeys for the given keys from the Live BucketList. Does
    // not query other BucketLists or reason about archival.
    bool getLedgerEntryRaw(std::string const& params, std::string const& body,
                           std::string& retStr);

  public:
    QueryServer(const std::string& address, unsigned short port, int maxClient,
                size_t threadPoolSize,
                BucketSnapshotManager& bucketSnapshotManager);
};
}