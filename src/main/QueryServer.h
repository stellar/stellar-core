// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "lib/httpthreaded/server.hpp"

#include "ledger/ImmutableLedgerView.h"
#include "util/ThreadAnnotations.h"
#include <atomic>
#include <functional>
#include <map>
#include <optional>
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

    // Per-thread cache of ImmutableLedgerView objects. Each thread owns its
    // cache exclusively, so no synchronization is needed for access. Entries
    // are created lazily from mStates and garbage-collected when no longer
    // present in the shared map.
    std::unordered_map<std::thread::id, std::map<uint32_t, ImmutableLedgerView>>
        mPerThreadSnapshots;

    AppConnector& mAppConnector;

    std::atomic<bool> mIsReady{false};

    // Ledger states for query lookups, containing both the current and recent
    // historical states. Protected by a shared mutex so that worker threads
    // can read concurrently while the main thread writes on ledger close.
    mutable ANNOTATED_SHARED_MUTEX(mMutex);
    std::map<uint32_t, ImmutableLedgerDataPtr> mStates GUARDED_BY(mMutex);
    uint32_t const mMaxSnapshots;

    // Returns a cached ImmutableLedgerView for the given ledger seq, or
    // the latest available snapshot if ledgerSeq is nullopt. The pointer
    // is into the per-thread cache and remains valid until the next call
    // to getSnapshotForLedger on the same thread. Returns nullptr if no
    // snapshot is found.
    ImmutableLedgerView*
    getSnapshotForLedger(std::optional<uint32_t> ledgerSeq);

    bool safeRouter(HandlerRoute route, std::string const& params,
                    std::string const& body, std::string& retStr);

    bool notFound(std::string const& params, std::string const& body,
                  std::string& retStr);

    void addRoute(std::string const& name, HandlerRoute route);

#ifdef BUILD_TESTS
  public:
    // Register the calling thread for per-thread snapshot caching. Must be
    // called before any query methods are called from that thread.
    void
    registerThread()
    {
        SharedLockExclusive guard(mMutex);
        mPerThreadSnapshots[std::this_thread::get_id()];
    }

    ImmutableLedgerView*
    getSnapshotForLedgerForTesting(std::optional<uint32_t> ledgerSeq)
    {
        return getSnapshotForLedger(ledgerSeq);
    }

    // Clear all snapshot state. Used between newDB() and start() in tests
    // to avoid the duplicate-seq assertion when both paths push the same LCL.
    void
    resetForTesting()
    {
        SharedLockExclusive guard(mMutex);
        mStates.clear();
        for (auto& [tid, cache] : mPerThreadSnapshots)
        {
            cache.clear();
        }
    }
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

    // Called from main thread when a new ledger state is published. The state
    // is added to the snapshot map so query workers can serve current and
    // historical ledger lookups.
    void addSnapshot(ImmutableLedgerDataPtr state);
};
}
