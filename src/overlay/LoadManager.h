#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "overlay/Peer.h"
#include "util/HashOfHash.h"
#include "util/lrucache.hpp"
#include "xdr/Stellar-types.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"

#include "util/Timer.h"

namespace stellar
{

class Application;

class LoadManager
{
    // This class monitors system load, and attempts to assign blame for
    // the origin of load to particular peers, including the transactions
    // we inject ourselves.
    //
    // The purpose is ultimately to offer a diagnostic view of the peer
    // when and if it's overloaded, as well as to support an automatic
    // load-shedding action of disconnecting the "worst" peers and/or
    // rejecting traffic due to load.
    //
    // This is all very heuristic and speculative; if it turns out not to
    // work, or to do more harm than good, it ought to be disabled/removed.

  public:
    LoadManager();
    ~LoadManager();
    void reportLoads(std::map<NodeID, Peer::pointer> const& peers,
                     Application& app);

    // We track the costs incurred by each peer in a PeerCosts structure,
    // and keep these in an LRU cache to avoid overfilling the LoadManager
    // should we have ongoing churn in low-cost peers.
    struct PeerCosts
    {
        PeerCosts();
        bool isLessThan(std::shared_ptr<PeerCosts> other);
        medida::Meter mTimeSpent;
        medida::Meter mBytesSend;
        medida::Meter mBytesRecv;
        medida::Meter mSQLQueries;
    };

    std::shared_ptr<PeerCosts> getPeerCosts(NodeID const& peer);

  private:
    cache::lru_cache<NodeID, std::shared_ptr<PeerCosts>> mPeerCosts;

  public:
    // Measure recent load on the system and, if the system appears
    // overloaded, shed one or more of the worst-behaved peers,
    // according to our local per-peer accounting.
    void maybeShedExcessLoad(Application& app);

    // Context manager for doing work on behalf of a node, we push
    // one of these on the stack. When destroyed it will debit the
    // peer in question with the cost.
    class PeerContext
    {
        Application& mApp;
        NodeID mNode;

        VirtualClock::time_point mWorkStart;
        std::uint64_t mBytesSendStart;
        std::uint64_t mBytesRecvStart;
        std::uint64_t mSQLQueriesStart;

      public:
        PeerContext(Application& app, NodeID const& node);
        ~PeerContext();
    };
};
}
