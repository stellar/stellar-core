#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/httpthreaded/server.hpp"
#include "main/Config.h"

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace stellar
{
class SearchableLiveBucketListSnapshot;
class SearchableHotArchiveBucketListSnapshot;
class BucketSnapshotManager;

class QueryServer
{
  private:
    using HandlerRoute = std::function<bool(QueryServer*, std::string const&,
                                            std::string const&, std::string&)>;

    httpThreaded::server::server mServer;

    std::unordered_map<std::thread::id,
                       std::shared_ptr<SearchableLiveBucketListSnapshot>>
        mBucketListSnapshots;
    std::unordered_map<std::thread::id,
                       std::shared_ptr<SearchableHotArchiveBucketListSnapshot>>
        mHotArchiveBucketListSnapshots;

#ifdef BUILD_TESTS
    bool const mRequireProofsForAllEvictedEntries;
    bool const mSimulateFilterMiss;
#endif

    bool safeRouter(HandlerRoute route, std::string const& params,
                    std::string const& body, std::string& retStr);

    bool notFound(std::string const& params, std::string const& body,
                  std::string& retStr);

    void addRoute(std::string const& name, HandlerRoute route);

    // Returns raw LedgerEntries for the given keys from the Live BucketList.
    // Does not query other BucketLists or reason about archival.
    bool getLedgerEntryRaw(std::string const& params, std::string const& body,
                           std::string& retStr);

    // Returns LedgerEntries for the given keys in addition to archival state.
    // This function may query BucketLists in addition to the Live BucketList,
    // query archival filters, etc. to provide complete information about the
    // given LedgerKey.
    bool getLedgerEntry(std::string const& params, std::string const& body,
                        std::string& retStr);

  public:
    QueryServer(const std::string& address, unsigned short port, int maxClient,
                size_t threadPoolSize,
                BucketSnapshotManager& bucketSnapshotManager
#ifdef BUILD_TESTS
                ,
                Config const& cfg
#endif
    );
};
}